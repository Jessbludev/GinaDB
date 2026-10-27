//! GinaDB Lite 0.5 — Zig build:
//!   zig build                 → gina-inspect CLI + libginaread.a (C ABI reader)
//!   zig build test            → parser unit tests
//!   zig build ffi-demo        → FFI demo against ../libginadb.so (build C++ first)
//!   zig build ffi-demo -Dlink-c++-engine=true
const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseSafe });

    // ---- pure-Zig reader module ------------------------------------------
    const reader_mod = b.createModule(.{
        .root_source_file = b.path("ginaread.zig"),
        .target = target,
        .optimize = optimize,
    });

    const reader_lib = b.addLibrary(.{
        .name = "ginaread",
        .root_module = reader_mod,
        .linkage = .static,
    });
    b.installArtifact(reader_lib);

    const reader_shared = b.addLibrary(.{
        .name = "ginaread",
        .root_module = b.createModule(.{
            .root_source_file = b.path("ginaread.zig"),
            .target = target,
            .optimize = optimize,
        }),
        .linkage = .dynamic,
    });
    b.installArtifact(reader_shared);

    // ---- inspector CLI ------------------------------------------------------
    const inspect = b.addExecutable(.{
        .name = "gina-inspect",
        .root_module = b.createModule(.{
            .root_source_file = b.path("inspect.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "ginaread", .module = reader_mod },
            },
        }),
    });
    b.installArtifact(inspect);

    // ---- tests ---------------------------------------------------------------
    const tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("ginaread.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    const run_tests = b.addRunArtifact(tests);
    b.step("test", "Run ginaread parser tests").dependOn(&run_tests.step);

    // ---- FFI demo (optional; links the C++ engine's shared library) ---------
    const ffi_step = b.step("ffi-demo", "Build the Zig FFI demo (needs ../libginadb.so)");
    const ffi_mod = b.createModule(.{
        .root_source_file = b.path("ffi_demo.zig"),
        .target = target,
        .optimize = optimize,
    });
    ffi_mod.addLibraryPath(b.path("../"));
    ffi_mod.linkSystemLibrary("ginadb", .{});
    ffi_mod.link_libc = true;
    const ffi = b.addExecutable(.{
        .name = "gina-ffi-demo",
        .root_module = ffi_mod,
    });
    ffi_step.dependOn(&b.addInstallArtifact(ffi, .{}).step);
}
