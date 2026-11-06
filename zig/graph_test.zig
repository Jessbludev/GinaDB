//! GinaDB — Block B6 graph layer tests (need libginadb.so built: `make lib`).
//! Run: zig build graph-test
//!
//! These tests PIN the on-disk contract of the layer: edge keys are built by
//! hand in some tests (e.g. "u1\x1Fknows\x1Fu2\x1F") so that a future change
//! of the key layout is caught here (P1: the format is the contract).

const std = @import("std");
const gina = @import("gina.zig");
const graph = @import("graph.zig");

test {
    // include the pure codec tests declared inside graph.zig
    _ = @import("graph.zig");
}

const testing = std.testing;

fn freshDir(dir: []const u8) !void {
    var threaded = std.Io.Threaded.init(testing.allocator, .{});
    defer threaded.deinit();
    const io = threaded.io();
    try std.Io.Dir.cwd().deleteTree(io, dir);
}

fn freeNeighbors(a: std.mem.Allocator, ns: []graph.Neighbor) void {
    for (ns) |n| {
        a.free(@constCast(n.node));
        a.free(@constCast(n.edge_type));
    }
    a.free(@constCast(ns));
}

fn freeVisits(a: std.mem.Allocator, vs: []graph.Visit) void {
    for (vs) |v| a.free(@constCast(v.node));
    a.free(@constCast(vs));
}

fn freePath(a: std.mem.Allocator, p: []const []const u8) void {
    for (p) |s| a.free(@constCast(s));
    a.free(@constCast(p));
}

test "graph: double entry, edgeCount, idempotent re-init" {
    const a = testing.allocator;
    const dir = ".zig-cache/gt/basic";
    try freshDir(dir);
    var db = try gina.Database.open(dir, .{});
    defer db.close();
    {
        var g = try graph.Graph.init(&db, a, "social");
        defer g.deinit();
        try g.addEdge("u1", "knows", "u2", null);
        try g.addEdge("u1", "knows", "u3", null);

        try testing.expectEqual(@as(i64, 2), try g.edgeCount());
        try testing.expectEqual(@as(i64, 2), try db.count("g.social.in"));

        // both entries exist and carry the pinned key layout
        if (try db.get("g.social.out", "u1\x1Fknows\x1Fu2\x1F")) |row| {
            defer row.free();
            try testing.expect(std.mem.indexOf(u8, row.span(), "\"etype\":\"knows\"") != null);
        } else return error.TestUnexpectedResult;
        if (try db.get("g.social.in", "u2\x1Fknows\x1Fu1\x1F")) |row| {
            defer row.free();
            try testing.expect(std.mem.indexOf(u8, row.span(), "\"src\":\"u1\"") != null);
        } else return error.TestUnexpectedResult;

        // re-init must tolerate existing tables
        var g2 = try graph.Graph.init(&db, a, "social");
        defer g2.deinit();
        try testing.expectEqual(@as(i64, 2), try g2.edgeCount());
    }
}

test "graph: prefix scan is exact — n1 never leaks n10/n11" {
    const a = testing.allocator;
    const dir = ".zig-cache/gt/prefix";
    try freshDir(dir);
    var db = try gina.Database.open(dir, .{});
    defer db.close();
    var g = try graph.Graph.init(&db, a, "p");
    defer g.deinit();

    try g.addEdges(&.{
        .{ .src = "n1", .edge_type = "knows", .dst = "x" },
        .{ .src = "n10", .edge_type = "knows", .dst = "y" },
        .{ .src = "n11", .edge_type = "knows", .dst = "z" },
        .{ .src = "n1", .edge_type = "follows", .dst = "w" },
    });

    const ns = try g.neighbors(a, "n1", .out, &.{}, 100);
    defer freeNeighbors(a, ns);
    try testing.expectEqual(@as(usize, 2), ns.len);
    for (ns) |n| {
        try testing.expect(std.mem.eql(u8, "knows", n.edge_type) or
            std.mem.eql(u8, "follows", n.edge_type));
        try testing.expect(!std.mem.eql(u8, "y", n.node) and !std.mem.eql(u8, "z", n.node));
    }
}

