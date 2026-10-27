//! GinaDB Lite 0.7 — ginaread.zig
//! Pure-Zig READ parser for GinaDB storage: WAL frames + 4 KiB slotted-page
//! snapshots. Zero dependencies on the C++ engine — any language can read
//! GinaDB files through this module (or the C ABI it exports).
//!
//! Format mirror of src/codec.hpp + src/pager.cpp (docs/FORMATO_ARCHIVOS.md).
//!
//! Semantics (same as the C++ recovery path):
//!   1. load data.pages (if present) → tables, rows, checkpoint_lsn
//!   2. replay wal.log: skip frames with lsn <= checkpoint_lsn, apply only
//!      COMMITTED transactions; a new tx id discards the abandoned pending one
//!   3. a torn/corrupt WAL tail stops replay (never throws mid-row)

const std = @import("std");

pub const Error = error{
    Corruption,
    Io,
    InvalidArgument,
    OutOfMemory,
};

pub const WAL_MAGIC: u32 = 0x414E_4947; // "GINA" LE
pub const PAGE_MAGIC_HDR: u32 = 0x3550_4E47; // "GNP5" (0.5)
pub const PAGE_MAGIC_HDR6: u32 = 0x3650_4E47; // "GNP6" (0.6)
pub const PAGE_MAGIC_DATA: u32 = 0x4747_5047; // "GPGN"
pub const PAGE_MAGIC_OVF: u32 = 0x564F_4E47; // "GNOV"
pub const PAGE_SIZE: usize = 4096;
pub const FORMAT_VERSION: u8 = 6;
pub const WAL_VERSION_V4: u8 = 4; // 0.4 migration
pub const WAL_VERSION_V5: u8 = 5; // 0.5 migration
pub const MAX_FRAME_BODY: usize = 64 << 20;

// 0.6 — delta.pages (checkpoint incremental)
pub const DELTA_HDR_MAGIC: u32 = 0x4C44_4E47; // "GNDL" LE
pub const DELTA_VERSION: u8 = 6;
pub const PG_DELTA_FLAGS: u16 = 6;
pub const L_ROW: u8 = 0;
pub const L_OVF: u8 = 1;
pub const L_TOMB: u8 = 2;
pub const L_SCHEMA: u8 = 3;
pub const L_DROP: u8 = 4;

pub const Kind = enum(u8) {
    put = 1,
    del = 2,
    commit = 3,
    schema = 4,
    drop = 5,
    base = 6,
    resch = 7, // 0.6: re-schema (índices secundarios)
    _,
};

pub const Value = union(enum) {
    null,
    boolean: bool,
    int: i64,
    float: f64,
    text: []const u8,
    bytes: []const u8,
    array: []const Value,
    object: []const Entry,
    vector: []const f32,

    pub const Entry = struct { key: []const u8, value: Value };
};

pub const Column = struct {
    name: []const u8,
    type_tag: u8,
    nullable: bool,
    fts: bool,
};

pub const TableDef = struct {
    name: []const u8,
    primary_key: []const u8,
    schemaless: bool,
    columns: []const Column,
    indexes: []const []const u8 = &.{}, // 0.6
};

pub const Row = std.StringArrayHashMapUnmanaged(Value);
pub const Table = struct {
    def: TableDef,
    rows: std.StringArrayHashMapUnmanaged(Row),
};

pub const Frame = struct {
    version: u8,
    kind: Kind,
    lsn: u64,
    tx: u64,
    payload: []const u8,
};

// ---------------------------------------------------------------------------
// CRC32 (IEEE, same polynomial as the C++ codec)
pub fn crc32(data: []const u8) u32 {
    var c: u32 = 0xFFFF_FFFF;
    for (data) |b| {
        c ^= b;
        var i: usize = 0;
        while (i < 8) : (i += 1) {
            const mask: u32 = if (c & 1 != 0) 0xEDB8_8320 else 0;
            c = (c >> 1) ^ mask;
        }
    }
    return ~c;
}

// ---------------------------------------------------------------------------
// bounds-checked little-endian cursor
const Cursor = struct {
    data: []const u8,
    pos: usize = 0,

    fn remaining(self: *const Cursor) usize {
        return self.data.len - self.pos;
    }
    fn need(self: *const Cursor, n: usize) Error!void {
        if (self.pos + n > self.data.len) return Error.Corruption;
    }
    fn u8v(self: *Cursor) Error!u8 {
        try self.need(1);
        const v = self.data[self.pos];
        self.pos += 1;
        return v;
    }
    fn u16v(self: *Cursor) Error!u16 {
        try self.need(2);
        const v = std.mem.readInt(u16, self.data[self.pos..][0..2], .little);
        self.pos += 2;
        return v;
    }
    fn u32v(self: *Cursor) Error!u32 {
        try self.need(4);
        const v = std.mem.readInt(u32, self.data[self.pos..][0..4], .little);
        self.pos += 4;
        return v;
    }
    fn u64v(self: *Cursor) Error!u64 {
        try self.need(8);
        const v = std.mem.readInt(u64, self.data[self.pos..][0..8], .little);
        self.pos += 8;
        return v;
    }
    fn bytes(self: *Cursor, n: usize) Error![]const u8 {
        try self.need(n);
        const s = self.data[self.pos .. self.pos + n];
        self.pos += n;
        return s;
    }
    fn str(self: *Cursor) Error![]const u8 {
        const n = try self.u32v();
        return self.bytes(n);
    }
};

