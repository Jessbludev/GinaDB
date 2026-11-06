//! GinaDB — Block B6 graph demo + bench over libginadb.so (the C++ engine).
//! Build the C++ shared library first (`make lib`), then `zig build graph-demo`.
//!
//!   ./zig-out/bin/gina-graph <db-dir>                     # social demo
//!   ./zig-out/bin/gina-graph <db-dir> --bench 1000000 10   # 10^6-edge bench
//!
//! The bench builds a directed ring lattice (node i -> i+1..i+K mod M), the
//! only graph family whose BFS levels are known analytically: level d is
//! exactly {(d-1)K+1 .. dK}, so the depth-3 traversal result is asserted,
//! not just printed (B6 acceptance: bounded latency, frontier memory).

const std = @import("std");
const gina = @import("gina.zig");
const graph = @import("graph.zig");

pub fn main(init: std.process.Init.Minimal) !void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const a = arena.allocator();

    var threaded = std.Io.Threaded.init(a, .{ .environ = init.environ });
    defer threaded.deinit();
    const io = threaded.io();
    var wbuf: [16384]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &wbuf);
    defer out.interface.flush() catch {};

    var args_it = std.process.Args.Iterator.init(init.args);
    var args: std.ArrayListUnmanaged([]const u8) = .empty;
    while (args_it.next()) |arg| try args.append(a, arg);
    const dir = if (args.items.len > 1) args.items[1] else "zig-graph-demo-data";

    var bench_edges: usize = 0;
    var fanout: usize = 10;
    if (args.items.len > 3 and std.mem.eql(u8, args.items[2], "--bench")) {
        bench_edges = try std.fmt.parseInt(usize, args.items[3], 10);
        if (args.items.len > 4) fanout = try std.fmt.parseInt(usize, args.items[4], 10);
    }

    std.Io.Dir.cwd().deleteTree(io, dir) catch {};
    try out.interface.print("engine: {s}\n", .{gina.Database.version()});

    if (bench_edges > 0) {
        try runBench(a, io, &out, dir, bench_edges, fanout);
        try out.interface.print("Zig graph bench OK\n", .{});
    } else {
        try runDemo(a, &out, dir);
        try out.interface.print("Zig graph demo OK\n", .{});
    }
}

// ---- demo -------------------------------------------------------------------------

