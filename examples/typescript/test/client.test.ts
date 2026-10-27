// GinaDB-Lite 0.6 — suite funcional del cliente TypeScript (bun test).
// Cubre: CRUD, schemas, índices secundarios + planner, select JSON, rangos PK,
// transacciones, multimodal (KNN + FTS + bytes), watch/changelog, persistencia
// base+delta, compactación, y el códec de valores (b64/vec).
import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import { rmSync, mkdirSync } from "node:fs";
import { GinaDB, vec } from "../src/client.ts";

const DIR = "ts-data/test";
let db: GinaDB;

beforeAll(async () => {
  rmSync(DIR, { recursive: true, force: true });
  mkdirSync(DIR, { recursive: true });
  db = await GinaDB.open(DIR, { checkpointEvery: 0 });
});

afterAll(() => db.close());

describe("GinaDB 0.7 — cliente TypeScript", () => {
  test("versión y capacidades", () => {
    expect(db.version).toContain("0.7");
    expect(db.hasSelect).toBe(true);
    expect(db.hasIndexes).toBe(true);
    expect(db.hasCompact).toBe(true);
  });

  test("DDL + CRUD tipado", () => {
    db.createTable("users", {
      primary_key: "id",
      columns: [
        { name: "id", type: "text" },
        { name: "email", type: "text" },
        { name: "age", type: "int" },
        { name: "bio", type: "text", fts: true },
      ],
      indexes: ["email", "age"],
    });
    db.insert("users", { id: "u1", email: "gina@db.io", age: 33, bio: "crea bases de datos" });
    db.insert("users", { id: "u2", email: "ada@db.io", age: 36, bio: "escribe algoritmos" });
    db.insert("users", { id: "u3", email: "lin@db.io", age: 21, bio: "crea kernels" });

    const u = db.get("users", "u2")!;
    expect(u.email).toBe("ada@db.io");
    expect(u.age).toBe(36);
    expect(db.get("users", "nope")).toBeNull();

    expect(db.update("users", "u1", { age: 34 })).toBe(true);
    expect(db.get("users", "u1")!.age).toBe(34);
    expect(db.update("users", "zz", { age: 1 })).toBe(false);
    expect(db.count("users")).toBe(3);
    expect(db.erase("users", "u3")).toBe(true);
    expect(db.erase("users", "u3")).toBe(false);
    expect(db.count("users")).toBe(2);
  });

  test("select con pushdown a índices", () => {
    const hit = db.select("users", { where: { op: "eq", col: "email", val: "ada@db.io" } });
    expect(hit.length).toBe(1);
    expect(hit[0]!.id).toBe("u2");

    const rng = db.select("users", {
      where: { op: "between", col: "age", lo: 30, hi: 40 },
      order: [{ col: "age", desc: true }],
    });
    expect(rng.map((r) => r.age)).toEqual([36, 34]);

    const pkRange = db.select("users", {
      where: { op: "between", col: "id", lo: "u1", hi: "u2" },
      fields: ["id", "email"],
    });
    expect(pkRange.length).toBe(2);
    expect(pkRange[0]).toEqual({ id: "u1", email: "gina@db.io" });
  });

  test("select en 0.5 falla limpiamente / en 0.6 opera", () => {
    // (0.6 siempre aquí; el fallback 0.5 se prueba en el workload)
    const all = db.select("users", {});
    expect(all.length).toBe(2);
  });

  test("índices dinámicos (create/drop)", () => {
    db.createIndex("users", "bio");
    expect(db.schema("users").indexes).toContain("bio");
    db.dropIndex("users", "bio");
    expect(db.schema("users").indexes).toEqual(["email", "age"]);
  });

  test("transacciones batch (exec) atómicas", () => {
    expect(() =>
      db.exec([
        { op: "put", table: "users", row: { id: "tx1", email: "tx1@x.io", age: 1 } },
        { op: "put", table: "missing", row: { id: "x" } }, // aborta todo
      ]),
    ).toThrow();
    expect(db.get("users", "tx1")).toBeNull(); // nada aplicado
    db.exec([
      { op: "put", table: "users", row: { id: "tx1", email: "tx1@x.io", age: 40 } },
      { op: "del", table: "users", key: "u1" },
    ]);
    expect(db.get("users", "tx1")!.age).toBe(40);
    expect(db.get("users", "u1")).toBeNull();
  });

  test("bytes (b64) round-trip", () => {
    const blob = new Uint8Array(9000);
    for (let i = 0; i < blob.length; i++) blob[i] = (i * 31) & 255;
    db.createTable("blobs", { primary_key: "id", columns: [{ name: "id", type: "text" }, { name: "data", type: "bytes" }] });
    db.insert("blobs", { id: "b1", data: blob });
    const got = db.get("blobs", "b1")!.data as Uint8Array;
    expect(got.length).toBe(9000);
    expect(got[8999]).toBe((8999 * 31) & 255);
  });

  test("KNN por embeddings", () => {
    db.createTable("media", { primary_key: "id", columns: [{ name: "id", type: "text" }, { name: "vec", type: "vector" }] });
    db.insert("media", { id: "m1", vec: vec([1, 0, 0]) });
    db.insert("media", { id: "m2", vec: vec([0, 1, 0]) });
    db.insert("media", { id: "m3", vec: vec([0.9, 0.1, 0]) });
    const hits = db.knn("media", "vec", [1, 0, 0], 2);
    expect(hits.length).toBe(2);
    expect(hits[0]!.key).toBe("m1"); // coseno exacto
    expect(hits[1]!.key).toBe("m3");
    expect(hits[0]!.score).toBeCloseTo(1.0, 5);
  });

  test("FTS con índice invertido", () => {
    // u1 ("bases de datos") fue borrado en el test de tx → 0 resultados
    expect(db.search("users", "bio", "datos").length).toBe(0);
    const docs = db.search("users", "bio", "algoritmos");
    expect(docs.length).toBe(1);
    expect(docs[0]!.id).toBe("u2");
  });

  test("watch + changelog en tiempo real", () => {
    let events = 0;
    const id = db.watch("users", () => { events++; });
    db.exec([{ op: "put", table: "users", row: { id: "w1", email: "w1@x.io", age: 50 } }]);
    db.exec([{ op: "del", table: "users", key: "w1" }]);
    expect(events).toBe(2);
    const cl = db.changes("users", 0);
    expect(cl.events.length).toBeGreaterThanOrEqual(2);
    expect(cl.next_cursor).toBeGreaterThan(0);
    db.unwatch(id);
  });

  test("persistencia: checkpoint incremental → reopen → compactación", () => {
    db.checkpoint(); // IC: todo al delta
    let st = db.stats();
    expect(st.delta_live_pages).toBeGreaterThan(0);
    db.close();
    db = null as unknown as GinaDB;
    return GinaDB.open(DIR, { checkpointEvery: 0 }).then(async (d2) => {
      db = d2;
      expect(db.count("users")).toBe(2); // u2 + tx1
      expect(db.get("users", "tx1")!.age).toBe(40);
      const b = db.get("blobs", "b1")!.data as Uint8Array;
      expect(b.length).toBe(9000);
      db.compact();
      st = db.stats();
      expect(st.data_bytes).toBeGreaterThan(0);
      expect(st.delta_live_pages).toBe(0);
    });
  });
});