pub fn decodeValue(data: []const u8, pos: *usize) Error!Value {
    var cur = Cursor{ .data = data, .pos = pos.* };
    const tag = try cur.u8v();
    const v: Value = switch (tag) {
        0 => .null,
        1 => .{ .boolean = (try cur.u8v()) != 0 },
        2 => .{ .int = @bitCast(try cur.u64v()) },
        3 => blk: {
            const bits = try cur.u64v();
            break :blk .{ .float = @bitCast(bits) };
        },
        4 => .{ .text = try cur.str() },
        5 => .{ .bytes = try cur.bytes(try cur.u32v()) },
        6 => blk: {
            const n = try cur.u32v();
            var list: std.ArrayListUnmanaged(Value) = .empty;
            var i: u32 = 0;
            while (i < n) : (i += 1) {
                try list.append(std.heap.page_allocator, try decodeValue(data, &cur.pos));
            }
            break :blk .{ .array = try list.toOwnedSlice(std.heap.page_allocator) };
        },
        7 => blk: {
            const n = try cur.u32v();
            var list: std.ArrayListUnmanaged(Value.Entry) = .empty;
            var i: u32 = 0;
            while (i < n) : (i += 1) {
                const k = try cur.str();
                const v7 = try decodeValue(data, &cur.pos);
                try list.append(std.heap.page_allocator, .{ .key = k, .value = v7 });
            }
            break :blk .{ .object = try list.toOwnedSlice(std.heap.page_allocator) };
        },
        8 => blk: {
            const n = try cur.u32v();
            const raw = try cur.bytes(@as(usize, n) * 4);
            const out = std.heap.page_allocator.alloc(f32, n) catch return Error.OutOfMemory;
            var i: usize = 0;
            while (i < n) : (i += 1) {
                out[i] = @bitCast(std.mem.readInt(u32, raw[i * 4 ..][0..4], .little));
            }
            break :blk .{ .vector = out };
        },
        else => return Error.Corruption,
    };
    pos.* = cur.pos;
    return v;
}

fn decodeRow(alloc: std.mem.Allocator, data: []const u8, pos: *usize) Error!Row {
    var cur = Cursor{ .data = data, .pos = pos.* };
    const n = try cur.u32v();
    var row: Row = .empty;
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const k = try cur.str();
        const v = try decodeValue(data, &cur.pos);
        row.put(alloc, try alloc.dupe(u8, k), v) catch return Error.Io;
    }
    pos.* = cur.pos;
    return row;
}

pub fn decodeSchema(data: []const u8, pos: *usize, v4: bool) Error!TableDef {
    var cur = Cursor{ .data = data, .pos = pos.* };
    const name = try cur.str();
    const pk = try cur.str();
    const schemaless = if (v4) false else (try cur.u8v()) != 0;
    const n = try cur.u32v();
    const cols = std.heap.page_allocator.alloc(Column, n) catch return Error.Io;
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const cname = try cur.str();
        const t = try cur.u8v();
        const nullable = (try cur.u8v()) != 0;
        const fts = if (v4) false else (try cur.u8v()) != 0;
        if (t > 8) return Error.Corruption;
        cols[i] = .{ .name = cname, .type_tag = t, .nullable = nullable, .fts = fts };
    }
    // 0.6: sección trailing de índices (ausente en payloads v5/v4)
    var indexes: []const []const u8 = &.{};
    if (!v4 and cur.pos < cur.data.len) {
        _ = try cur.u8v(); // marcador de sección
        const ni = try cur.u32v();
        const list = std.heap.page_allocator.alloc([]const u8, ni) catch return Error.Io;
        var j: u32 = 0;
        while (j < ni and cur.pos < cur.data.len) : (j += 1)
            list[j] = try cur.str();
        indexes = list;
    }
    pos.* = cur.pos;
    return .{ .name = name, .primary_key = pk, .schemaless = schemaless, .columns = cols, .indexes = indexes };
}

// ---------------------------------------------------------------------------
// WAL iteration — stops at the first torn/corrupt frame.
pub const WalResult = struct {
    frames: []const Frame, // successfully parsed frames (owned by this result)
    good_bytes: usize, // offset after the last valid frame
    bad_tail_bytes: usize, // trailing bytes discarded (torn/corrupt)

    pub fn deinit(self: *WalResult, alloc: std.mem.Allocator) void {
        alloc.free(@constCast(self.frames));
        self.frames = &.{};
    }

    pub fn parse(alloc: std.mem.Allocator, data: []const u8) Error!WalResult {
        var frames: std.ArrayListUnmanaged(Frame) = .empty;
        var pos: usize = 0;
        var bad = false;
        while (pos < data.len) {
            if (data.len - pos < 12) { // torn header
                bad = true;
                break;
            }
            const magic = std.mem.readInt(u32, data[pos..][0..4], .little);
            const len = std.mem.readInt(u32, data[pos + 4 ..][0..4], .little);
            const crc = std.mem.readInt(u32, data[pos + 8 ..][0..4], .little);
            if (magic != WAL_MAGIC or len > MAX_FRAME_BODY) {
                bad = true;
                break;
            }
            const end = pos + 12 + len;
            if (end > data.len) { // torn body
                bad = true;
                break;
            }
            const body = data[pos + 12 .. end];
            if (crc32(body) != crc) { // crc mismatch
                bad = true;
                break;
            }
            if (body.len < 18) {
                bad = true;
                break;
            }
            const version = body[0];
            if (version != FORMAT_VERSION and version != WAL_VERSION_V4 and
                version != WAL_VERSION_V5)
            {
                bad = true;
                break;
            }
            frames.append(alloc, .{
                .version = version,
                .kind = @enumFromInt(body[1]),
                .lsn = std.mem.readInt(u64, body[2..][0..8], .little),
                .tx = std.mem.readInt(u64, body[10..][0..8], .little),
                .payload = body[18..],
            }) catch return Error.Io;
            pos = end;
        }
        const owned = frames.toOwnedSlice(alloc) catch return Error.OutOfMemory;
        return .{
            .frames = owned,
            .good_bytes = pos,
            .bad_tail_bytes = if (bad) data.len - pos else 0,
        };
    }
};

// ---------------------------------------------------------------------------
// snapshot (data.pages) parsing
const DataPageHeader = struct {
    magic: u32,
    page_id: u32,
    page_lsn: u64,
    slot_count: u16,
    data_end: u16,
    slots_end: u16,
    flags: u16,
    crc: u32,
};