fn runDemo(a: std.mem.Allocator, out: anytype, dir: []const u8) !void {
    try out.interface.print("--- B6 graph layer demo (graph \"social\") ---\n", .{});
    {
        var db = try gina.Database.open(dir, .{});
        defer db.close();
        var g = try graph.Graph.init(&db, a, "social");
        defer g.deinit();

        // B6: nodes are rows of any table (here: a regular users table)
        try db.createTable("users", &.{
            .{ .name = "id", .type = .text, .nullable = false },
            .{ .name = "name", .type = .text },
        }, "id", &.{});
        for ([_][]const u8{ "ada", "bob", "cyd", "dee", "eli", "fen" }, 0..) |nm, i| {
            const row = try a.print("{{\"id\":\"u{d}\",\"name\":\"{s}\"}}", .{ i + 1, nm });
            try db.insert("users", row);
        }

        // atomic double-entry edges (one gina_exec per batch)
        try g.addEdges(&.{
            .{ .src = "u1", .edge_type = "knows", .dst = "u2" },
            .{ .src = "u1", .edge_type = "knows", .dst = "u3", .props = "{\"since\":2026}" },
            .{ .src = "u2", .edge_type = "knows", .dst = "u4" },
            .{ .src = "u3", .edge_type = "knows", .dst = "u4" },
            .{ .src = "u4", .edge_type = "knows", .dst = "u5" },
            .{ .src = "u2", .edge_type = "follows", .dst = "u6" },
        });
        try out.interface.print("edges: {d} (out) / {d} (in)\n", .{ try g.edgeCount(), try db.count("g.social.in") });

        const ns = try g.neighbors(a, "u1", .out, &.{}, 100);
        defer {
            for (ns) |n| {
                a.free(@constCast(n.node));
                a.free(@constCast(n.edge_type));
            }
            a.free(@constCast(ns));
        }
        try out.interface.print("neighbors(u1, out) = [", .{});
        for (ns, 0..) |n, i| {
            try out.interface.print("{s}{s}({s})", .{ if (i > 0) ", " else "", n.node, n.edge_type });
        }
        try out.interface.print("]\n", .{});

        const fin = try g.neighbors(a, "u4", .in, &.{"knows"}, 100);
        defer {
            for (fin) |n| {
                a.free(@constCast(n.node));
                a.free(@constCast(n.edge_type));
            }
            a.free(@constCast(fin));
        }
        try out.interface.print("neighbors(u4, in, knows) = [{s}]\n", .{if (fin.len > 0) fin[0].node else ""});

        const tr = try g.traverse(a, &.{"u1"}, .out, &.{"knows"}, 3, 100, 100);
        defer {
            for (tr.visits) |v| a.free(@constCast(v.node));
            a.free(@constCast(tr.visits));
        }
        try out.interface.print("traverse(u1, knows, depth 3): {d} nodes [", .{tr.visits.len});
        for (tr.visits, 0..) |v, i| {
            if (i > 0) try out.interface.print(" ", .{});
            try out.interface.print("{s}@{d}", .{ v.node, v.depth });
        }
        try out.interface.print("] truncated={}\n", .{tr.truncated});

        const p = try g.shortestPath(a, "u1", "u6", .both, &.{}, 1000);
        if (p.path) |path| {
            defer {
                for (path) |s| a.free(@constCast(s));
                a.free(@constCast(path));
            }
            try out.interface.print("shortest(u1 -> u6, both) = len {d}: ", .{path.len});
            for (path, 0..) |s, i| try out.interface.print("{s}{s}", .{ if (i > 0) " -> " else "", s });
            try out.interface.print("\n", .{});
        }

        if (try g.getEdge("u1", "knows", "u3")) |row| {
            defer row.free();
            try out.interface.print("getEdge(u1,knows,u3) = {s}\n", .{row.span()});
        }
        _ = try g.setEdgeProps("u1", "knows", "u3", "{\"since\":2027,\"weight\":2}");
        if (try g.getEdge("u1", "knows", "u3")) |row| {
            defer row.free();
            try out.interface.print("after setEdgeProps   = {s}\n", .{row.span()});
        }

        try g.removeEdge("u2", "knows", "u4");
        try out.interface.print("after remove(u2 knows u4): edges={d}\n", .{try g.edgeCount()});
    }

    // restart: the graph must survive on disk (WAL/snapshot), tables persist
    {
        var db = try gina.Database.open(dir, .{});
        defer db.close();
        var g = try graph.Graph.init(&db, a, "social");
        defer g.deinit();
        const tr = try g.traverse(a, &.{"u1"}, .out, &.{"knows"}, 3, 100, 100);
        defer {
            for (tr.visits) |v| a.free(@constCast(v.node));
            a.free(@constCast(tr.visits));
        }
        try out.interface.print("after restart: edges={d}, traverse(u1)={d} nodes\n", .{ try g.edgeCount(), tr.visits.len });
    }
}

// ---- bench ------------------------------------------------------------------------

fn msSince(io: std.Io, t0: std.Io.Timestamp) u64 {
    const d = t0.durationTo(std.Io.Timestamp.now(io, .awake));
    return @intCast(@divTrunc(d.nanoseconds, std.time.ns_per_ms));
}