test "graph: atomicity — rejected batch leaves no partial edge" {
    const a = testing.allocator;
    const dir = ".zig-cache/gt/atomic";
    try freshDir(dir);
    var db = try gina.Database.open(dir, .{});
    defer db.close();
    var g = try graph.Graph.init(&db, a, "a");
    defer g.deinit();

    // props must be a JSON object; an array violates the out-row schema and
    // the WHOLE batch (both entries) must be rejected.
    try testing.expectError(error.SchemaViolation, g.addEdges(&.{
        .{ .src = "u1", .edge_type = "knows", .dst = "u2" },
        .{ .src = "u1", .edge_type = "knows", .dst = "u3", .props = "[1,2,3]" },
    }));
    // nothing landed: the first edge must NOT exist in either table
    try testing.expectEqual(@as(i64, 0), try g.edgeCount());
    try testing.expectEqual(@as(i64, 0), try db.count("g.a.in"));
    try testing.expect((try g.getEdge("u1", "knows", "u2")) == null);
    // a valid batch still works afterwards
    try g.addEdge("u1", "knows", "u2", null);
    try testing.expectEqual(@as(i64, 1), try g.edgeCount());
}

test "graph: neighbors directions, both, and type filter" {
    const a = testing.allocator;
    const dir = ".zig-cache/gt/neighbors";
    try freshDir(dir);
    var db = try gina.Database.open(dir, .{});
    defer db.close();
    var g = try graph.Graph.init(&db, a, "n");
    defer g.deinit();

    try g.addEdges(&.{
        .{ .src = "a", .edge_type = "knows", .dst = "b" },
        .{ .src = "a", .edge_type = "follows", .dst = "c" },
        .{ .src = "b", .edge_type = "knows", .dst = "c" },
    });

    const out = try g.neighbors(a, "a", .out, &.{}, 100);
    defer freeNeighbors(a, out);
    try testing.expectEqual(@as(usize, 2), out.len);

    const typed = try g.neighbors(a, "a", .out, &.{"knows"}, 100);
    defer freeNeighbors(a, typed);
    try testing.expectEqual(@as(usize, 1), typed.len);
    try testing.expectEqualStrings("b", typed[0].node);
    try testing.expectEqualStrings("knows", typed[0].edge_type);

    const in = try g.neighbors(a, "c", .in, &.{}, 100);
    defer freeNeighbors(a, in);
    try testing.expectEqual(@as(usize, 2), in.len);

    // mutual edges: x->y and y->x => neighbors(x, .both) sees y twice (two edges)
    try g.addEdges(&.{
        .{ .src = "x", .edge_type = "knows", .dst = "y" },
        .{ .src = "y", .edge_type = "knows", .dst = "x" },
    });
    const both = try g.neighbors(a, "x", .both, &.{}, 100);
    defer freeNeighbors(a, both);
    try testing.expectEqual(@as(usize, 2), both.len);

    // a node with no edges yields an empty (non-error) neighborhood
    const none = try g.neighbors(a, "zz", .out, &.{}, 100);
    defer freeNeighbors(a, none);
    try testing.expectEqual(@as(usize, 0), none.len);
}

test "graph: NUL bytes in ids are rejected loudly" {
    const a = testing.allocator;
    const dir = ".zig-cache/gt/nul";
    try freshDir(dir);
    var db = try gina.Database.open(dir, .{});
    defer db.close();
    var g = try graph.Graph.init(&db, a, "nul");
    defer g.deinit();
    try testing.expectError(error.InvalidArgument, g.addEdge("a\x00b", "knows", "c", null));
    try testing.expectError(error.InvalidArgument, g.addEdge("a", "kn\x00ows", "c", null));
    try testing.expectEqual(@as(i64, 0), try g.edgeCount());
}

test "graph: traverse depths, cycles and type filter" {
    const a = testing.allocator;
    const dir = ".zig-cache/gt/traverse";
    try freshDir(dir);
    var db = try gina.Database.open(dir, .{});
    defer db.close();
    var g = try graph.Graph.init(&db, a, "t");
    defer g.deinit();

    try g.addEdges(&.{
        .{ .src = "a", .edge_type = "knows", .dst = "b" },
        .{ .src = "b", .edge_type = "knows", .dst = "c" },
        .{ .src = "c", .edge_type = "knows", .dst = "d" },
        .{ .src = "b", .edge_type = "knows", .dst = "a" }, // cycle a<->b
        .{ .src = "a", .edge_type = "follows", .dst = "e" },
        .{ .src = "e", .edge_type = "follows", .dst = "f" },
    });

    const r1 = try g.traverse(a, &.{"a"}, .out, &.{}, 1, 100, 100);
    defer freeVisits(a, r1.visits);
    try testing.expect(!r1.truncated);
    try testing.expectEqual(@as(usize, 3), r1.visits.len); // a, b (knows), e (follows)

    const r2 = try g.traverse(a, &.{"a"}, .out, &.{"knows"}, 2, 100, 100);
    defer freeVisits(a, r2.visits);
    try testing.expectEqual(@as(usize, 3), r2.visits.len); // a, b, c (cycle absorbed)

    const r3 = try g.traverse(a, &.{"a"}, .out, &.{"knows"}, 5, 100, 100);
    defer freeVisits(a, r3.visits);
    try testing.expectEqual(@as(usize, 4), r3.visits.len); // a,b,c,d — cycle does not loop

    const rf = try g.traverse(a, &.{"a"}, .out, &.{"follows"}, 2, 100, 100);
    defer freeVisits(a, rf.visits);
    try testing.expectEqual(@as(usize, 3), rf.visits.len); // a, e, f

    // depth-by-depth check on the chain knows: a=0, b=1, c=2, d=3
    for (r3.visits) |v| {
        const want: u32 = switch (v.node[0]) {
            'a' => 0,
            'b' => 1,
            'c' => 2,
            'd' => 3,
            else => return error.TestUnexpectedResult,
        };
        try testing.expectEqual(want, v.depth);
    }
}

