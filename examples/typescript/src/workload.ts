// GinaDB-Lite 0.6 — workload real de datos: somete la base al uso para el que
// fue diseñada (CRUD masivo, índices secundarios, rangos PK, FTS, KNN,
// transacciones, real-time, checkpoints incrementales, compactación, crash)
// y recopila métricas (ops/s, p50/p95/p99, tamaños de archivo, recuperación).
//
// Uso:
//   bun  src/workload.ts [usuarios] [docs] [gets]      (backend bun:ffi)
//   node src/workload.ts ...                           (backend koffi)
//   GINA_LIB=/ruta/libginadb.so  → apunta a otra versión (p.ej. 0.5): el
//   harness degrada select/índices a su equivalente 0.5 (all + filtro).
import { rmSync, mkdirSync, existsSync, statSync, writeFileSync } from "node:fs";
import { spawn } from "node:child_process";
import { GinaDB, vec } from "./client.ts";
import type { Row, SelectQuery } from "./types.ts";

// ---- configuración -------------------------------------------------------------
const USERS = Number(process.argv[2] ?? 20000);
const DOCS = Number(process.argv[3] ?? 5000);
const GETS = Number(process.argv[4] ?? 5000);
const DIR = process.env.GINA_DIR ?? "ts-data/workload";
const VEC_DIM = 64;
const IS_BUN = typeof (globalThis as any).Bun !== "undefined";
const RUNTIME = IS_BUN ? "bun" : "node";
const LIB = process.env.GINA_LIB ?? "libginadb.so (default)";

interface Phase {
  name: string;
  ops: number;
  ms: number;
  p50?: number;
  p95?: number;
  p99?: number;
  extra?: Record<string, string | number>;
}
const results: Phase[] = [];

function now(): number {
  return performance.now();
}
function pct(sorted: number[], p: number): number {
  if (!sorted.length) return 0;
  const i = Math.min(sorted.length - 1, Math.floor((p / 100) * sorted.length));
  return sorted[i]!;
}
function record(name: string, ops: number, t0: number,
                lat?: number[], extra?: Record<string, string | number>) {
  const ms = now() - t0;
  const sorted = lat ? [...lat].sort((a, b) => a - b) : undefined;
  results.push({
    name, ops, ms,
    p50: sorted ? +pct(sorted, 50).toFixed(1) : undefined,
    p95: sorted ? +pct(sorted, 95).toFixed(1) : undefined,
    p99: sorted ? +pct(sorted, 99).toFixed(1) : undefined,
    extra,
  });
  console.log(
    `  ${name.padEnd(34)} ${String(ops).padStart(7)} ops  ${ms.toFixed(0).padStart(7)} ms  ` +
    (ops > 0 && ms > 0 ? `${(ops / (ms / 1000)).toFixed(0).padStart(8)} ops/s` : "".padStart(14)) +
    (sorted ? `  p50=${pct(sorted, 50).toFixed(0)}µs p95=${pct(sorted, 95).toFixed(0)}µs p99=${pct(sorted, 99).toFixed(0)}µs` : "") +
    (extra ? "  " + Object.entries(extra).map(([k, v]) => `${k}=${v}`).join(" ") : ""),
  );
}
const fsize = (p: string): number => (existsSync(p) ? statSync(p).size : 0);

// ---- generadores de datos ---------------------------------------------------------
const FIRST = ["gina", "ada", "linus", "grace", "alan", "barbara", "dennis", "radia",
  "ken", "margaret", "edsger", "frances", "donald", "jean", "john", "hedy"];
const LAST = ["lovelace", "torvalds", "hopper", "turing", "liskov", "ritchie",
  "perlman", "thompson", "hamilton", "dijkstra", "spencer", "bartik", "knuth",
  "sammet", "backus", "lamarr"];
const CITIES = ["Madrid", "Lima", "Bogotá", "Buenos Aires", "CDMX", "Santiago",
  "Barcelona", "Quito", "Montevideo", "Sevilla"];
const WORDS = ["base", "datos", "embeddings", "índice", "btree", "wal", "delta",
  "vector", "busqueda", "texto", "memoria", "páginas", "transacción", "clave",
  "consulta", "rendimiento", "persistencia", "recovery", "compactación", "latch"];
