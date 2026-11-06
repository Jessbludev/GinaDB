//! GinaDB — Block B6: graph layer over the frozen 0.7 engine (C ABI).
//!
//! Design decision (BLOQUES §5): the graph is a LAYER over the kernel, not a
//! separate engine. An edge (src, type, dst, props) is stored twice as
//! ordered keys, exactly as §2-B6 mandates:
//!
//!   out table "g.<name>.out":  id = src <0x1F> type <0x1F> dst <0x1F>
//!   in  table "g.<name>.in":   id = dst <0x1F> type <0x1F> src <0x1F>
//!
//!   - Byte-stuffing: a 0x1F byte inside a component is doubled, so a
//!     component can never forge a separator. Components end with a single
//!     unstuffed 0x1F. In both tables the SCANNED node comes first and the
//!     OTHER endpoint last, so `parseKey().c` is always "the other node".
//!   - A neighborhood is an exact B+Tree prefix range scan via gina_select:
//!     `and(ge(P), lt(P'))` on the PK column. `lt` is exclusive; P' = P with
//!     the trailing 0x1F incremented to 0x20. [P, P') contains exactly the
//!     keys sharing prefix P — zero over-scan, no false positives.
//!   - Both rows are written in ONE atomic gina_exec batch (the engine
//!     validates every op before appending a single WAL byte, P3).
//!   - No new WAL kinds: edges are ordinary K_PUT/K_DEL rows, so the frozen
//!     C++ oracle and the independent Zig reader stay untouched.
//!   - The physical keyspaces of §1.1 (space_kind 5/6) arrive with the Zig
//!     kernel (F2–F5); this interface will not change when they do.
//!
//! Memory rules: results are allocated by the CALLER-provided allocator;
//! the reusable internal work buffer lives in the Graph and is freed on
//! deinit(). Single-threaded per Graph instance (matches C ABI guarantees).

const std = @import("std");
const gina = @import("gina.zig");

/// Component separator (byte-stuffed: doubled inside components).
pub const SEP: u8 = 0x1F;
/// Exclusive upper bound byte for a prefix scan: prefixes end with SEP,
/// so P' = P with the trailing SEP incremented to SEP+1.
const SEP_HI: u8 = SEP + 1;

pub const GinaError = gina.GinaError;

pub const Direction = enum { out, in, both };

pub const Edge = struct {
    src: []const u8,
    edge_type: []const u8,
    dst: []const u8,
    /// Optional JSON object stored in the out-row (e.g. `{"weight":3}`).
    props: ?[]const u8 = null,
};

pub const Neighbor = struct {
    node: []const u8,
    edge_type: []const u8,
};

pub const Visit = struct {
    node: []const u8,
    depth: u32,
};

pub const TraverseResult = struct {
    /// Every reached node with its BFS depth (seeds have depth 0).
    visits: []Visit,
    /// true when a frontier/scan limit truncated the traversal.
    truncated: bool,
};

pub const PathResult = struct {
    /// null when dst is unreachable.
    path: ?[]const []const u8 = null,
    /// true when the expansion budget ran out before finding dst.
    exhausted: bool = false,
};

// ---- hot-path externs ---------------------------------------------------------
// The generic binding (gina.zig) dupes every string into a never-freed arena;
// for millions of calls that arena would grow unbounded. These externs take
// NUL-terminated buffers we manage ourselves (reusable work buffer).
const cabi = struct {
    extern fn gina_exec(db: *anyopaque, json_ops: [*:0]const u8) callconv(.c) i32;
    extern fn gina_select(db: *anyopaque, table: [*:0]const u8, json_query: [*:0]const u8) callconv(.c) ?[*:0]u8;
    extern fn gina_get(db: *anyopaque, table: [*:0]const u8, key: [*:0]const u8) callconv(.c) ?[*:0]u8;
    extern fn gina_update(db: *anyopaque, table: [*:0]const u8, key: [*:0]const u8, json_patch: [*:0]const u8) callconv(.c) i32;
    extern fn gina_count(db: *anyopaque, table: [*:0]const u8) callconv(.c) i64;
    extern fn gina_free(s: ?[*:0]u8) callconv(.c) void;
    extern fn gina_last_status() callconv(.c) i32;
};

