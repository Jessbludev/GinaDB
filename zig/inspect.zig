//! GinaDB Lite 0.7 — gina-inspect: offline CLI inspector (pure Zig reader).
//!
//!   gina-inspect <dir>                    summary: tables, rows, storage health
//!   gina-inspect <dir> <table>            dump a table as JSON lines
//!   gina-inspect <dir> <table> <key>      one row as JSON
//!   gina-inspect --wal <dir>              WAL frame walk
const std = @import("std");
const ginaread = @import("ginaread.zig");

pub fn main(init: std.process.Init.Minimal) !void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const a = arena.allocator();

    var threaded = std.Io.Threaded.init(a, .{ .environ = init.environ });
    defer threaded.deinit();
    const io = threaded.io();
    var wbuf: [8192]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &wbuf);
    defer out.interface.flush() catch {};

    // argv
    var args_it = std.process.Args.Iterator.init(init.args);
    var args: std.ArrayListUnmanaged([]const u8) = .empty;
    while (args_it.next()) |arg| try args.append(a, arg);

    if (args.items.len < 2) {
        try out.interface.print("usage: gina-inspect <dir> [<table> [<key>]] | --wal <dir>\n", .{});
        return;
    }

    if (std.mem.eql(u8, args.items[1], "--wal")) {
        if (args.items.len < 3) return error.MissingDir;
        return walDump(a, args.items[2], &out);
    }

    const dir = args.items[1];
    var db = ginaread.Db.open(a, dir) catch |e| {
        try out.interface.print("error: cannot open {s}: {s}\n", .{ dir, @errorName(e) });
        return e;
    };
    defer db.deinit();

    if (args.items.len >= 4) {
        const row = db.get(args.items[2], args.items[3]) orelse {
            try out.interface.print("null\n", .{});
            return;
        };
        const json = try ginaread.rowToJson(a, row);
        try out.interface.print("{s}\n", .{json});
        return;
    }
    if (args.items.len == 3) {
        const table = db.tables.get(args.items[2]) orelse {
            try out.interface.print("error: no such table '{s}'\n", .{args.items[2]});
            return error.NoSuchTable;
        };
        var rows = table.rows.iterator();
        while (rows.next()) |e| {
            const json = try ginaread.rowToJson(a, e.value_ptr);
            try out.interface.print("{s}\n", .{json});
        }
        return;
    }

    // summary
    try out.interface.print("GinaDB Lite 0.7 (Zig reader) — {s}\n", .{dir});
    try out.interface.print("  tables: {d}\n", .{db.tables.count()});
    var total_rows: usize = 0;
    var it = db.tables.iterator();
    while (it.next()) |e| {
        total_rows += e.value_ptr.rows.count();
        try out.interface.print("  - {s}: {d} rows (pk='{s}', schemaless={}, indexes={d})\n", .{
            e.key_ptr.*,
            e.value_ptr.rows.count(),
            e.value_ptr.def.primary_key,
            e.value_ptr.def.schemaless,
            e.value_ptr.def.indexes.len,
        });
    }
    try out.interface.print("  rows total: {d}\n", .{total_rows});
    try out.interface.print("  last_lsn: {d}  checkpoint_lsn: {d}\n", .{ db.last_lsn, db.checkpoint_lsn });
    try out.interface.print("  snapshot: {d} pages (+{d} overflow)\n", .{ db.snapshot_pages, db.snapshot_overflow });
    try out.interface.print("  delta: seq={d} watermark={d} pages={d} live={d} free={d} suspect={d} corrupt={d} broken_ovf={d} records={d}\n", .{
        db.delta.seq, db.delta.watermark, db.delta.page_count,
        db.delta.live_pages, db.delta.free_pages,
        db.delta.suspect_pages, db.delta.corrupt_pages,
        db.delta.broken_ovf_records, db.delta.records,
    });
    try out.interface.print("  wal: {d} frames, bad tail: {d} bytes\n", .{ db.wal_frames, db.wal_bad_tail });
}

fn walDump(a: std.mem.Allocator, dir: []const u8, out: anytype) !void {
    const path = try std.fmt.allocPrint(a, "{s}/wal.log", .{dir});
    var threaded = std.Io.Threaded.init(a, .{});
    defer threaded.deinit();
    const io = threaded.io();
    const data = std.Io.Dir.cwd().readFileAlloc(io, path, a, .limited(512 << 20)) catch |e| {
        try out.interface.print("error: cannot read {s}: {s}\n", .{ path, @errorName(e) });
        return e;
    };
    const res = try ginaread.WalResult.parse(a, data);
    try out.interface.print("WAL {s}: {d} frames, {d} good bytes, {d} bad tail bytes\n", .{
        path, res.frames.len, res.good_bytes, res.bad_tail_bytes,
    });
    for (res.frames) |fr| {
        try out.interface.print("  lsn={d:>6} tx={d:>4} kind={s:>7} v{d} payload={d}B\n", .{
            fr.lsn, fr.tx, @tagName(fr.kind), fr.version, fr.payload.len,
        });
    }
    if (res.bad_tail_bytes > 0)
        try out.interface.print("  (torn/corrupt tail of {d} bytes — repaired on open by the engine)\n", .{res.bad_tail_bytes});
}