test "graph: frontier limit and supernode scan limit truncate" {
    const a = testing.allocator;
    const dir = ".zig-cache/gt/limits";
    try freshDir(dir);
    var db = try gina.Database.open(dir, .{});
    defer db.close();
    var g = try graph.Graph.init(&db, a, "l");
    defer g.deinit();

    // star of 50 out-edges from S (ids stay alive until after addEdges)
    var edges: std.ArrayListUnmanaged(graph.Edge) = .empty;
    var ids: std.ArrayListUnmanaged([]const u8) = .empty;
    defer {
        for (ids.items) |s| a.free(@constCast(s));
        ids.deinit(a);
        edges.deinit(a);
    }
    for (0..50) |i| {
        const dst = try a.print("s{d}", .{i});
        try ids.append(a, dst);
        try edges.append(a, .{ .src = "S", .edge_type = "knows", .dst = dst });
    }
    try g.addEdges(edges.items);

    const r = try g.traverse(a, &.{"S"}, .out, &.{}, 2, 20, 100);
    defer freeVisits(a, r.visits);
    try testing.expect(r.truncated);
    try testing.expectEqual(@as(usize, 21), r.visits.len); // S + 20 capped frontier

    // hub with 60 edges, per-node scan cap 10 => 10 neighbors reported
    var hub: std.ArrayListUnmanaged(graph.Edge) = .empty;
    var hids: std.ArrayListUnmanaged([]const u8) = .empty;
    defer {
        for (hids.items) |s| a.free(@constCast(s));
        hids.deinit(a);
        hub.deinit(a);
    }
    for (0..60) |i| {
        const dst = try a.print("h{d}", .{i});
        try hids.append(a, dst);
        try hub.append(a, .{ .src = "H", .edge_type = "knows", .dst = dst });
    }
    try g.addEdges(hub.items);
    const nh = try g.neighbors(a, "H", .out, &.{}, 10);
    defer freeNeighbors(a, nh);
    try testing.expectEqual(@as(usize, 10), nh.len);
}

test "graph: shortest path, budget, unreachable, src==dst" {
    const a = testing.allocator;
    const dir = ".zig-cache/gt/path";
    try freshDir(dir);
    var db = try gina.Database.open(dir, .{});
    defer db.close();
    var g = try graph.Graph.init(&db, a, "p");
    defer g.deinit();

    try g.addEdges(&.{
        .{ .src = "a", .edge_type = "knows", .dst = "b" },
        .{ .src = "b", .edge_type = "knows", .dst = "c" },
        .{ .src = "c", .edge_type = "knows", .dst = "d" },
    });

    const r = try g.shortestPath(a, "a", "d", .out, &.{}, 100);
    try testing.expect(r.path != null);
    defer freePath(a, r.path.?);
    try testing.expectEqual(@as(usize, 4), r.path.?.len);
    try testing.expectEqualStrings("a", r.path.?[0]);
    try testing.expectEqualStrings("b", r.path.?[1]);
    try testing.expectEqualStrings("d", r.path.?[3]);

    const same = try g.shortestPath(a, "a", "a", .out, &.{}, 100);
    try testing.expect(same.path != null);
    defer freePath(a, same.path.?);
    try testing.expectEqual(@as(usize, 1), same.path.?.len);

    const dead = try g.shortestPath(a, "d", "a", .out, &.{}, 100);
    try testing.expect(dead.path == null);
    try testing.expect(!dead.exhausted);

    const budgeted = try g.shortestPath(a, "a", "d", .out, &.{}, 2);
    try testing.expect(budgeted.exhausted);

    const und = try g.shortestPath(a, "d", "a", .both, &.{}, 100);
    try testing.expect(und.path != null);
    defer freePath(a, und.path.?);
    try testing.expectEqual(@as(usize, 4), und.path.?.len);
}