fn statusToError(st: i32) GinaError {
    return switch (st) {
        gina.Status.OK => unreachable,
        gina.Status.ENOENT => error.NotFound,
        gina.Status.EEXISTS => error.TableExists,
        gina.Status.ESCHEMA => error.SchemaViolation,
        gina.Status.EKEY => error.BadKey,
        gina.Status.ETX => error.TxDiscarded,
        gina.Status.EIO => error.Io,
        gina.Status.ECORRUPT => error.Corruption,
        gina.Status.ELIMIT => error.LimitExceeded,
        gina.Status.EARG => error.InvalidArgument,
        else => error.Gina,
    };
}

// ---- key codec ------------------------------------------------------------------

/// Appends `s` byte-stuffed and terminated by a single SEP.
fn stuffComponent(out: *std.ArrayListUnmanaged(u8), alloc: std.mem.Allocator, s: []const u8) !void {
    for (s) |ch| {
        try out.append(alloc, ch);
        if (ch == SEP) try out.append(alloc, SEP);
    }
    try out.append(alloc, SEP);
}

fn checkComponent(s: []const u8) !void {
    // NUL would truncate the C string silently — reject loudly (P8 spirit).
    for (s) |ch| if (ch == 0) return error.InvalidArgument;
}

/// Decodes a stuffed body (pairs of SEP -> one SEP).
/// ALWAYS returns an owned slice (callers free it uniformly).
fn unstuff(alloc: std.mem.Allocator, s: []const u8) ![]const u8 {
    if (std.mem.indexOfScalar(u8, s, SEP) == null) return try alloc.dupe(u8, s);
    var out: std.ArrayListUnmanaged(u8) = .empty;
    errdefer out.deinit(alloc);
    var i: usize = 0;
    while (i < s.len) {
        try out.append(alloc, s[i]);
        if (s[i] == SEP and i + 1 < s.len and s[i + 1] == SEP) i += 1; // pair -> one
        i += 1;
    }
    return try out.toOwnedSlice(alloc);
}

pub const KeyParts = struct { a: []const u8, b: []const u8, c: []const u8 };

/// Splits a stuffed edge key into its three unstuffed components.
/// out-key: a=src, b=type, c=dst · in-key: a=scanned node, b=type, c=other.
pub fn parseKey(alloc: std.mem.Allocator, key: []const u8) !KeyParts {
    const e1 = componentEnd(key, 0);
    if (e1 >= key.len) return error.InvalidArgument;
    const e2 = componentEnd(key, e1 + 1);
    if (e2 >= key.len) return error.InvalidArgument;
    const e3 = componentEnd(key, e2 + 1);
    if (e3 != key.len - 1) return error.InvalidArgument;
    const a = try unstuff(alloc, key[0..e1]);
    errdefer alloc.free(@constCast(a));
    const b = try unstuff(alloc, key[e1 + 1 .. e2]);
    errdefer alloc.free(@constCast(b));
    const c = try unstuff(alloc, key[e2 + 1 .. e3]);
    return .{ .a = a, .b = b, .c = c };
}

/// Index of the terminator SEP of the component starting at `from`
/// (an odd run of SEPs ends a component; even runs are stuffed bytes).
fn componentEnd(key: []const u8, from: usize) usize {
    var i = from;
    while (i < key.len) {
        if (key[i] != SEP) {
            i += 1;
            continue;
        }
        var run: usize = 0;
        var j = i;
        while (j < key.len and key[j] == SEP) : (j += 1) run += 1;
        if (run % 2 == 1) return i;
        i = j;
    }
    return key.len;
}

// ---- JSON helpers ---------------------------------------------------------------

fn jsonEscape(out: *std.ArrayListUnmanaged(u8), alloc: std.mem.Allocator, s: []const u8) !void {
    try out.append(alloc, '"');
    for (s) |ch| {
        switch (ch) {
            '"' => try out.appendSlice(alloc, "\\\""),
            '\\' => try out.appendSlice(alloc, "\\\\"),
            '\n' => try out.appendSlice(alloc, "\\n"),
            '\r' => try out.appendSlice(alloc, "\\r"),
            '\t' => try out.appendSlice(alloc, "\\t"),
            0x08 => try out.appendSlice(alloc, "\\b"),
            0x0C => try out.appendSlice(alloc, "\\f"),
            else => {
                if (ch < 0x20) {
                    var buf: [6]u8 = undefined;
                    const hex = std.fmt.bufPrint(&buf, "\\u{x:0>4}", .{ch}) catch unreachable;
                    try out.appendSlice(alloc, hex);
                } else {
                    try out.append(alloc, ch);
                }
            },
        }
    }
    try out.append(alloc, '"');
}

