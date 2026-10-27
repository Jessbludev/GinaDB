//! GinaDB Lite 0.7 — gina.zig: Zig FFI binding over libginadb (C ABI).
//! Full CRUD access from Zig by linking the C++ engine's shared library.
//!
//!   const gina = @import("gina");
//!   var db = try gina.Database.open("mi-datos", .{});
//!   defer db.close();
//!   try db.createTable("users", &.{
//!       .{ .name = "id", .type = .text, .nullable = false },
//!       .{ .name = "vec", .type = .vector },
//!   });
//!   try db.insert("users", "{\"id\":\"u1\",\"vec\":{\"$vec\":[0.1,0.2]}}");
//!   const row = (try db.get("users", "u1")).?;   // JSON string, call .free()

const std = @import("std");

// ---- C ABI declarations (include/gina/cabi.h) -------------------------------
const c = struct {
    pub const Status = struct {
        pub const OK: i32 = 0;
        pub const ENOENT: i32 = 1;
        pub const EEXISTS: i32 = 2;
        pub const ESCHEMA: i32 = 3;
        pub const EKEY: i32 = 4;
        pub const ETX: i32 = 5;
        pub const EIO: i32 = 6;
        pub const ECORRUPT: i32 = 7;
        pub const ELIMIT: i32 = 8;
        pub const EARG: i32 = 9;
        pub const ERROR: i32 = -1;
    };
    extern fn gina_open(dir: [*:0]const u8, durable: i32, checkpoint_every: u32,
        changelog_capacity: u64, max_blob_size: u64) callconv(.c) ?*anyopaque;
    extern fn gina_close(db: ?*anyopaque) callconv(.c) void;
    extern fn gina_create_table(db: ?*anyopaque, name: [*:0]const u8,
        json_schema: [*:0]const u8) callconv(.c) i32;
    extern fn gina_drop_table(db: ?*anyopaque, name: [*:0]const u8) callconv(.c) i32;
    extern fn gina_insert(db: ?*anyopaque, table: [*:0]const u8,
        json_row: [*:0]const u8) callconv(.c) i32;
    extern fn gina_update(db: ?*anyopaque, table: [*:0]const u8, key: [*:0]const u8,
        json_patch: [*:0]const u8) callconv(.c) i32;
    extern fn gina_erase(db: ?*anyopaque, table: [*:0]const u8,
        key: [*:0]const u8) callconv(.c) i32;
    extern fn gina_get(db: ?*anyopaque, table: [*:0]const u8,
        key: [*:0]const u8) callconv(.c) ?[*:0]u8;
    extern fn gina_all(db: ?*anyopaque, table: [*:0]const u8) callconv(.c) ?[*:0]u8;
    extern fn gina_count(db: ?*anyopaque, table: [*:0]const u8) callconv(.c) i64;
    extern fn gina_knn(db: ?*anyopaque, table: [*:0]const u8, column: [*:0]const u8,
        query: [*]const f32, dims: u32, k: u32, metric: i32) callconv(.c) ?[*:0]u8;
    extern fn gina_search(db: ?*anyopaque, table: [*:0]const u8, column: [*:0]const u8,
        query: [*:0]const u8, limit: u32) callconv(.c) ?[*:0]u8;
    extern fn gina_changes(db: ?*anyopaque, table: [*:0]const u8,
        since: u64) callconv(.c) ?[*:0]u8;
    extern fn gina_watch(db: ?*anyopaque, table: [*:0]const u8,
        cb: *const fn ([*:0]const u8, ?*anyopaque) callconv(.c) void,
        userdata: ?*anyopaque) callconv(.c) i64;
    extern fn gina_unwatch(db: ?*anyopaque, watch_id: i64) callconv(.c) i32;
    extern fn gina_checkpoint(db: ?*anyopaque) callconv(.c) i32;
    extern fn gina_compact(db: ?*anyopaque) callconv(.c) i32;
    extern fn gina_select(db: ?*anyopaque, table: [*:0]const u8,
        json_query: [*:0]const u8) callconv(.c) ?[*:0]u8;
    extern fn gina_create_index(db: ?*anyopaque, table: [*:0]const u8,
        column: [*:0]const u8) callconv(.c) i32;
    extern fn gina_drop_index(db: ?*anyopaque, table: [*:0]const u8,
        column: [*:0]const u8) callconv(.c) i32;
    extern fn gina_stats(db: ?*anyopaque) callconv(.c) ?[*:0]u8;
    extern fn gina_tables(db: ?*anyopaque) callconv(.c) ?[*:0]u8;
    extern fn gina_exec(db: ?*anyopaque, json_ops: [*:0]const u8) callconv(.c) i32;
    extern fn gina_free(s: ?[*:0]u8) callconv(.c) void;
    extern fn gina_version() callconv(.c) [*:0]const u8;
    extern fn gina_last_error(db: ?*anyopaque) callconv(.c) [*:0]const u8;
    extern fn gina_last_status() callconv(.c) i32;
};