fn runBench(a: std.mem.Allocator, io: std.Io, out: anytype, dir: []const u8, edges: usize, fanout: usize) !void {
    const nodes = edges / fanout;
    const depth = 3;
    try out.interface.print("--- B6 bench: ring lattice, {d} edges, fanout {d}, {d} nodes ---\n", .{ edges, fanout, nodes });

    var t0 = std.Io.Timestamp.now(io, .awake);
    {
        var db = try gina.Database.open(dir, .{});
        var g = try graph.Graph.init(&db, a, "lattice");
        defer g.deinit();

        var batch: std.ArrayListUnmanaged(graph.Edge) = .empty;
        defer batch.deinit(a);
        var i: usize = 0;
        while (i < edges) {
            batch.clearRetainingCapacity();
            const end = @min(i + 250, edges);
            for (i..end) |ei| {
                const src = try a.print("n{d}", .{ei / fanout});
                const dst = try a.print("n{d}", .{(ei / fanout + 1 + (ei % fanout)) % nodes});
                try batch.append(a, .{ .src = src, .edge_type = "link", .dst = dst });
            }
            try g.addEdges(batch.items);
            i = end;
        }
        db.close(); // explicit: reopen below measures WAL/snapshot load
    }
    const insert_ms = msSince(io, t0);
    try out.interface.print("insert: {d} ms ({d} edges/s, atomic batches of 250)\n", .{ insert_ms, edges * 1000 / @max(1, insert_ms) });

    // reopen (cold read of WAL + snapshot)
    t0 = std.Io.Timestamp.now(io, .awake);
    var db = try gina.Database.open(dir, .{});
    defer db.close();
    const reopen_ms = msSince(io, t0);
    var g = try graph.Graph.init(&db, a, "lattice");
    defer g.deinit();
    try out.interface.print("reopen: {d} ms — edgeCount={d}\n", .{ reopen_ms, try g.edgeCount() });
    try testing.expectEqual(@as(i64, @intCast(edges)), try g.edgeCount());

    // depth-3 traversal from n0: analytic result {n0} ∪ {n1..3K}
    const scan_limit = fanout + 2;
    t0 = std.Io.Timestamp.now(io, .awake);
    const tr = try g.traverse(a, &.{"n0"}, .out, &.{"link"}, depth, nodes, scan_limit);
    const trav_ms = msSince(io, t0);
    defer {
        for (tr.visits) |v| a.free(@constCast(v.node));
        a.free(@constCast(tr.visits));
    }
    var per_depth = [_]usize{ 0, 0, 0, 0 };
    for (tr.visits) |v| per_depth[@min(v.depth, 3)] += 1;
    try out.interface.print("traverse(n0, depth 3): {d} nodes in {d} ms — frontier by level: d1={d} d2={d} d3={d} truncated={}\n", .{ tr.visits.len, trav_ms, per_depth[1], per_depth[2], per_depth[3], tr.truncated });

    // B6 acceptance: latency bounded by frontier (not by graph size) and the
    // analytic BFS of the lattice holds: each level holds exactly `fanout`.
    try testing.expect(!tr.truncated);
    try testing.expectEqual(@as(usize, 1), per_depth[0]);
    try testing.expectEqual(fanout, per_depth[1]);
    try testing.expectEqual(fanout, per_depth[2]);
    try testing.expectEqual(fanout, per_depth[3]);
    try testing.expectEqual(@as(usize, 1 + depth * fanout), tr.visits.len);

    // shortest path n0 -> n(4K): exactly 4 hops in the lattice
    const target = try a.print("n{d}", .{4 * fanout});
    const p = try g.shortestPath(a, "n0", target, .out, &.{"link"}, nodes);
    if (p.path) |path| {
        defer {
            for (path) |s| a.free(@constCast(s));
            a.free(@constCast(path));
        }
        try out.interface.print("shortest(n0 -> {s}) = {d} hops\n", .{ target, path.len - 1 });
        try testing.expectEqual(@as(usize, 5), path.len);
    } else {
        try out.interface.print("shortest(n0 -> {s}): NOT FOUND (exhausted={})\n", .{ target, p.exhausted });
        return error.TestUnexpectedResult;
    }

    // supernode: a hub with 20k out-edges must be contained by frontier_limit
    {
        var hub: std.ArrayListUnmanaged(graph.Edge) = .empty;
        defer hub.deinit(a);
        for (0..20_000) |hi| {
            const dst = try a.print("x{d}", .{hi});
            try hub.append(a, .{ .src = "H", .edge_type = "link", .dst = dst });
        }
        // batched through the same atomic path
        var k: usize = 0;
        while (k < hub.items.len) {
            const end = @min(k + 250, hub.items.len);
            try g.addEdges(hub.items[k..end]);
            k = end;
        }
        t0 = std.Io.Timestamp.now(io, .awake);
        const htr = try g.traverse(a, &.{"H"}, .out, &.{"link"}, 2, 100, std.math.maxInt(usize));
        const hms = msSince(io, t0);
        defer {
            for (htr.visits) |v| a.free(@constCast(v.node));
            a.free(@constCast(htr.visits));
        }
        try out.interface.print("supernode H (20k edges): depth-2 traversal capped -> {d} nodes in {d} ms, truncated={}\n", .{ htr.visits.len, hms, htr.truncated });
        try testing.expect(htr.truncated);
        try testing.expectEqual(@as(usize, 101), htr.visits.len); // H + 100 frontier cap
    }

    // engine-side footprint (in-RAM engine: H7, bounded by B2 later)
    const st = try db.stats();
    defer st.free();
    try out.interface.print("engine stats: {s}\n", .{st.span()});
}

const testing = std.testing;