/// Extracts every "id" string value from a select response
/// (`[{"id":"..."},...]` with the fields:["id"] projection). Minimal scanner
/// that unescapes what the engine escapes (control chars as \u00xx).
fn extractIds(alloc: std.mem.Allocator, json: []const u8) ![][]const u8 {
    var ids: std.ArrayListUnmanaged([]const u8) = .empty;
    errdefer {
        for (ids.items) |it| alloc.free(@constCast(it));
        ids.deinit(alloc);
    }
    var i: usize = 0;
    while (i < json.len) {
        const key_at = std.mem.indexOfPos(u8, json, i, "\"id\":") orelse break;
        var p = key_at + 5;
        if (p >= json.len or json[p] != '"') {
            i = key_at + 4;
            continue;
        }
        p += 1;
        var val: std.ArrayListUnmanaged(u8) = .empty;
        errdefer val.deinit(alloc);
        while (p < json.len and json[p] != '"') {
            if (json[p] != '\\') {
                try val.append(alloc, json[p]);
                p += 1;
                continue;
            }
            p += 1;
            if (p >= json.len) return error.InvalidArgument;
            switch (json[p]) {
                '"' => try val.append(alloc, '"'),
                '\\' => try val.append(alloc, '\\'),
                '/' => try val.append(alloc, '/'),
                'b' => try val.append(alloc, 0x08),
                'f' => try val.append(alloc, 0x0C),
                'n' => try val.append(alloc, '\n'),
                'r' => try val.append(alloc, '\r'),
                't' => try val.append(alloc, '\t'),
                'u' => {
                    if (p + 4 >= json.len) return error.InvalidArgument;
                    const cp = std.fmt.parseInt(u21, json[p + 1 .. p + 5], 16) catch
                        return error.InvalidArgument;
                    var ubuf: [4]u8 = undefined;
                    const n = std.unicode.utf8Encode(cp, &ubuf) catch
                        return error.InvalidArgument;
                    try val.appendSlice(alloc, ubuf[0..n]);
                    p += 4;
                },
                else => return error.InvalidArgument,
            }
            p += 1;
        }
        if (p >= json.len) return error.InvalidArgument;
        p += 1; // closing quote
        try ids.append(alloc, try val.toOwnedSlice(alloc));
        i = p;
    }
    return try ids.toOwnedSlice(alloc);
}

// ---- Graph -----------------------------------------------------------------------