fn parseDataHeader(p: []const u8) DataPageHeader {
    return .{
        .magic = std.mem.readInt(u32, p[0..4], .little),
        .page_id = std.mem.readInt(u32, p[4..8], .little),
        .page_lsn = std.mem.readInt(u64, p[8..16], .little),
        .slot_count = std.mem.readInt(u16, p[16..18], .little),
        .data_end = std.mem.readInt(u16, p[18..20], .little),
        .slots_end = std.mem.readInt(u16, p[20..22], .little),
        .flags = std.mem.readInt(u16, p[22..24], .little),
        .crc = std.mem.readInt(u32, p[24..28], .little),
    };
}

fn keyOf(def: *const TableDef, row: *const Row) Error![]const u8 {
    if (row.get(def.primary_key)) |v| {
        switch (v) {
            .text => |t| return t,
            else => {},
        }
    }
    return Error.Corruption;
}

const SnapshotInfo = struct {
    checkpoint_lsn: u64,
    page_count: u64,
    overflow_pages: u64,
};

fn parseSnapshot(
    db: *Db,
    data: []const u8,
) Error!SnapshotInfo {
    if (data.len < PAGE_SIZE) return Error.Corruption;
    const hmagic = std.mem.readInt(u32, data[0..4], .little);
    if (hmagic != PAGE_MAGIC_HDR and hmagic != PAGE_MAGIC_HDR6)
        return Error.Corruption;
    if (data[4] != FORMAT_VERSION and data[4] != 5) return Error.Corruption;
    const ckpt_lsn = std.mem.readInt(u64, data[8..16], .little);
    const page_count = std.mem.readInt(u64, data[16..24], .little);
    const table_count = std.mem.readInt(u32, data[24..28], .little);
    if (page_count == 0 or data.len < page_count * PAGE_SIZE) return Error.Corruption;

    // overflow chain reader
    const readOverflow = struct {
        fn go(buf: []const u8, pages: u64, first: u64, total: u64, out: *std.ArrayListUnmanaged(u8)) Error!void {
            const alloc = std.heap.page_allocator;
            var page = first;
            var hops: u64 = 0;
            while (page != 0) {
                hops += 1;
                if (page >= pages or hops > pages) return Error.Corruption;
                const p = buf[@as(usize, page) * PAGE_SIZE ..][0..PAGE_SIZE];
                if (std.mem.readInt(u32, p[0..4], .little) != PAGE_MAGIC_OVF)
                    return Error.Corruption;
                const chunk = std.mem.readInt(u32, p[8..12], .little);
                if (24 + chunk > PAGE_SIZE) return Error.Corruption;
                out.appendSlice(alloc, p[24 .. 24 + chunk]) catch return Error.Io;
                page = std.mem.readInt(u64, p[16..24], .little);
            }
            if (out.items.len != total) return Error.Corruption;
        }
    }.go;

    var overflow_pages: u64 = 0;
    // pass 1: catalog pages
    var pid: u64 = 1;
    while (pid < page_count) : (pid += 1) {
        const p = data[@as(usize, pid) * PAGE_SIZE ..][0..PAGE_SIZE];
        const hd = parseDataHeader(p);
        if (hd.magic == PAGE_MAGIC_OVF) continue;
        if (hd.magic != PAGE_MAGIC_DATA) return Error.Corruption;
        if (hd.flags != 2) continue;
        if (hd.crc != crc32(p[32..])) return Error.Corruption;
        var s: u16 = 0;
        while (s < hd.slot_count) : (s += 1) {
            const so = PAGE_SIZE - (@as(usize, s) + 1) * 4;
            const off = std.mem.readInt(u16, p[so..][0..2], .little);
            const len = std.mem.readInt(u16, p[so + 2 ..][0..2], .little);
            if (32 + @as(usize, off) + len > PAGE_SIZE or len < 5)
                return Error.Corruption;
            const rec = p[32 + off ..][0..len];
            var pos: usize = 5; // skip table_id + layout
            const def = try decodeSchema(rec, &pos, false);
            var t: Table = .{ .def = def, .rows = .empty };
            t.def.name = try db.arena.allocator().dupe(u8, def.name);
            t.def.primary_key = try db.arena.allocator().dupe(u8, def.primary_key);
            db.tables.put(db.arena.allocator(), t.def.name, t) catch return Error.Io;
        }
    }
    // count mismatch?
    if (db.tables.count() != table_count) return Error.Corruption;

    // pass 2: data pages
    pid = 1;
    while (pid < page_count) : (pid += 1) {
        const p = data[@as(usize, pid) * PAGE_SIZE ..][0..PAGE_SIZE];
        if (std.mem.readInt(u32, p[0..4], .little) == PAGE_MAGIC_OVF) {
            overflow_pages += 1;
            continue;
        }
        const hd = parseDataHeader(p);
        if (hd.flags != 0) continue;
        if (hd.crc != crc32(p[32..])) return Error.Corruption;
        var s: u16 = 0;
        while (s < hd.slot_count) : (s += 1) {
            const so = PAGE_SIZE - (@as(usize, s) + 1) * 4;
            const off = std.mem.readInt(u16, p[so..][0..2], .little);
            const len = std.mem.readInt(u16, p[so + 2 ..][0..2], .little);
            if (32 + @as(usize, off) + len > PAGE_SIZE or len < 5)
                return Error.Corruption;
            const rec = p[32 + off ..][0..len];
            const table_id = std.mem.readInt(u32, rec[0..4], .little);
            const layout = rec[4];
            var body: []const u8 = undefined;
            var spilled: std.ArrayListUnmanaged(u8) = .empty;
            if (layout == 0) {
                body = rec[5..];
            } else if (layout == 1) {
                if (len != 21) return Error.Corruption;
                const total = std.mem.readInt(u64, rec[5..][0..8], .little);
                const first = std.mem.readInt(u64, rec[13..][0..8], .little);
                try readOverflow(data, page_count, first, total, &spilled);
                body = spilled.items;
            } else return Error.Corruption;
            const table = db.findTableById(table_id) orelse return Error.Corruption;
            var pos: usize = 0;
            var row = try decodeRow(db.arena.allocator(), body, &pos);
            const key = try keyOf(&table.def, &row);
            const kcopy = try db.arena.allocator().dupe(u8, key);
            var tref = db.tables.getPtr(table.def.name).?;
            tref.rows.put(db.arena.allocator(), kcopy, row) catch return Error.Io;
        }
    }
    return .{ .checkpoint_lsn = ckpt_lsn, .page_count = page_count, .overflow_pages = overflow_pages };
}