pub const Status = c.Status;

pub const GinaError = error{
    Gina,
    NotFound,
    TableExists,
    SchemaViolation,
    BadKey,
    TxDiscarded,
    Io,
    Corruption,
    LimitExceeded,
    InvalidArgument,
    OutOfMemory,
};

pub const ColumnType = enum { null_, bool_, int, float, text, bytes, array, object, vector };

pub const ColumnDef = struct {
    name: []const u8,
    type: ColumnType = .text,
    nullable: bool = true,
    fts: bool = false,
};

pub const Options = struct {
    durable: bool = false,
    checkpoint_every: u32 = 1000,
    changelog_capacity: u64 = 4096,
    max_blob_size: u64 = 16 * 1024 * 1024,
};

pub const Metric = enum(i32) { cosine = 0, l2 = 1 };

/// A heap JSON string owned by the caller; call free() when done.
pub const Json = struct {
    ptr: [*:0]u8,

    pub fn span(self: Json) [:0]const u8 {
        return std.mem.span(self.ptr);
    }
    pub fn free(self: Json) void {
        c.gina_free(self.ptr);
    }
};

fn statusToError(st: i32) GinaError {
    return switch (st) {
        c.Status.OK => unreachable,
        c.Status.ENOENT => error.NotFound,
        c.Status.EEXISTS => error.TableExists,
        c.Status.ESCHEMA => error.SchemaViolation,
        c.Status.EKEY => error.BadKey,
        c.Status.ETX => error.TxDiscarded,
        c.Status.EIO => error.Io,
        c.Status.ECORRUPT => error.Corruption,
        c.Status.ELIMIT => error.LimitExceeded,
        c.Status.EARG => error.InvalidArgument,
        else => error.Gina,
    };
}