pub const Graph = struct {
    db: *gina.Database,
    alloc: std.mem.Allocator,
    name: []const u8,
    out_table: [:0]const u8,
    in_table: [:0]const u8,
    /// reusable work buffer for queries / batch ops (never aliased with pbuf)
    buf: std.ArrayListUnmanaged(u8) = .empty,
    /// separate buffer for scan prefixes: scanPrefix builds the query in
    /// `buf` while reading the prefix from `pbuf` — they must never alias.
    pbuf: std.ArrayListUnmanaged(u8) = .empty,

    /// Creates the two backing tables if they do not exist yet.
    pub fn init(db: *gina.Database, alloc: std.mem.Allocator, name: []const u8) GinaError!Graph {
        if (name.len == 0) return error.InvalidArgument;
        for (name) |ch| if (ch == SEP or ch == 0) return error.InvalidArgument;

        const name_d = try alloc.dupe(u8, name);
        errdefer alloc.free(name_d);
        const out_t = try tableFor(alloc, name, ".out");
        errdefer alloc.free(out_t);
        const in_t = try tableFor(alloc, name, ".in");
        errdefer alloc.free(in_t);

        // out table: props live here (the in table is a minimal key mirror)
        db.createTable(out_t, &.{
            .{ .name = "id", .type = .text, .nullable = false },
            .{ .name = "etype", .type = .text },
            .{ .name = "dst", .type = .text },
            .{ .name = "props", .type = .object },
        }, "id", &.{}) catch |e| switch (e) {
            error.TableExists => {}, // reopening an existing graph
            else => return e,
        };
        db.createTable(in_t, &.{
            .{ .name = "id", .type = .text, .nullable = false },
            .{ .name = "etype", .type = .text },
            .{ .name = "src", .type = .text },
        }, "id", &.{}) catch |e| switch (e) {
            error.TableExists => {},
            else => return e,
        };
        return .{ .db = db, .alloc = alloc, .name = name_d, .out_table = out_t, .in_table = in_t };
    }

    pub fn deinit(g: *Graph) void {
        g.buf.deinit(g.alloc);
        g.pbuf.deinit(g.alloc);
        g.alloc.free(g.out_table);
        g.alloc.free(g.in_table);
        g.alloc.free(g.name);
    }

    fn tableFor(alloc: std.mem.Allocator, name: []const u8, suffix: []const u8) ![:0]const u8 {
        const total = 2 + name.len + suffix.len;
        const buf = try alloc.allocSentinel(u8, total, 0);
        errdefer alloc.free(buf);
        @memcpy(buf[0..2], "g.");
        @memcpy(buf[2 .. 2 + name.len], name);
        @memcpy(buf[2 + name.len ..][0..suffix.len], suffix);
        return buf;
    }

    // -- key builders (use g.buf; never nest) --------------------------------

    fn buildEdgeKey(g: *Graph, comptime table: enum { out, in }, e: Edge) ![]const u8 {
        try checkComponent(e.src);
        try checkComponent(e.edge_type);
        try checkComponent(e.dst);
        const start = g.buf.items.len;
        defer g.buf.shrinkRetainingCapacity(start);
        const first = if (table == .out) e.src else e.dst;
        const last = if (table == .out) e.dst else e.src;
        try stuffComponent(&g.buf, g.alloc, first);
        try stuffComponent(&g.buf, g.alloc, e.edge_type);
        try stuffComponent(&g.buf, g.alloc, last);
        return try g.alloc.dupe(u8, g.buf.items[start..]);
    }

    /// Builds `node [SEP type] SEP` into g.pbuf and returns it. Valid until
    /// the next use of the prefix buffer.
    fn nodePrefix(g: *Graph, node: []const u8, edge_type: ?[]const u8) ![]const u8 {
        try checkComponent(node);
        if (edge_type) |t| try checkComponent(t);
        g.pbuf.clearRetainingCapacity();
        try stuffComponent(&g.pbuf, g.alloc, node);
        if (edge_type) |t| try stuffComponent(&g.pbuf, g.alloc, t);
        return g.pbuf.items;
    }

    // -- writes ----------------------------------------------------------------

    /// Atomically writes both entries of every edge (one gina_exec batch):
    /// either all edges land or none does.
    pub fn addEdges(g: *Graph, edges: []const Edge) GinaError!void {
        if (edges.len == 0) return;
        g.buf.clearRetainingCapacity();
        try g.buf.appendSlice(g.alloc, "[");
        for (edges, 0..) |e, i| {
            if (i > 0) try g.buf.append(g.alloc, ',');
            const okey = try g.buildEdgeKey(.out, e);
            defer g.alloc.free(@constCast(okey));
            const ikey = try g.buildEdgeKey(.in, e);
            defer g.alloc.free(@constCast(ikey));
            // out row (owns props)
            try g.buf.appendSlice(g.alloc, "{\"op\":\"put\",\"table\":");
            try jsonEscape(&g.buf, g.alloc, g.out_table);
            try g.buf.appendSlice(g.alloc, ",\"row\":{\"id\":");
            try jsonEscape(&g.buf, g.alloc, okey);
            try g.buf.appendSlice(g.alloc, ",\"etype\":");
            try jsonEscape(&g.buf, g.alloc, e.edge_type);
            try g.buf.appendSlice(g.alloc, ",\"dst\":");
            try jsonEscape(&g.buf, g.alloc, e.dst);
            if (e.props) |p| {
                try g.buf.appendSlice(g.alloc, ",\"props\":");
                try g.buf.appendSlice(g.alloc, p);
            }
            try g.buf.appendSlice(g.alloc, "}}");
            // in row (minimal mirror)
            try g.buf.appendSlice(g.alloc, ",{\"op\":\"put\",\"table\":");
            try jsonEscape(&g.buf, g.alloc, g.in_table);
            try g.buf.appendSlice(g.alloc, ",\"row\":{\"id\":");
            try jsonEscape(&g.buf, g.alloc, ikey);
            try g.buf.appendSlice(g.alloc, ",\"etype\":");
            try jsonEscape(&g.buf, g.alloc, e.edge_type);
            try g.buf.appendSlice(g.alloc, ",\"src\":");
            try jsonEscape(&g.buf, g.alloc, e.src);
            try g.buf.appendSlice(g.alloc, "}}");
        }
        try g.buf.append(g.alloc, ']');
        try g.buf.append(g.alloc, 0);
        defer g.buf.shrinkRetainingCapacity(g.buf.items.len - 1);
        const st = cabi.gina_exec(g.db.h, @ptrCast(g.buf.items.ptr));
        if (st != gina.Status.OK) return statusToError(st);
    }

    pub fn addEdge(g: *Graph, src: []const u8, edge_type: []const u8, dst: []const u8, props: ?[]const u8) GinaError!void {
        try g.addEdges(&.{.{ .src = src, .edge_type = edge_type, .dst = dst, .props = props }});
    }

    /// Atomically removes both entries of every edge. Idempotent.
    pub fn removeEdges(g: *Graph, edges: []const Edge) GinaError!void {
        if (edges.len == 0) return;
        g.buf.clearRetainingCapacity();
        try g.buf.appendSlice(g.alloc, "[");
        for (edges, 0..) |e, i| {
            if (i > 0) try g.buf.append(g.alloc, ',');
            const okey = try g.buildEdgeKey(.out, e);
            defer g.alloc.free(@constCast(okey));
            const ikey = try g.buildEdgeKey(.in, e);
            defer g.alloc.free(@constCast(ikey));
            try g.buf.appendSlice(g.alloc, "{\"op\":\"del\",\"table\":");
            try jsonEscape(&g.buf, g.alloc, g.out_table);
            try g.buf.appendSlice(g.alloc, ",\"key\":");
            try jsonEscape(&g.buf, g.alloc, okey);
            try g.buf.appendSlice(g.alloc, "},{\"op\":\"del\",\"table\":");
            try jsonEscape(&g.buf, g.alloc, g.in_table);
            try g.buf.appendSlice(g.alloc, ",\"key\":");
            try jsonEscape(&g.buf, g.alloc, ikey);
            try g.buf.append(g.alloc, '}');
        }
        try g.buf.append(g.alloc, ']');
        try g.buf.append(g.alloc, 0);
        defer g.buf.shrinkRetainingCapacity(g.buf.items.len - 1);
        const st = cabi.gina_exec(g.db.h, @ptrCast(g.buf.items.ptr));
        if (st != gina.Status.OK) return statusToError(st);
    }

    pub fn removeEdge(g: *Graph, src: []const u8, edge_type: []const u8, dst: []const u8) GinaError!void {
        try g.removeEdges(&.{.{ .src = src, .edge_type = edge_type, .dst = dst }});
    }

    /// Replaces the props of an existing edge (out row). false if absent.
    pub fn setEdgeProps(g: *Graph, src: []const u8, edge_type: []const u8, dst: []const u8, props: []const u8) GinaError!bool {
        const e = Edge{ .src = src, .edge_type = edge_type, .dst = dst };
        const okey = try g.buildEdgeKey(.out, e);
        defer g.alloc.free(@constCast(okey));
        g.buf.clearRetainingCapacity();
        try g.buf.appendSlice(g.alloc, "{\"props\":");
        try g.buf.appendSlice(g.alloc, props);
        try g.buf.appendSlice(g.alloc, "}");
        try g.buf.append(g.alloc, 0);
        defer g.buf.shrinkRetainingCapacity(g.buf.items.len - 1);
        const st = cabi.gina_update(g.db.h, g.out_table.ptr, @ptrCast(okey.ptr), @ptrCast(g.buf.items.ptr));
        if (st == gina.Status.ENOENT) return false;
        if (st != gina.Status.OK) return statusToError(st);
        return true;
    }

    /// Out-row of the edge (JSON; free it with Json.free()), null if absent.
    pub fn getEdge(g: *Graph, src: []const u8, edge_type: []const u8, dst: []const u8) GinaError!?gina.Json {
        const e = Edge{ .src = src, .edge_type = edge_type, .dst = dst };
        const okey = try g.buildEdgeKey(.out, e);
        defer g.alloc.free(@constCast(okey));
        g.buf.clearRetainingCapacity();
        try g.buf.appendSlice(g.alloc, okey);
        try g.buf.append(g.alloc, 0);
        defer g.buf.shrinkRetainingCapacity(g.buf.items.len - 1);
        const p = cabi.gina_get(g.db.h, g.out_table.ptr, @ptrCast(g.buf.items.ptr)) orelse {
            if (cabi.gina_last_status() == gina.Status.ENOENT) return null;
            return statusToError(cabi.gina_last_status());
        };
        return .{ .ptr = p };
    }

    pub fn edgeCount(g: *Graph) GinaError!i64 {
        const n = cabi.gina_count(g.db.h, g.out_table.ptr);
        // The engine leaks GINA_ENOENT (=1) as the count value for a missing
        // table; disambiguate via the status (a real 1-row table keeps OK).
        if (cabi.gina_last_status() == gina.Status.ENOENT) return error.NotFound;
        if (n < 0) return statusToError(@intCast(n));
        return n;
    }

    // -- reads -----------------------------------------------------------------

    /// Range-scans one prefix in one table, appending the raw edge keys.
    /// `prefix` must come from nodePrefix(); it is mutated in place to build
    /// the exclusive upper bound (trailing SEP -> SEP+1).
    fn scanPrefix(g: *Graph, table: [:0]const u8, prefix: []const u8, limit: usize, out_keys: *std.ArrayListUnmanaged([]const u8), alloc: std.mem.Allocator) GinaError!void {
        g.buf.clearRetainingCapacity();
        try g.buf.appendSlice(g.alloc, "{\"where\":{\"op\":\"and\",\"args\":[{\"op\":\"ge\",\"col\":\"id\",\"val\":");
        try jsonEscape(&g.buf, g.alloc, prefix); // lo: still ends with SEP
        const hi = @constCast(prefix);
        hi[hi.len - 1] = SEP_HI; // in-place: [P, P') with P' exclusive
        try g.buf.appendSlice(g.alloc, "},{\"op\":\"lt\",\"col\":\"id\",\"val\":");
        try jsonEscape(&g.buf, g.alloc, hi);
        try g.buf.appendSlice(g.alloc, "}]},\"fields\":[\"id\"],\"limit\":");
        var nbuf: [24]u8 = undefined;
        const nstr = std.fmt.bufPrint(&nbuf, "{d}", .{limit}) catch unreachable;
        try g.buf.appendSlice(g.alloc, nstr);
        try g.buf.appendSlice(g.alloc, "}");
        try g.buf.append(g.alloc, 0);
        defer g.buf.shrinkRetainingCapacity(g.buf.items.len - 1);

        const p = cabi.gina_select(g.db.h, table.ptr, @ptrCast(g.buf.items.ptr)) orelse
            return statusToError(cabi.gina_last_status());
        const resp = gina.Json{ .ptr = p };
        defer resp.free();
        const ids = try extractIds(alloc, resp.span());
        defer {
            for (ids) |id| alloc.free(@constCast(id));
            alloc.free(@constCast(ids));
        }
        for (ids) |id| {
            try out_keys.append(alloc, try alloc.dupe(u8, id));
        }
    }

    /// Nodes directly connected to `node` (with the connecting edge type).
    /// Empty `types` means every type. `scan_limit` bounds each prefix scan
    /// (supernode guard); use `degree` to measure a node first.
    pub fn neighbors(g: *Graph, alloc: std.mem.Allocator, node: []const u8, dir: Direction, types: []const []const u8, scan_limit: usize) GinaError![]Neighbor {
        var keys: std.ArrayListUnmanaged([]const u8) = .empty;
        defer {
            for (keys.items) |k| alloc.free(@constCast(k));
            keys.deinit(alloc);
        }
        try g.neighborsInto(alloc, node, dir, types, scan_limit, &keys);

        var nbrs: std.ArrayListUnmanaged(Neighbor) = .empty;
        errdefer {
            for (nbrs.items) |n| {
                alloc.free(@constCast(n.node));
                alloc.free(@constCast(n.edge_type));
            }
            nbrs.deinit(alloc);
        }
        for (keys.items) |k| {
            // A malformed key in our table means foreign writes: surface it
            // as data corruption instead of crashing (P8 spirit).
            const parts = parseKey(alloc, k) catch return error.Corruption;
            defer alloc.free(@constCast(parts.a));
            try nbrs.append(alloc, .{ .node = parts.c, .edge_type = parts.b });
        }
        return try nbrs.toOwnedSlice(alloc);
    }

    fn neighborsInto(g: *Graph, alloc: std.mem.Allocator, node: []const u8, dir: Direction, types: []const []const u8, scan_limit: usize, keys: *std.ArrayListUnmanaged([]const u8)) GinaError!void {
        if (dir == .out or dir == .both) {
            if (types.len == 0) {
                const pfx = try g.nodePrefix(node, null);
                try g.scanPrefix(g.out_table, pfx, scan_limit, keys, alloc);
            } else for (types) |t| {
                const pfx = try g.nodePrefix(node, t);
                try g.scanPrefix(g.out_table, pfx, scan_limit, keys, alloc);
            }
        }
        if (dir == .in or dir == .both) {
            if (types.len == 0) {
                const pfx = try g.nodePrefix(node, null);
                try g.scanPrefix(g.in_table, pfx, scan_limit, keys, alloc);
            } else for (types) |t| {
                const pfx = try g.nodePrefix(node, t);
                try g.scanPrefix(g.in_table, pfx, scan_limit, keys, alloc);
            }
        }
    }

    /// Number of edges of `node` in `dir` (unbounded scan — measure, don't
    /// loop over it; for a possibly-huge node prefer neighbors with a limit).
    pub fn degree(g: *Graph, alloc: std.mem.Allocator, node: []const u8, dir: Direction) GinaError!usize {
        var keys: std.ArrayListUnmanaged([]const u8) = .empty;
        defer {
            for (keys.items) |k| alloc.free(@constCast(k));
            keys.deinit(alloc);
        }
        try g.neighborsInto(alloc, node, dir, &.{}, std.math.maxInt(usize), &keys);
        return keys.items.len;
    }

    /// BFS from `seeds` up to `depth` levels. Memory is proportional to the
    /// reached nodes (visited set + current frontier), never to the graph.
    /// `frontier_limit` caps each level; `scan_limit` caps each per-node scan.
    /// If any cap fires, `truncated` is true.
    pub fn traverse(g: *Graph, alloc: std.mem.Allocator, seeds: []const []const u8, dir: Direction, types: []const []const u8, depth: u32, frontier_limit: usize, scan_limit: usize) GinaError!TraverseResult {
        var arena = std.heap.ArenaAllocator.init(alloc);
        defer arena.deinit();
        const aa = arena.allocator();

        var visited = std.StringHashMap(void).init(aa);
        var visits: std.ArrayListUnmanaged(Visit) = .empty;
        var level: std.ArrayListUnmanaged([]const u8) = .empty;
        var truncated = false;

        for (seeds) |s| {
            const gop = try visited.getOrPut(try aa.dupe(u8, s));
            if (gop.found_existing) continue;
            try level.append(aa, gop.key_ptr.*);
            try visits.append(aa, .{ .node = gop.key_ptr.*, .depth = 0 });
        }

        var d: u32 = 1;
        while (d <= depth and level.items.len > 0) : (d += 1) {
            var next: std.ArrayListUnmanaged([]const u8) = .empty;
            for (level.items) |node| {
                // Arena memory: freed wholesale at the end of traverse — NEVER
                // free per item (ArenaAllocator rewinds on last-allocation
                // frees and would corrupt sibling slices).
                const keys = try g.neighbors(aa, node, dir, types, scan_limit);
                if (keys.len == scan_limit) truncated = true; // heuristic
                for (keys) |k| {
                    // cap the frontier DURING the level build so visits stay
                    // proportional to the (capped) frontier
                    if (next.items.len >= frontier_limit) {
                        truncated = true;
                        break;
                    }
                    const gop = try visited.getOrPut(k.node);
                    if (!gop.found_existing) {
                        try next.append(aa, k.node); // frontier takes ownership
                        try visits.append(aa, .{ .node = k.node, .depth = d });
                    }
                }
            }
            level = next;
            if (truncated) break; // never expand a partial level
        }

        const out = try alloc.alloc(Visit, visits.items.len);
        errdefer alloc.free(out);
        var done: usize = 0;
        errdefer for (out[0..done]) |v| alloc.free(@constCast(v.node));
        for (visits.items, out) |v, *o| {
            o.* = .{ .node = try alloc.dupe(u8, v.node), .depth = v.depth };
            done += 1;
        }
        return .{ .visits = out, .truncated = truncated };
    }

    /// Shortest path src -> dst by BFS in `dir` (undirected search: .both).
    /// `budget` caps the number of node expansions. Memory ∝ reached nodes.
    pub fn shortestPath(g: *Graph, alloc: std.mem.Allocator, src: []const u8, dst: []const u8, dir: Direction, types: []const []const u8, budget: usize) GinaError!PathResult {
        if (std.mem.eql(u8, src, dst)) {
            const p = try alloc.alloc([]const u8, 1);
            errdefer alloc.free(p);
            p[0] = try alloc.dupe(u8, src);
            return .{ .path = p };
        }
        var arena = std.heap.ArenaAllocator.init(alloc);
        defer arena.deinit();
        const aa = arena.allocator();

        var parent = std.StringHashMap([]const u8).init(aa);
        var level: std.ArrayListUnmanaged([]const u8) = .empty;
        try level.append(aa, try aa.dupe(u8, src));
        try parent.put(level.items[0], "");
        var expansions: usize = 0;
        var found = false;

        while (level.items.len > 0 and !found) {
            var next: std.ArrayListUnmanaged([]const u8) = .empty;
            for (level.items) |node| {
                if (expansions >= budget) return .{ .exhausted = true };
                expansions += 1;
                // Arena memory: freed wholesale at the end — no per-item frees.
                const keys = try g.neighbors(aa, node, dir, types, std.math.maxInt(usize));
                for (keys) |k| {
                    const gop = try parent.getOrPut(k.node);
                    if (!gop.found_existing) {
                        gop.value_ptr.* = node;
                        try next.append(aa, k.node);
                        if (std.mem.eql(u8, k.node, dst)) {
                            found = true;
                            break;
                        }
                    }
                }
                if (found) break;
            }
            level = next;
        }

        if (!found) return .{};

        // reconstruct src -> dst walking parents backwards
        var rev: std.ArrayListUnmanaged([]const u8) = .empty;
        var cur: []const u8 = dst;
        while (cur.len > 0) {
            try rev.append(aa, cur);
            cur = parent.get(cur) orelse break;
        }
        const out = try alloc.alloc([]const u8, rev.items.len);
        errdefer alloc.free(out);
        var done: usize = 0;
        errdefer for (out[0..done]) |s| alloc.free(@constCast(s));
        var i: usize = rev.items.len;
        for (out) |*slot| {
            i -= 1;
            slot.* = try alloc.dupe(u8, rev.items[i]);
            done += 1;
        }
        return .{ .path = out };
    }

    /// Drops both backing tables. Best-effort (two DDL ops, not one tx) —
    /// documented in the block contract (docs/BLOQUE-B6-CONTRATO.md).
    pub fn dropGraph(g: *Graph) GinaError!void {
        try g.db.dropTable(g.out_table);
        try g.db.dropTable(g.in_table);
    }
};