let seed = 42;
function rnd(): number {
  seed = (seed * 1103515245 + 12345) & 0x7fffffff;
  return seed / 0x7fffffff;
}
const pick = <T,>(a: T[]): T => a[Math.floor(rnd() * a.length)]!;
const uid = (i: number) => "u" + String(i).padStart(7, "0");

function userRow(i: number): Row {
  return {
    id: uid(i),
    name: `${pick(FIRST)} ${pick(LAST)}`,
    email: `user${i}@ejemplo.com`,
    age: 18 + Math.floor(rnd() * 60),
    city: pick(CITIES),
    tags: [pick(WORDS), pick(WORDS), pick(WORDS)],
    bio: `${pick(WORDS)} ${pick(WORDS)} ${pick(WORDS)} ${pick(WORDS)} ${pick(WORDS)}`,
  };
}
function docRow(i: number): Row {
  const v = new Array(VEC_DIM);
  for (let d = 0; d < VEC_DIM; d++) v[d] = +(rnd() * 2 - 1).toFixed(3);
  return {
    id: "d" + String(i).padStart(7, "0"),
    title: `${pick(WORDS)} ${pick(WORDS)} ${i}`,
    body: `${pick(WORDS)} ${pick(WORDS)} ${pick(WORDS)} ${pick(WORDS)}`,
    embedding: vec(v),
  };
}

// ---- select con fallback 0.5 (all + filtro en el cliente) -------------------------
function selectCompat(db: GinaDB, table: string, q: SelectQuery): Row[] {
  if (db.hasSelect) return db.select(table, q);
  // 0.5: traer todo y filtrar/proyectar en TypeScript
  const rows = db.all(table);
  const w = q.where;
  const out: Row[] = [];
  const match = (r: Row): boolean => {
    if (!w) return true;
    if (w.op === "and") return (w.args ?? []).every((a) =>
      match({ ...r, __c: 0 } as Row) && matchCond(r, a));
    if (w.op === "or") return (w.args ?? []).some((a) => matchCond(r, a));
    return matchCond(r, w);
  };
  const matchCond = (r: Row, c: any): boolean => {
    const v = r[c.col as string];
    switch (c.op) {
      case "eq": return v === c.val;
      case "ne": return v !== c.val;
      case "gt": return (v as any) > c.val;
      case "ge": return (v as any) >= c.val;
      case "lt": return (v as any) < c.val;
      case "le": return (v as any) <= c.val;
      case "between": return v >= c.lo && v <= c.hi;
      case "in": return (c.vals as any[]).includes(v);
      default: return true;
    }
  };
  for (const r of rows) if (match(r)) out.push(r);
  if (q.order && q.order.length) {
    const o = q.order[0]!;
    const col = typeof o === "string" ? o : o.col;
    const desc = typeof o === "string" ? false : !!o.desc;
    out.sort((a, b) => {
      const x = a[col] as any, y = b[col] as any;
      return desc ? (x < y ? 1 : x > y ? -1 : 0) : (x < y ? -1 : x > y ? 1 : 0);
    });
  }
  const lim = q.limit ?? Infinity;
  return out.slice(0, lim);
}