pub const Database = struct {
    h: *anyopaque,
    arena: std.heap.ArenaAllocator,

    pub fn open(path: []const u8, opts: Options) GinaError!Database {
        var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
        errdefer arena.deinit();
        const p = arena.allocator().dupeZ(u8, path) catch return error.OutOfMemory;
        const h = c.gina_open(p.ptr, if (opts.durable) 1 else 0,
            opts.checkpoint_every, opts.changelog_capacity,
            opts.max_blob_size) orelse return statusToError(c.gina_last_status());
        return .{ .h = h, .arena = arena };
    }

    pub fn close(self: *Database) void {
        c.gina_close(self.h);
        self.arena.deinit();
    }

    fn z(self: *Database, s: []const u8) GinaError![:0]const u8 {
        return self.arena.allocator().dupeZ(u8, s) catch error.OutOfMemory;
    }

    fn check(self: *Database, st: i32) GinaError!void {
        if (st != c.Status.OK) {
            _ = self;
            return statusToError(st);
        }
    }

    pub fn lastError(self: *Database) [:0]const u8 {
        return std.mem.span(c.gina_last_error(self.h));
    }

    pub fn createTable(self: *Database, name: []const u8, columns: []const ColumnDef, primary_key: []const u8, indexes: []const []const u8) GinaError!void {
        var buf: std.ArrayListUnmanaged(u8) = .empty;
        const a = self.arena.allocator();
        buf.appendSlice(a, "{\"primary_key\":") catch return error.OutOfMemory;
        jsonStr(&buf, a, primary_key) catch return error.OutOfMemory;
        buf.appendSlice(a, ",\"columns\":[") catch return error.OutOfMemory;
        for (columns, 0..) |col, i| {
            if (i > 0) buf.append(a, ',') catch return error.OutOfMemory;
            buf.appendSlice(a, "{\"name\":") catch return error.OutOfMemory;
            jsonStr(&buf, a, col.name) catch return error.OutOfMemory;
            const tname = switch (col.type) {
                .null_ => "null", .bool_ => "bool", .int => "int", .float => "float",
                .text => "text", .bytes => "bytes", .array => "array",
                .object => "object", .vector => "vector",
            };
            buf.appendSlice(a, ",\"type\":\"") catch return error.OutOfMemory;
            buf.appendSlice(a, tname) catch return error.OutOfMemory;
            buf.appendSlice(a, if (col.nullable) "\",\"nullable\":true" else "\",\"nullable\":false") catch return error.OutOfMemory;
            buf.appendSlice(a, if (col.fts) ",\"fts\":true}" else ",\"fts\":false}") catch return error.OutOfMemory;
        }
        buf.appendSlice(a, "],\"indexes\":[") catch return error.OutOfMemory;
        for (indexes, 0..) |ix, i| {
            if (i > 0) buf.append(a, ',') catch return error.OutOfMemory;
            jsonStr(&buf, a, ix) catch return error.OutOfMemory;
        }
        buf.appendSlice(a, "]}") catch return error.OutOfMemory;
        buf.append(a, 0) catch return error.OutOfMemory; // NUL terminator
        const n = try self.z(name);
        try self.check(c.gina_create_table(self.h, n.ptr, @ptrCast(buf.items.ptr)));

    }

    pub fn dropTable(self: *Database, name: []const u8) GinaError!void {
        const n = try self.z(name);
        try self.check(c.gina_drop_table(self.h, n.ptr));
    }

    /// json_row is a JSON object; vectors as {"$vec":[...]}, blobs as {"$bytes":"<b64>"}
    pub fn insert(self: *Database, table: []const u8, json_row: []const u8) GinaError!void {
        const t = try self.z(table);
        const r = try self.z(json_row);
        try self.check(c.gina_insert(self.h, t.ptr, r.ptr));
    }

    pub fn update(self: *Database, table: []const u8, key: []const u8, json_patch: []const u8) GinaError!bool {
        const t = try self.z(table);
        const k = try self.z(key);
        const p = try self.z(json_patch);
        const st = c.gina_update(self.h, t.ptr, k.ptr, p.ptr);
        if (st == c.Status.ENOENT) return false;
        if (st != c.Status.OK) return statusToError(st);
        return true;
    }

    pub fn erase(self: *Database, table: []const u8, key: []const u8) GinaError!bool {
        const t = try self.z(table);
        const k = try self.z(key);
        const rc = c.gina_erase(self.h, t.ptr, k.ptr);
        if (rc < 0) return statusToError(rc);
        return rc == 1;
    }

    /// Returns null when the key does not exist.
    pub fn get(self: *Database, table: []const u8, key: []const u8) GinaError!?Json {
        const t = try self.z(table);
        const k = try self.z(key);
        const p = c.gina_get(self.h, t.ptr, k.ptr) orelse {
            if (c.gina_last_status() == c.Status.ENOENT) return null;
            return statusToError(c.gina_last_status());
        };
        return .{ .ptr = p };
    }

    pub fn all(self: *Database, table: []const u8) GinaError!Json {
        const t = try self.z(table);
        const p = c.gina_all(self.h, t.ptr) orelse
            return statusToError(c.gina_last_status());
        return .{ .ptr = p };
    }

    pub fn count(self: *Database, table: []const u8) GinaError!i64 {
        const t = try self.z(table);
        const n = c.gina_count(self.h, t.ptr);
        if (n < 0) return statusToError(@intCast(n));
        return n;
    }

    pub fn knn(self: *Database, table: []const u8, column: []const u8, query: []const f32, k: u32, metric: Metric) GinaError!Json {
        const t = try self.z(table);
        const col = try self.z(column);
        const p = c.gina_knn(self.h, t.ptr, col.ptr, query.ptr,
            @intCast(query.len), k, @intFromEnum(metric)) orelse
            return statusToError(c.gina_last_status());
        return .{ .ptr = p };
    }

    pub fn search(self: *Database, table: []const u8, column: []const u8, query: []const u8, limit: u32) GinaError!Json {
        const t = try self.z(table);
        const col = try self.z(column);
        const q = try self.z(query);
        const p = c.gina_search(self.h, t.ptr, col.ptr, q.ptr, limit) orelse
            return statusToError(c.gina_last_status());
        return .{ .ptr = p };
    }

    pub fn changes(self: *Database, table: []const u8, since: u64) GinaError!Json {
        const t = try self.z(table);
        const p = c.gina_changes(self.h, t.ptr, since) orelse
            return statusToError(c.gina_last_status());
        return .{ .ptr = p };
    }

    pub fn checkpoint(self: *Database) GinaError!void {
        try self.check(c.gina_checkpoint(self.h));
    }

    /// 0.6: checkpoint completo (reescribe data.pages, reinicia delta).
    pub fn compact(self: *Database) GinaError!void {
        try self.check(c.gina_compact(self.h));
    }

    /// 0.6: select JSON con planner (índices PK/secundarios, order, limit,
    /// fields). Devuelve un array JSON de filas (liberar con Json.free()).
    pub fn select(self: *Database, table: []const u8, json_query: []const u8) GinaError!Json {
        const t = try self.z(table);
        const q = try self.z(json_query);
        const p = c.gina_select(self.h, t.ptr, q.ptr) orelse
            return statusToError(c.gina_last_status());
        return .{ .ptr = p };
    }

    /// 0.6: índice secundario atómico (WAL, sobrevive reinicios).
    pub fn createIndex(self: *Database, table: []const u8, column: []const u8) GinaError!void {
        const t = try self.z(table);
        const co = try self.z(column);
        try self.check(c.gina_create_index(self.h, t.ptr, co.ptr));
    }

    pub fn dropIndex(self: *Database, table: []const u8, column: []const u8) GinaError!void {
        const t = try self.z(table);
        const co = try self.z(column);
        try self.check(c.gina_drop_index(self.h, t.ptr, co.ptr));
    }

    pub fn stats(self: *Database) GinaError!Json {
        const p = c.gina_stats(self.h) orelse
            return statusToError(c.gina_last_status());
        return .{ .ptr = p };
    }

    pub fn tables(self: *Database) GinaError!Json {
        const p = c.gina_tables(self.h) orelse
            return statusToError(c.gina_last_status());
        return .{ .ptr = p };
    }

    /// Atomic batch: json_ops = [{"op":"put","table":t,"row":{...}}, ...]
    pub fn exec(self: *Database, json_ops: []const u8) GinaError!void {
        const ops = try self.z(json_ops);
        try self.check(c.gina_exec(self.h, ops.ptr));
    }

    pub fn version() [:0]const u8 {
        return std.mem.span(c.gina_version());
    }
};

fn jsonStr(out: *std.ArrayListUnmanaged(u8), alloc: std.mem.Allocator, s: []const u8) !void {
    try out.append(alloc, '"');
    for (s) |ch| {
        switch (ch) {
            '"' => try out.appendSlice(alloc, "\\\""),
            '\\' => try out.appendSlice(alloc, "\\\\"),
            '\n' => try out.appendSlice(alloc, "\\n"),
            else => try out.append(alloc, ch),
        }
    }
    try out.append(alloc, '"');
}