// ---- pure codec tests (no engine needed) ------------------------------------

const testing = std.testing;

test "key codec: stuff/unstuff roundtrip" {
    const a = testing.allocator;
    const cases = [_][]const u8{
        "u1",         "",
        "a\x1Fb",     "\x1F",
        "\x1F\x1F",   "n12345",
        "weird \" q", "tab\tnew\nline",
        "é中文",       "\x01\x02\x03",
    };
    for (cases) |s| {
        var buf: std.ArrayListUnmanaged(u8) = .empty;
        defer buf.deinit(a);
        try stuffComponent(&buf, a, s);
        try testing.expect(buf.items[buf.items.len - 1] == SEP);
        const body = buf.items[0 .. buf.items.len - 1];
        const back = try unstuff(a, body);
        defer a.free(@constCast(back)); // unstuff always owns
        try testing.expectEqualStrings(s, back);
    }
}

test "key codec: parseKey splits three components (stuffed SEP inside)" {
    const a = testing.allocator;
    var buf: std.ArrayListUnmanaged(u8) = .empty;
    defer buf.deinit(a);
    try stuffComponent(&buf, a, "sr\x1Fc");
    try stuffComponent(&buf, a, "knows");
    try stuffComponent(&buf, a, "d\x1Fst");
    const parts = try parseKey(a, buf.items);
    defer {
        a.free(@constCast(parts.a));
        a.free(@constCast(parts.b));
        a.free(@constCast(parts.c));
    }
    try testing.expectEqualStrings("sr\x1Fc", parts.a);
    try testing.expectEqualStrings("knows", parts.b);
    try testing.expectEqualStrings("d\x1Fst", parts.c);
}