// ---------------------------------------------------------------------------
// 0.6 — delta.pages: registros versionados desde el último checkpoint completo
// Espejo exacto de src/delta.cpp (derivation newest-wins + free-list derivada).
pub const DeltaInfo = struct {
    seq: u64 = 0,
    watermark: u64 = 0,
    base_lsn: u64 = 0,
    page_count: u64 = 2,
    live_pages: u64 = 0,
    free_pages: u64 = 0,
    suspect_pages: u64 = 0,
    // 0.7 — clasificación: rotas DENTRO del extent comprometido (medio,
    // pérdida real de registros) vs torn de IC sin flip (normal).
    corrupt_pages: u64 = 0,
    broken_ovf_records: u64 = 0,
    records: u64 = 0,
};

const DeltaRec = struct {
    lsn: u64,
    page: u64,
    tid: u32,
    layout: u8,
    key: []const u8,
    name: []const u8,
    row: Row,
    def: ?TableDef,
    ovf_first: u64,
    ovf_total: u64,
};

fn parseDelta(db: *Db, data: []const u8) Error!DeltaInfo {
    if (data.len < 2 * PAGE_SIZE) return Error.Corruption;
    var info = DeltaInfo{};

    const Slot = struct { seq: u64, wm: u64, base: u64, pc: u64 };
    const slotOk = struct {
        fn go(h: []const u8) ?Slot {
            if (std.mem.readInt(u32, h[0..4], .little) != DELTA_HDR_MAGIC) return null;
            if (h[4] != DELTA_VERSION) return null;
            if (std.mem.readInt(u32, h[60..64], .little) != crc32(h[0..60])) return null;
            return .{
                .seq = std.mem.readInt(u64, h[8..16], .little),
                .wm = std.mem.readInt(u64, h[16..24], .little),
                .base = std.mem.readInt(u64, h[24..32], .little),
                .pc = std.mem.readInt(u64, h[32..40], .little),
            };
        }
    }.go;
    const sa = slotOk(data[0..PAGE_SIZE]);
    const sb = slotOk(data[PAGE_SIZE .. 2 * PAGE_SIZE]);
    const use_b = if (sb) |b| (sa == null or b.seq > sa.?.seq) else false;
    const slot = if (use_b) sb else sa;
    if (slot == null) return Error.Corruption; // el protocolo crea ambos slots
    const s = slot.?;
    info.seq = s.seq;
    info.watermark = s.wm;
    info.base_lsn = s.base;
    info.page_count = s.pc;
    if (data.len < s.pc * PAGE_SIZE) return Error.Corruption;

    const a = db.arena.allocator();

    // pase 1: páginas (delta slotted + overflow) — páginas con lsn > watermark
    // son de un IC sin flip (o un hueco reutilizado torn): se ignoran.
    var recs: std.ArrayListUnmanaged(DeltaRec) = .empty;
    const OvfPage = struct { chunk: u32, next: u64, data: []const u8 };
    var ovf = std.AutoArrayHashMapUnmanaged(u64, OvfPage).empty;
    defer ovf.deinit(a);
    var pid: u64 = 2;
    while (pid < s.pc) : (pid += 1) {
        const p = data[@as(usize, pid) * PAGE_SIZE ..][0..PAGE_SIZE];
        if (std.mem.readInt(u32, p[0..4], .little) == PAGE_MAGIC_OVF) {
            const chunk = std.mem.readInt(u32, p[8..12], .little);
            if (24 + chunk > PAGE_SIZE) return Error.Corruption;
            ovf.put(a, pid, .{ .chunk = chunk,
                .next = std.mem.readInt(u64, p[16..24], .little),
                .data = p[24..] }) catch return Error.Io;
            continue;
        }
        const hd = parseDataHeader(p);
        if (hd.magic != PAGE_MAGIC_DATA or hd.flags != PG_DELTA_FLAGS or
            hd.crc != crc32(p[32..]))
        {
            // 0.7: misma clasificación que el motor — un header legible con
            // page_lsn > watermark es un torn de IC sin flip (normal); lo
            // demás, dentro del extent comprometido, es corrupción de medio.
            if (hd.magic == PAGE_MAGIC_DATA and hd.page_lsn > s.wm) {
                info.suspect_pages += 1; // hueco torn / no comprometida
            } else {
                info.corrupt_pages += 1; // comprometida ROTA: pérdida real
            }
            continue;
        }
        if (hd.page_lsn > s.wm) continue; // IC sin flip
        var sl: u16 = 0;
        while (sl < hd.slot_count) : (sl += 1) {
            const so = PAGE_SIZE - (@as(usize, sl) + 1) * 4;
            const off = std.mem.readInt(u16, p[so..][0..2], .little);
            const len = std.mem.readInt(u16, p[so + 2 ..][0..2], .little);
            if (32 + @as(usize, off) + len > PAGE_SIZE or len < 5)
                return Error.Corruption;
            const rec = p[32 + off ..][0..len];
            const tid = std.mem.readInt(u32, rec[0..4], .little);
            const layout = rec[4];
            const body = rec[5..];
            var d = DeltaRec{ .lsn = hd.page_lsn, .page = pid, .tid = tid,
                .layout = layout, .key = "", .name = "", .row = .empty,
                .def = null, .ovf_first = 0, .ovf_total = 0 };
            switch (layout) {
                L_ROW => {
                    var pos: usize = 0;
                    d.row = try decodeRow(a, body, &pos);
                },
                L_OVF => {
                    if (body.len != 16) return Error.Corruption;
                    d.ovf_total = std.mem.readInt(u64, body[0..8], .little);
                    d.ovf_first = std.mem.readInt(u64, body[8..16], .little);
                },
                L_TOMB => {
                    var cur = Cursor{ .data = body, .pos = 0 };
                    d.key = try cur.str();
                },
                L_SCHEMA => {
                    var pos: usize = 0;
                    d.def = try decodeSchema(body, &pos, false);
                    d.name = d.def.?.name;
                },
                L_DROP => {
                    var cur = Cursor{ .data = body, .pos = 0 };
                    d.name = try cur.str();
                },
                else => return Error.Corruption,
            }
            recs.append(a, d) catch return Error.Io;
        }
    }
    info.records = recs.items.len;

    // materializar overflow (necesita el mapa completo de páginas).
    // 0.7: una cadena rota/corrupta NO tumba la lectura: el registro se
    // marca muerto (layout neutro), se cuenta y se sigue — espejo del motor.
    for (recs.items) |*r| {
        if (r.layout != L_OVF) continue;
        var body: std.ArrayListUnmanaged(u8) = .empty;
        var page = r.ovf_first;
        var hops: u64 = 0;
        var broken = false;
        while (page != 0) {
            hops += 1;
            const o = ovf.get(page) orelse {
                broken = true;
                break;
            };
            if (hops > s.pc) {
                broken = true;
                break;
            }
            body.appendSlice(a, o.data[0..o.chunk]) catch return Error.Io;
            page = o.next;
        }
        if (!broken and body.items.len != r.ovf_total) broken = true;
        if (broken) {
            info.broken_ovf_records += 1;
            r.layout = L_TOMB; // neutro: no aporta estado
            r.key = "";
            continue;
        }
        var pos: usize = 0;
        r.row = try decodeRow(a, body.items, &pos);
    }

    // orden lsn (estable por página)
    const less = struct {
        fn f(_: void, x: DeltaRec, y: DeltaRec) bool {
            return x.lsn < y.lsn;
        }
    }.f;
    std.mem.sort(DeltaRec, recs.items, {}, less);

    // pk por tid: defs del propio delta primero (inmutable por encarnación)
    var tid_pk = std.AutoHashMapUnmanaged(u32, []const u8).empty;
    defer tid_pk.deinit(a);
    var tid_name = std.AutoHashMapUnmanaged(u32, []const u8).empty;
    defer tid_name.deinit(a);
    // base: ids 1..N por nombre ordenado (convención del writer)
    for (db.tables.values(), 0..) |*t, i| {
        tid_pk.put(a, @intCast(i + 1), t.def.primary_key) catch return Error.Io;
        tid_name.put(a, @intCast(i + 1), t.def.name) catch return Error.Io;
    }
    for (recs.items) |r| {
        if (r.layout == L_SCHEMA) {
            tid_pk.put(a, r.tid, r.def.?.primary_key) catch return Error.Io;
            tid_name.put(a, r.tid, r.def.?.name) catch return Error.Io;
        }
    }

    // vida: último índice visto por (tid,key) + último estructural por tid
    var last_struct = std.AutoHashMapUnmanaged(u32, usize).empty;
    defer last_struct.deinit(a);
    var last_seen = std.AutoHashMapUnmanaged(u128, usize).empty;
    defer last_seen.deinit(a);
    for (recs.items, 0..) |r, i| {
        if (r.layout == L_SCHEMA or r.layout == L_DROP) {
            try last_struct.put(a, r.tid, i);
        } else {
            const k128 = (@as(u128, r.tid) << 64) |
                std.hash.Wyhash.hash(r.tid, r.key);
            try last_seen.put(a, k128, i); // recorriendo en orden lsn, queda el último
        }
    }
    var dead = std.ArrayListUnmanaged(bool).empty;
    dead.appendNTimes(a, false, recs.items.len) catch return Error.Io;
    defer dead.deinit(a);
    for (recs.items, 0..) |r, i| {
        if (r.layout == L_SCHEMA or r.layout == L_DROP) {
            dead.items[i] = last_struct.get(r.tid).? != i;
            continue;
        }
        const k128 = (@as(u128, r.tid) << 64) |
            std.hash.Wyhash.hash(r.tid, r.key);
        var d = last_seen.get(k128).? != i;
        if (!d) {
            if (last_struct.get(r.tid)) |ls| {
                const st = recs.items[ls];
                if (st.layout == L_DROP and st.lsn > r.lsn) d = true;
            }
        }
        dead.items[i] = d;
    }

    // aplicar en orden lsn: reproduce exactamente el estado del motor
    for (recs.items) |r| {
        switch (r.layout) {
            L_SCHEMA => {
                const def = r.def.?;
                if (db.tables.getPtr(def.name)) |t| {
                    t.def = def;
                } else {
                    const name = try a.dupe(u8, def.name);
                    var t: Table = .{ .def = def, .rows = .empty };
                    t.def.name = name;
                    t.def.primary_key = try a.dupe(u8, def.primary_key);
                    db.tables.put(a, name, t) catch return Error.Io;
                }
            },
            L_DROP => {
                if (tid_name.get(r.tid)) |nm| _ = db.tables.orderedRemove(nm);
            },
            L_ROW, L_OVF => {
                const nm = tid_name.get(r.tid) orelse continue;
                var t = db.tables.getPtr(nm) orelse continue;
                const pk = tid_pk.get(r.tid) orelse t.def.primary_key;
                var row = r.row;
                const key = keyOfPk(pk, &row) orelse continue;
                const kcopy = try a.dupe(u8, key);
                t.rows.put(a, kcopy, row) catch return Error.Io;
            },
            L_TOMB => {
                const nm = tid_name.get(r.tid) orelse continue;
                var t = db.tables.getPtr(nm) orelse continue;
                _ = t.rows.orderedRemove(r.key);
            },
            else => {},
        }
    }

    // páginas vivas = con ≥1 registro vivo (+ cadenas ovf referenciadas)
    var page_live = std.AutoHashMapUnmanaged(u64, void).empty;
    defer page_live.deinit(a);
    for (recs.items, 0..) |r, i| {
        if (dead.items[i]) continue;
        try page_live.put(a, r.page, {});
        if (r.layout == L_OVF) {
            var page = r.ovf_first;
            while (page != 0) {
                try page_live.put(a, page, {});
                page = ovf.get(page).?.next;
            }
        }
    }
    var live: u64 = 0;
    var it = page_live.keyIterator();
    while (it.next()) |_| live += 1;
    info.live_pages = live;
    info.free_pages = if (s.pc > 2) (s.pc - 2) - live - info.suspect_pages else 0;
    return info;
}