test "graph: props roundtrip and update" {
    const a = testing.allocator;
    const dir = ".zig-cache/gt/props";
    try freshDir(dir);
    var db = try gina.Database.open(dir, .{});
    defer db.close();
    var g = try graph.Graph.init(&db, a, "w");
    defer g.deinit();

    try g.addEdge("u1", "knows", "u2", "{\"weight\":3,\"since\":\"2026\"}");
    if (try g.getEdge("u1", "knows", "u2")) |row| {
        defer row.free();
        try testing.expect(std.mem.indexOf(u8, row.span(), "\"weight\":3") != null);
    } else return error.TestUnexpectedResult;

    try testing.expect(try g.setEdgeProps("u1", "knows", "u2", "{\"weight\":7}"));
    if (try g.getEdge("u1", "knows", "u2")) |row| {
        defer row.free();
        try testing.expect(std.mem.indexOf(u8, row.span(), "\"weight\":7") != null);
        try testing.expect(std.mem.indexOf(u8, row.span(), "2026") == null);
    } else return error.TestUnexpectedResult;

    try testing.expect(!(try g.setEdgeProps("u1", "knows", "ghost", "{\"weight\":1}")));
}

test "graph: removeEdge is atomic and idempotent" {
    const a = testing.allocator;
    const dir = ".zig-cache/gt/remove";
    try freshDir(dir);
    var db = try gina.Database.open(dir, .{});
    defer db.close();
    var g = try graph.Graph.init(&db, a, "r");
    defer g.deinit();

    try g.addEdge("u1", "knows", "u2", null);
    try g.removeEdge("u1", "knows", "u2");
    try testing.expectEqual(@as(i64, 0), try g.edgeCount());
    try testing.expectEqual(@as(i64, 0), try db.count("g.r.in"));
    try testing.expect((try g.getEdge("u1", "knows", "u2")) == null);
    // removing again is fine (del of a missing key is not an error)
    try g.removeEdge("u1", "knows", "u2");
}

test "graph: degree in every direction" {
    const a = testing.allocator;
    const dir = ".zig-cache/gt/degree";
    try freshDir(dir);
    var db = try gina.Database.open(dir, .{});
    defer db.close();
    var g = try graph.Graph.init(&db, a, "d");
    defer g.deinit();

    try g.addEdges(&.{
        .{ .src = "a", .edge_type = "knows", .dst = "b" },
        .{ .src = "a", .edge_type = "knows", .dst = "c" },
        .{ .src = "d", .edge_type = "knows", .dst = "a" },
    });
    try testing.expectEqual(@as(usize, 2), try g.degree(a, "a", .out));
    try testing.expectEqual(@as(usize, 1), try g.degree(a, "a", .in));
    try testing.expectEqual(@as(usize, 3), try g.degree(a, "a", .both));
}

test "graph: restart persistence (tables already exist)" {
    const a = testing.allocator;
    const dir = ".zig-cache/gt/restart";
    try freshDir(dir);
    {
        var db = try gina.Database.open(dir, .{});
        defer db.close();
        var g = try graph.Graph.init(&db, a, "persist");
        defer g.deinit();
        try g.addEdges(&.{
            .{ .src = "a", .edge_type = "knows", .dst = "b" },
            .{ .src = "b", .edge_type = "knows", .dst = "c" },
            .{ .src = "a", .edge_type = "knows", .dst = "c", .props = "{\"w\":1}" },
        });
    }
    // reopen: WAL/snapshot must restore both tables and every edge
    var db2 = try gina.Database.open(dir, .{});
    defer db2.close();
    var g2 = try graph.Graph.init(&db2, a, "persist");
    defer g2.deinit();
    try testing.expectEqual(@as(i64, 3), try g2.edgeCount());
    const ns = try g2.neighbors(a, "a", .out, &.{}, 100);
    defer freeNeighbors(a, ns);
    try testing.expectEqual(@as(usize, 2), ns.len);
    if (try g2.getEdge("a", "knows", "c")) |row| {
        defer row.free();
        try testing.expect(std.mem.indexOf(u8, row.span(), "\"w\":1") != null);
    } else return error.TestUnexpectedResult;
}

test "graph: dropGraph removes both tables" {
    const a = testing.allocator;
    const dir = ".zig-cache/gt/drop";
    try freshDir(dir);
    var db = try gina.Database.open(dir, .{});
    defer db.close();
    var g = try graph.Graph.init(&db, a, "gone");
    defer g.deinit();
    try g.addEdge("a", "knows", "b", null);
    try g.dropGraph();
    // operations on the dropped graph must now fail cleanly:
    // the engine leaks GINA_ENOENT (=1) as count's value; the layer maps it
    // to error.NotFound via the status code.
    try testing.expectError(error.NotFound, g.edgeCount());
    try testing.expectError(error.NotFound, g.neighbors(a, "a", .out, &.{}, 10));
}