test "key codec: separator cannot be forged" {
    const a = testing.allocator;
    var k1: std.ArrayListUnmanaged(u8) = .empty;
    defer k1.deinit(a);
    try stuffComponent(&k1, a, "a");
    try stuffComponent(&k1, a, "\x1Fb");
    try stuffComponent(&k1, a, "c");
    var k2: std.ArrayListUnmanaged(u8) = .empty;
    defer k2.deinit(a);
    try stuffComponent(&k2, a, "a");
    try stuffComponent(&k2, a, "");
    try stuffComponent(&k2, a, "b\x1Fc"); // would collide without stuffing
    try testing.expect(!std.mem.eql(u8, k1.items, k2.items));
}

test "json scanner: extractIds unescapes \\u001f and friends" {
    const a = testing.allocator;
    const json =
        "[{\"id\":\"n1\\u001fknows\\u001fn2\\u001f\"},{\"id\":\"x\\ty\\\"z\"},{\"id\":\"plain\"}]";
    const ids = try extractIds(a, json);
    defer {
        for (ids) |id| a.free(@constCast(id));
        a.free(@constCast(ids));
    }
    try testing.expectEqual(@as(usize, 3), ids.len);
    try testing.expectEqualStrings("n1\x1fknows\x1fn2\x1f", ids[0]);
    try testing.expectEqualStrings("x\ty\"z", ids[1]);
    try testing.expectEqualStrings("plain", ids[2]);
}