fn keyOfPk(pk: []const u8, row: *const Row) ?[]const u8 {
    if (row.get(pk)) |v| switch (v) {
        .text => |t| return t,
        else => {},
    };
    return null;
}

// ---------------------------------------------------------------------------
// read-only database view
pub const Db = struct {
    arena: std.heap.ArenaAllocator,
    tables: std.StringArrayHashMapUnmanaged(Table) = .empty,
    last_lsn: u64 = 0,
    checkpoint_lsn: u64 = 0,
    snapshot_pages: u64 = 0,
    snapshot_overflow: u64 = 0,
    wal_bad_tail: u64 = 0,
    wal_frames: usize = 0,
    delta: DeltaInfo = .{}, // 0.6

    pub fn open(gpa: std.mem.Allocator, dir: []const u8) Error!Db {
        var db = Db{ .arena = std.heap.ArenaAllocator.init(gpa) };
        errdefer db.deinit();
        const a = db.arena.allocator();

        // 1. snapshot
        const data_path = std.fmt.allocPrint(a, "{s}/data.pages", .{dir}) catch return Error.Io;
        if (readFileAlloc(a, data_path)) |snap| {
            const info = try parseSnapshot(&db, snap);
            db.checkpoint_lsn = info.checkpoint_lsn;
            db.snapshot_pages = info.page_count;
            db.snapshot_overflow = info.overflow_pages;
            db.last_lsn = info.checkpoint_lsn;
        } else |e| switch (e) {
            error.FileNotFound => {},
            else => return Error.Io, // unreadable snapshot → surface the error
        }

        // 2. delta incremental (0.6): versiones desde el último checkpoint
        //    completo; el watermark sustituye al del base si es más nuevo.
        const delta_path = std.fmt.allocPrint(a, "{s}/delta.pages", .{dir}) catch return Error.Io;
        if (readFileAlloc(a, delta_path)) |ddata| {
            if (ddata.len >= 2 * PAGE_SIZE) {
                if (parseDelta(&db, ddata)) |dinfo| {
                    db.delta = dinfo;
                    if (dinfo.watermark > db.checkpoint_lsn)
                        db.checkpoint_lsn = dinfo.watermark;
                } else |e| return e;
            }
        } else |e| switch (e) {
            error.FileNotFound => {},
            else => return Error.Io,
        }

        // 3. WAL replay
        const wal_path = std.fmt.allocPrint(a, "{s}/wal.log", .{dir}) catch return Error.Io;
        if (readFileAlloc(a, wal_path)) |wal| {
            const res = try WalResult.parse(a, wal);
            db.wal_bad_tail = res.bad_tail_bytes;
            db.wal_frames = res.frames.len;
            var active_tx: u64 = 0;
            var pending: std.ArrayListUnmanaged(Frame) = .empty;
            for (res.frames) |fr| {
                db.last_lsn = @max(db.last_lsn, fr.lsn);
                if (fr.lsn <= db.checkpoint_lsn) continue;
                switch (fr.kind) {
                    .commit => {
                        if (active_tx != 0 and fr.tx == active_tx) {
                            for (pending.items) |op| try db.applyOp(op);
                            pending.clearRetainingCapacity();
                            active_tx = 0;
                        }
                    },
                    .base => {
                        pending.clearRetainingCapacity();
                        active_tx = 0;
                    },
                    .put, .del, .schema, .drop, .resch => {
                        if (fr.tx != active_tx) {
                            pending.clearRetainingCapacity();
                            active_tx = fr.tx;
                        }
                        pending.append(a, fr) catch return Error.Io;
                    },
                    _ => {},
                }
            }
        } else |e| switch (e) {
            error.FileNotFound => {},
            else => return Error.Io,
        }
        db.last_lsn = @max(db.last_lsn, db.checkpoint_lsn);
        return db;
    }

    pub fn deinit(self: *Db) void {
        self.tables.deinit(self.arena.child_allocator);
        self.arena.deinit();
    }

    pub fn get(self: *const Db, table: []const u8, key: []const u8) ?*const Row {
        const t = self.tables.get(table) orelse return null;
        return t.rows.getPtr(key);
    }

    pub fn count(self: *const Db, table: []const u8) usize {
        const t = self.tables.get(table) orelse return 0;
        return t.rows.count();
    }

    fn findTableById(self: *Db, id: u32) ?*Table {
        // ids are assigned in name-sorted order (1..N) at checkpoint time
        if (id == 0 or id > self.tables.count()) return null;
        return &self.tables.values()[id - 1];
    }

    fn applyOp(self: *Db, fr: Frame) Error!void {
        var cur = Cursor{ .data = fr.payload, .pos = 0 };
        const legacy = fr.version == WAL_VERSION_V4;
        switch (fr.kind) {
            .schema => {
                var pos: usize = 0;
                const def = try decodeSchema(fr.payload, &pos, legacy);
                if (self.tables.contains(def.name)) return;
                const name = try self.arena.allocator().dupe(u8, def.name);
                var t: Table = .{ .def = def, .rows = .empty };
                t.def.name = name;
                t.def.primary_key = try self.arena.allocator().dupe(u8, def.primary_key);
                self.tables.put(self.arena.allocator(), name, t) catch return Error.Io;
            },
            .drop => {
                const name = cur.str() catch return Error.Corruption;
                _ = self.tables.orderedRemove(name);
            },
            .resch => {
                var pos: usize = 0;
                const def = try decodeSchema(fr.payload, &pos, legacy);
                if (self.tables.getPtr(def.name)) |t| {
                    t.def = def;
                } else {
                    const name = try self.arena.allocator().dupe(u8, def.name);
                    var t: Table = .{ .def = def, .rows = .empty };
                    t.def.name = name;
                    t.def.primary_key = try self.arena.allocator().dupe(u8, def.primary_key);
                    self.tables.put(self.arena.allocator(), name, t) catch return Error.Io;
                }
            },
            .put => {
                const name = cur.str() catch return Error.Corruption;
                var t = self.tables.getPtr(name) orelse return; // dropped earlier
                const row = try decodeRow(self.arena.allocator(), fr.payload, &cur.pos);
                const key = try keyOf(&t.def, &row);
                const kcopy = try self.arena.allocator().dupe(u8, key);
                t.rows.put(self.arena.allocator(), kcopy, row) catch return Error.Io;
            },
            .del => {
                const name = cur.str() catch return Error.Corruption;
                const key = cur.str() catch return Error.Corruption;
                var t = self.tables.getPtr(name) orelse return;
                _ = t.rows.orderedRemove(key);
            },
            else => {},
        }
    }
};