// =============================================================================
async function main() {
  console.log(`\nGinaDB workload — runtime=${RUNTIME}  lib=${LIB}`);
  console.log(`usuarios=${USERS}  docs=${DOCS} (dim ${VEC_DIM})  gets=${GETS}\n`);
  rmSync(DIR, { recursive: true, force: true });
  mkdirSync(DIR, { recursive: true });

  const t_open = now();
  let db = await GinaDB.open(DIR, { checkpointEvery: 0, changelogCapacity: 8192 });
  const openMs = now() - t_open;
  console.log(`  ${"apertura inicial".padEnd(34)} ${openMs.toFixed(1).padStart(8)} ms  versión=${db.version}  select=${db.hasSelect} índices=${db.hasIndexes}\n`);

  // ---- fase 1: DDL + carga masiva por lotes (transacciones) --------------------
  db.createTable("users", {
    primary_key: "id",
    columns: [
      { name: "id", type: "text" },
      { name: "name", type: "text" },
      { name: "email", type: "text" },
      { name: "age", type: "int" },
      { name: "city", type: "text" },
      { name: "tags", type: "array" },
      { name: "bio", type: "text", fts: true },
    ],
    indexes: db.hasIndexes && !process.env.GINA_NO_INDEXES
      ? ["email", "age", "city"] : [],
  });
  db.createTable("docs", {
    primary_key: "id",
    columns: [
      { name: "id", type: "text" },
      { name: "title", type: "text" },
      { name: "body", type: "text", fts: true },
      { name: "embedding", type: "vector" },
    ],
  });

  let t = now();
  const BATCH = 500;
  for (let i = 0; i < USERS; i += BATCH) {
    const ops = [];
    for (let j = i; j < Math.min(i + BATCH, USERS); j++)
      ops.push({ op: "put", table: "users", row: userRow(j) } as any);
    db.exec(ops);
  }
  record("carga users (lotes de 500)", USERS, t);

  t = now();
  for (let i = 0; i < DOCS; i += BATCH) {
    const ops = [];
    for (let j = i; j < Math.min(i + BATCH, DOCS); j++)
      ops.push({ op: "put", table: "docs", row: docRow(j) } as any);
    db.exec(ops);
  }
  record("carga docs+embeddings", DOCS, t);

  if (db.hasIndexes) {
    t = now();
    db.createIndex("docs", "title");
    record("create_index sobre tabla llena", DOCS, t, undefined,
           { detalle: "docs.title" });
  }

  // ---- fase 2: lecturas puntuales por PK ----------------------------------------
  const keyList = Array.from({ length: GETS }, () => uid(Math.floor(rnd() * USERS)));
  t = now();
  const lat: number[] = [];
  let hits = 0;
  for (const k of keyList) {
    const a = now();
    const r = db.get("users", k);
    if (r) hits++;
    lat.push(now() - a);
  }
  record("get puntual por PK", GETS, t, lat.map((x) => x * 1000), { hits });

  // ---- fase 3: eq por índice secundario -----------------------------------------
  const skipQueries = process.env.GINA_QUERIES === "0";
  const latEq: number[] = [];
  t = now();
  let eqRows = 0;
  for (let i = 0; i < (skipQueries ? 0 : 500); i++) {
    const a = now();
    const rows = selectCompat(db, "users",
      { where: { op: "eq", col: "email", val: `user${Math.floor(rnd() * USERS)}@ejemplo.com` } });
    eqRows += rows.length;
    latEq.push(now() - a);
  }
  record("select eq email (índice)", 500, t, latEq.map((x) => x * 1000), { filas: eqRows });

  // ---- fase 4: rango por índice secundario --------------------------------------
  const latRg: number[] = [];
  t = now();
  let rgRows = 0;
  for (let i = 0; i < (skipQueries ? 0 : 300); i++) {
    const a = now();
    const age = 20 + Math.floor(rnd() * 50);
    const rows = selectCompat(db, "users", {
      where: { op: "between", col: "age", lo: age, hi: age + 9 },
      order: [{ col: "age", desc: true }], limit: 100,
    });
    rgRows += rows.length;
    latRg.push(now() - a);
  }
  record("select rango age + order", 300, t, latRg.map((x) => x * 1000), { filas: rgRows });

  // ---- fase 5: rango PK inverso + limit ------------------------------------------
  const latPk: number[] = [];
  t = now();
  for (let i = 0; i < (skipQueries ? 0 : 300); i++) {
    const a = now();
    const k0 = Math.floor(rnd() * (USERS - 2000));
    const rows = selectCompat(db, "users", {
      where: { op: "between", col: "id", lo: uid(k0), hi: uid(k0 + 1999) },
      order: [{ col: "id", desc: true }], limit: 10, fields: ["id", "age"],
    });
    latPk.push(now() - a);
    if (rows.length !== 10) throw new Error(`rango PK devolvió ${rows.length}`);
  }
  record("rango PK desc + limit 10", 300, t, latPk.map((x) => x * 1000));

  // ---- fase 6: FTS ------------------------------------------------------------------
  const latF: number[] = [];
  t = now();
  let ftsRows = 0;
  for (let i = 0; i < 200; i++) {
    const a = now();
    const rows = db.search("users", "bio", `${pick(WORDS)} ${pick(WORDS)}`, 50);
    ftsRows += rows.length;
    latF.push(now() - a);
  }
  record("FTS bio (AND 2 términos)", 200, t, latF.map((x) => x * 1000), { filas: ftsRows });

  // ---- fase 7: KNN --------------------------------------------------------------------
  const latK: number[] = [];
  t = now();
  for (let i = 0; i < 100; i++) {
    const a = now();
    const q = new Array(VEC_DIM).fill(0).map(() => +(rnd() * 2 - 1).toFixed(3));
    const hits2 = db.knn("docs", "embedding", q, 10);
    if (hits2.length !== 10) throw new Error("knn incompleto");
    latK.push(now() - a);
  }
  record(`KNN k=10 sobre ${DOCS}×${VEC_DIM}`, 100, t, latK.map((x) => x * 1000));

  // ---- fase 8: CRUD mixto (ops individuales) --------------------------------------------
  t = now();
  const latC: number[] = [];
  const extraBase = USERS;
  for (let i = 0; i < 3000; i++) {
    const a = now();
    const roll = rnd();
    if (roll < 0.34) {
      db.insert("users", userRow(extraBase + i));
    } else if (roll < 0.67) {
      db.update("users", uid(Math.floor(rnd() * USERS)), { age: 18 + Math.floor(rnd() * 60) });
    } else {
      db.erase("users", uid(Math.floor(rnd() * USERS)));
    }
    latC.push(now() - a);
  }
  record("CRUD mixto (1 op/tx)", 3000, t, latC.map((x) => x * 1000));

  // ---- fase 9: real-time -----------------------------------------------------------------
  let events = 0;
  const wid = db.watch("users", () => events++);
  t = now();
  for (let i = 0; i < 1000; i++) {
    const row = { ...userRow(i), id: "rt" + i };
    db.exec([{ op: "put", table: "users", row }]);
  }
  record("watch: eventos recibidos", 1000, t, undefined, { eventos: events });
  db.unwatch(wid);

  // ---- fase 10: checkpoints (IC y FC medidos por separado) ------------------------
  t = now();
  db.checkpoint();
  const st1 = db.stats();
  record("checkpoint incremental (IC)", 1, t, undefined, {
    delta_bytes: st1.delta_bytes,
    delta_live: st1.delta_live_pages,
    wal_bytes: st1.wal_bytes,
  });

  if (db.hasCompact) {
    t = now();
    db.compact();                       // FC explícito: materializa el base
    record("compactación (FC) explícita", 1, t, undefined, {
      base_bytes: db.stats().data_bytes,
    });
  }

  // escrituras nuevas + IC con dirty pequeño (ahora el base existe y el
  // delta está vacío: este checkpoint es incremental de verdad)
  t = now();
  for (let i = 0; i < 500; i++)
    db.update("users", uid(i), { age: 70 + (i % 20) });
  const tIC = now();
  db.checkpoint();
  record("segundo IC (dirty pequeño)", 500, t, undefined, {
    ic_ms: +(now() - tIC).toFixed(1),
    delta_bytes: db.stats().delta_bytes,
  });

  // ---- fase 11: reabrir (recuperación) ----------------------------------------------------
  db.close();
  t = now();
  db = await GinaDB.open(DIR, { checkpointEvery: 0 });
  const reopenMs = now() - t;
  const cnt = db.count("users");
  record("reabrir (base+delta+WAL)", 1, t, undefined, {
    ms_reapertura: +reopenMs.toFixed(1),
    users: cnt,
  });
  // verificación de integridad de una muestra (rt* nunca se borra)
  const probe = db.get("users", "rt0")!;
  if (!probe || typeof probe.age !== "number") throw new Error("fila corrupta tras reopen");

  // ---- fase 12: segunda compactación (FC tras churn) ------------------------------
  t = now();
  if (db.hasCompact) db.compact();
  const fcMs = now() - t;
  const st2 = db.stats();
  results.push({ name: "compactación final (FC)", ops: 1, ms: fcMs, extra: {
    base_bytes: st2.data_bytes, delta_bytes: st2.delta_bytes,
  } });
  console.log(`  ${"compactación final (FC)".padEnd(34)} ${fcMs.toFixed(0).padStart(16)} ms   base=${st2.data_bytes}B delta=${st2.delta_bytes}B`);

  // ---- fase 13: crash-safety (SIGKILL + prefijo contiguo) --------------------------------------
  console.log("");
  const crashDir = DIR + "-crash";
  rmSync(crashDir, { recursive: true, force: true });
  mkdirSync(crashDir, { recursive: true });
  {
    const pre = await GinaDB.open(crashDir, { checkpointEvery: 0 });
    pre.createTable("victim", { primary_key: "id", columns: [{ name: "id", type: "text" }, { name: "n", type: "int" }] });
    pre.checkpoint();
    pre.close();
  }
  const rounds = 3;
  for (let r = 0; r < rounds; r++) {
    const exe = process.execPath;
    const script = new URL("./crash_child.ts", import.meta.url).pathname;
    const args = IS_BUN
      ? [script, crashDir, "1000000"]
      : ["--experimental-strip-types", script, crashDir, "1000000"];
    const child = spawn(exe, args, {
      env: { ...process.env, GINA_LIB: process.env.GINA_LIB },
      stdio: "ignore",
      cwd: process.cwd(),
    });
    let exited = false;
    child.once("exit", () => { exited = true; });
    await new Promise((res) => setTimeout(res, 250 + Math.floor(rnd() * 400)));
    let killed = false;
    if (!exited) {
      child.kill("SIGKILL");
      killed = true;
      await new Promise((res) => {
        if (exited) return res(null);
        child.once("exit", () => res(null));
      });
    }

    const tR = now();
    const cdb = await GinaDB.open(crashDir);
    const rMs = now() - tR;
    const n = cdb.count("victim");
    // prefijo contiguo k000000..k(n-1): frontera + muestreo denso + ausencia del siguiente
    let ok = true;
    const kOf = (i: number) => "k" + String(i).padStart(6, "0");
    const sample = Math.min(n, 5000);
    for (let i = 0; i < sample; i++)
      if (!cdb.get("victim", kOf(i))) { ok = false; break; }
    if (ok && n > 0) {
      for (let i = Math.max(0, n - 5); i < n; i++)
        if (!cdb.get("victim", kOf(i))) { ok = false; break; }
      for (let i = 1; i <= 50 && ok; i++) {  // ausencia estricta tras la frontera
        if (cdb.get("victim", kOf(n + i - 1))) ok = false;
      }
    }
    cdb.close();
    console.log(`  crash ronda ${r + 1}: ${killed ? "SIGKILL" : "completó"}  sobrevivieron ${n} commits  prefijo-contiguo=${ok ? "SÍ" : "NO"}  recuperación=${rMs.toFixed(1)}ms`);
    if (!ok) throw new Error("CRASH TEST FALLÓ: prefijo no contiguo");
    if (n === 0) throw new Error("CRASH TEST FALLÓ: se perdió todo");
  }

  // ---- resumen -----------------------------------------------------------------------------
  const rss = process.memoryUsage.rss() / 1024 / 1024;
  const totalRows = db.count("users") + db.count("docs");
  const stF = db.stats();
  console.log(`\n  filas totales=${totalRows}  RSS=${rss.toFixed(0)}MB`);
  console.log(`  archivos: base=${fsize(DIR + "/data.pages")}B delta=${fsize(DIR + "/delta.pages")}B wal=${fsize(DIR + "/wal.log")}B`);
  console.log(`  stats: checkpoints=${stF.checkpoints} compactions=${stF.compactions} suspect=${stF.delta_suspect_pages}`);
  db.close();

  const out = {
    runtime: RUNTIME, lib: LIB, users: USERS, docs: DOCS, gets: GETS,
    vecDim: VEC_DIM, engine: db.version, phases: results,
    rss_mb: +rss.toFixed(1), rows: totalRows,
    files: {
      base: fsize(DIR + "/data.pages"), delta: fsize(DIR + "/delta.pages"),
      wal: fsize(DIR + "/wal.log"),
    },
  };
  const outFile = `ts-data/workload-${RUNTIME}-${db.version.includes("0.5") ? "05" : "06"}.json`;
  writeFileSync(outFile, JSON.stringify(out, null, 2));
  console.log(`\n  resultados → ${outFile}`);
}

main().catch((e) => { console.error("WORKLOAD FALLÓ:", e); process.exit(1); });
