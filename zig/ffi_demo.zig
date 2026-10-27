//! GinaDB Lite 0.7 — Zig FFI demo over libginadb.so (the C++ engine).
//! Build the C++ shared library first (make lib), then: zig build ffi-demo
//! Run: ./zig-out/bin/gina-ffi-demo <db-dir>
const std = @import("std");
const gina = @import("gina.zig");

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

    var args_it = std.process.Args.Iterator.init(init.args);
    var args: std.ArrayListUnmanaged([]const u8) = .empty;
    while (args_it.next()) |arg| try args.append(a, arg);
    const dir = if (args.items.len > 1) args.items[1] else "zig-ffi-demo-data";
    std.Io.Dir.cwd().deleteTree(io, dir) catch {};

    var db = try gina.Database.open(dir, .{});
    defer db.close();

    try out.interface.print("engine: {s}\n", .{gina.Database.version()});

    // DDL
    try db.createTable("media", &.{
        .{ .name = "id", .type = .text, .nullable = false },
        .{ .name = "tag", .type = .text },
        .{ .name = "vec", .type = .vector },
    }, "id", &.{});
    try db.createTable("users", &.{
        .{ .name = "id", .type = .text, .nullable = false },
        .{ .name = "email", .type = .text },
        .{ .name = "age", .type = .int },
    }, "id", &.{ "email", "age" });

    // multimodal insert (vector tagged as {"$vec":[...]})
    try db.insert("media", "{\"id\":\"m1\",\"tag\":\"gina\",\"vec\":{\"$vec\":[0.1,0.2,0.3]}}");
    try db.insert("media", "{\"id\":\"m2\",\"tag\":\"db\",\"vec\":{\"$vec\":[0.9,0.1,0.0]}}");
    try db.insert("media", "{\"id\":\"m3\",\"tag\":\"lite\",\"vec\":{\"$vec\":[0.12,0.2,0.3]}}");

    // read back
    if (try db.get("media", "m1")) |row| {
        try out.interface.print("get(m1) = {s}\n", .{row.span()});
        row.free();
    }

    // KNN through the C ABI
    const q = [_]f32{ 0.1, 0.2, 0.3 };
    const nn = try db.knn("media", "vec", &q, 2, .cosine);
    try out.interface.print("knn(vec,q,k=2) = {s}\n", .{nn.span()});
    nn.free();

    // atomic batch
    try db.exec("[{\"op\":\"del\",\"table\":\"media\",\"key\":\"m2\"}]");
    const n = try db.count("media");
    try out.interface.print("count after batch del = {d}\n", .{n});

    // ---- 0.6: índices secundarios + select con planner + compactación --------
    try db.exec("[{\"op\":\"put\",\"table\":\"users\",\"row\":{\"id\":\"u1\",\"email\":\"gina@db.io\",\"age\":33}}," ++
        "{\"op\":\"put\",\"table\":\"users\",\"row\":{\"id\":\"u2\",\"email\":\"ada@db.io\",\"age\":36}}," ++
        "{\"op\":\"put\",\"table\":\"users\",\"row\":{\"id\":\"u3\",\"email\":\"lin@db.io\",\"age\":21}}]");

    const eq = try db.select("users",
        "{\"where\":{\"op\":\"eq\",\"col\":\"email\",\"val\":\"ada@db.io\"}}");
    try out.interface.print("select eq email (índice) = {s}\n", .{eq.span()});
    eq.free();

    const rng = try db.select("users",
        "{\"where\":{\"op\":\"between\",\"col\":\"age\",\"lo\":30,\"hi\":40}," ++
        "\"\":[],\"fields\":[\"id\",\"age\"]}");
    _ = rng;
    const rng2 = try db.select("users",
        "{\"where\":{\"op\":\"between\",\"col\":\"age\",\"lo\":30,\"hi\":40},\"fields\":[\"id\",\"age\"]}");
    try out.interface.print("select rango age (índice) = {s}\n", .{rng2.span()});
    rng2.free();

    // índice dinámico + persistencia vía checkpoint incremental
    try db.createIndex("media", "tag");
    try db.checkpoint();
    try db.compact();
    try out.interface.print("compactación OK\n", .{});

    const st = try db.stats();
    try out.interface.print("stats = {s}\n", .{st.span()});
    st.free();
    try out.interface.print("Zig FFI demo OK\n", .{});
}