fn readFileAlloc(alloc: std.mem.Allocator, path: []const u8) ![]u8 {
    // Zig 0.16 std.Io: synchronous reads go through an Io instance.
    var threaded = std.Io.Threaded.init(alloc, .{});
    defer threaded.deinit();
    const io = threaded.io();
    return std.Io.Dir.cwd().readFileAlloc(io, path, alloc, .limited(512 << 20));
}

// ---------------------------------------------------------------------------
// JSON output (used by the exported C ABI + CLI)
pub fn writeJsonValue(out: *std.ArrayListUnmanaged(u8), alloc: std.mem.Allocator, v: Value) !void {
    switch (v) {
        .null => try out.appendSlice(alloc, "null"),
        .boolean => |b| try out.appendSlice(alloc, if (b) "true" else "false"),
        .int => |i| {
            var buf: [24]u8 = undefined;
            const s = std.fmt.bufPrint(&buf, "{d}", .{i}) catch unreachable;
            try out.appendSlice(alloc, s);
        },
        .float => |f| {
            var buf: [48]u8 = undefined;
            const s = std.fmt.bufPrint(&buf, "{d}", .{f}) catch unreachable;
            try out.appendSlice(alloc, s);
        },
        .text => |t| try writeJsonString(out, alloc, t),
        .bytes => |b| {
            try out.appendSlice(alloc, "{\"$bytes\":\"");
            const enc = std.base64.standard.Encoder;
            const sz = enc.calcSize(b.len);
            const dst = try alloc.alloc(u8, sz);
            _ = enc.encode(dst, b);
            try out.appendSlice(alloc, dst);
            try out.appendSlice(alloc, "\"}");
        },
        .array => |arr| {
            try out.append(alloc, '[');
            for (arr, 0..) |e, i| {
                if (i > 0) try out.append(alloc, ',');
                try writeJsonValue(out, alloc, e);
            }
            try out.append(alloc, ']');
        },
        .object => |obj| {
            try out.append(alloc, '{');
            for (obj, 0..) |e, i| {
                if (i > 0) try out.append(alloc, ',');
                try writeJsonString(out, alloc, e.key);
                try out.append(alloc, ':');
                try writeJsonValue(out, alloc, e.value);
            }
            try out.append(alloc, '}');
        },
        .vector => |vec| {
            try out.appendSlice(alloc, "{\"$vec\":[");
            for (vec, 0..) |f, i| {
                if (i > 0) try out.append(alloc, ',');
                var buf: [48]u8 = undefined;
                const s = std.fmt.bufPrint(&buf, "{d}", .{f}) catch unreachable;
                try out.appendSlice(alloc, s);
            }
            try out.appendSlice(alloc, "]}");
        },
    }
}

pub fn writeJsonString(out: *std.ArrayListUnmanaged(u8), alloc: std.mem.Allocator, s: []const u8) !void {
    try out.append(alloc, '"');
    for (s) |c| {
        switch (c) {
            '"' => try out.appendSlice(alloc, "\\\""),
            '\\' => try out.appendSlice(alloc, "\\\\"),
            '\n' => try out.appendSlice(alloc, "\\n"),
            '\r' => try out.appendSlice(alloc, "\\r"),
            '\t' => try out.appendSlice(alloc, "\\t"),
            else => {
                if (c < 0x20) {
                    var buf: [8]u8 = undefined;
                    const esc = std.fmt.bufPrint(&buf, "\\u{x:0>4}", .{c}) catch unreachable;
                    try out.appendSlice(alloc, esc);
                } else try out.append(alloc, c);
            },
        }
    }
    try out.append(alloc, '"');
}

pub fn rowToJson(alloc: std.mem.Allocator, row: *const Row) ![]u8 {
    var out: std.ArrayListUnmanaged(u8) = .empty;
    try out.append(alloc, '{');
    var it = row.iterator();
    var first = true;
    while (it.next()) |e| {
        if (!first) try out.append(alloc, ',');
        first = false;
        try writeJsonString(&out, alloc, e.key_ptr.*);
        try out.append(alloc, ':');
        try writeJsonValue(&out, alloc, e.value_ptr.*);
    }
    try out.append(alloc, '}');
    return out.toOwnedSlice(alloc);
}

// ---------------------------------------------------------------------------
// exported C ABI: read-only access from any language via the Zig parser
var g_last_error_buf: [256]u8 = undefined;
var g_last_error: []const u8 = "";

fn setErr(comptime fmt: []const u8, args: anytype) void {
    g_last_error = std.fmt.bufPrint(&g_last_error_buf, fmt, args) catch "error";
}

export fn gina_read_open(dir: [*:0]const u8) callconv(.c) ?*anyopaque {
    const db = std.heap.page_allocator.create(Db) catch {
        setErr("out of memory", .{});
        return null;
    };
    db.* = Db.open(std.heap.page_allocator, std.mem.span(dir)) catch |e| {
        setErr("open failed: {s}", .{@errorName(e)});
        std.heap.page_allocator.destroy(db);
        return null;
    };
    return db;
}

export fn gina_read_get(db: ?*anyopaque, table: [*:0]const u8, key: [*:0]const u8) callconv(.c) ?[*:0]u8 {
    const d: *Db = @ptrCast(@alignCast(db orelse return null));
    const row = d.get(std.mem.span(table), std.mem.span(key)) orelse return null;
    const json = rowToJson(std.heap.page_allocator, row) catch return null;
    const z = std.heap.page_allocator.dupeZ(u8, json) catch {
        std.heap.page_allocator.free(json);
        return null;
    };
    std.heap.page_allocator.free(json);
    return z.ptr;
}

export fn gina_read_count(db: ?*anyopaque, table: [*:0]const u8) callconv(.c) i64 {
    const d: *Db = @ptrCast(@alignCast(db orelse return -1));
    return @intCast(d.count(std.mem.span(table)));
}

export fn gina_read_last_error() callconv(.c) [*:0]const u8 {
    return @ptrCast(g_last_error.ptr);
}

export fn gina_read_close(db: ?*anyopaque) callconv(.c) void {
    const d: *Db = @ptrCast(@alignCast(db orelse return));
    d.deinit();
    std.heap.page_allocator.destroy(d);
}

export fn gina_read_version() callconv(.c) [*:0]const u8 {
    return "ginaread 0.5 (zig 0.16)";
}

// ---------------------------------------------------------------------------
// tests
const testing = std.testing;

test "crc32 matches reference vector" {
    // IEEE CRC32 of "123456789" is 0xCBF43926 (same as zlib)
    try testing.expectEqual(@as(u32, 0xCBF4_3926), crc32("123456789"));
}

test "wal frame encode/decode roundtrip via bytes" {
    const alloc = testing.allocator;
    var body: std.ArrayListUnmanaged(u8) = .empty;
    defer body.deinit(alloc);
    try body.append(alloc, FORMAT_VERSION);
    try body.append(alloc, 1); // put
    const lsn: u64 = 42;
    var i: usize = 0;
    while (i < 8) : (i += 1) try body.append(alloc, @truncate(lsn >> @intCast(i * 8)));
    const tx: u64 = 7;
    i = 0;
    while (i < 8) : (i += 1) try body.append(alloc, @truncate(tx >> @intCast(i * 8)));
    try body.appendSlice(alloc, "payload");

    var frame: std.ArrayListUnmanaged(u8) = .empty;
    defer frame.deinit(alloc);
    var w: u32 = WAL_MAGIC;
    i = 0;
    while (i < 4) : (i += 1) try frame.append(alloc, @truncate(w >> @intCast(i * 8)));
    w = @intCast(body.items.len);
    i = 0;
    while (i < 4) : (i += 1) try frame.append(alloc, @truncate(w >> @intCast(i * 8)));
    w = crc32(body.items);
    i = 0;
    while (i < 4) : (i += 1) try frame.append(alloc, @truncate(w >> @intCast(i * 8)));
    try frame.appendSlice(alloc, body.items);

    var res = try WalResult.parse(alloc, frame.items);
    defer res.deinit(alloc);
    try testing.expectEqual(@as(usize, 1), res.frames.len);
    try testing.expectEqual(@as(u64, 42), res.frames[0].lsn);
    try testing.expectEqual(@as(u64, 7), res.frames[0].tx);
    try testing.expectEqualStrings("payload", res.frames[0].payload);
    try testing.expectEqual(@as(usize, 0), res.bad_tail_bytes);
    // NOTE: res.frames borrows from `frame`; nothing extra to free here
}

test "wal torn tail is reported, not thrown" {
    const alloc = testing.allocator;
    const data = [_]u8{ 0x47, 0x49, 0x4E, 0x41, 0x00, 0x00, 0x00 }; // magic + partial len
    const res = try WalResult.parse(alloc, &data);
    try testing.expectEqual(@as(usize, 0), res.frames.len);
    try testing.expect(res.bad_tail_bytes > 0);
}

test "value decode: vector + text" {
    const alloc = testing.allocator;
    var buf: std.ArrayListUnmanaged(u8) = .empty;
    defer buf.deinit(alloc);
    try buf.append(alloc, 8); // vector tag
    try buf.appendSlice(alloc, &[_]u8{ 2, 0, 0, 0 }); // dim = 2 (u32 LE)
    try buf.appendSlice(alloc, &.{ 0, 0, 0x80, 0x3F }); // 1.0f
    try buf.appendSlice(alloc, &.{ 0, 0, 0x00, 0xC0 }); // -2.0f
    var pos: usize = 0;
    const v = try decodeValue(buf.items, &pos);
    switch (v) {
        .vector => |vec| {
            try testing.expectEqual(@as(usize, 2), vec.len);
            try testing.expectEqual(@as(f32, 1.0), vec[0]);
            try testing.expectEqual(@as(f32, -2.0), vec[1]);
        },
        else => return error.TestUnexpectedResult,
    }
}
