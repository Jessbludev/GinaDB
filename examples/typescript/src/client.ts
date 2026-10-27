// GinaDB-Lite 0.6 — cliente TypeScript tipado sobre el C ABI.
// Uso:
//   const db = await GinaDB.open("./midata");
//   db.createTable("users", { primary_key: "id", columns: [...], indexes: ["email"] });
//   db.insert("users", { id: "u1", email: "gina@db.io", age: 33 });
//   const hit = db.select("users", { where: { op: "eq", col: "email", val: "gina@db.io" } });
//   db.close();
import type {
  ChangeList, ExecOp, GinaEvent, KnnHit, OpenOptions, Row, Schema,
  SelectQuery, Stats,
} from "./types.ts";
import { loadBackend, type Backend } from "./backend.ts";

// ---- códec de valores ---------------------------------------------------------
// bytes  <-> {"$bytes":"<base64>"}   vector <-> {"$vec":[f32,...]}
const b64tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

function b64encode(b: Uint8Array): string {
  let s = "";
  for (let i = 0; i < b.length; i += 3) {
    const n = (b[i]! << 16) | ((b[i + 1] ?? 0) << 8) | (b[i + 2] ?? 0);
    s += b64tab[(n >> 18) & 63]! + b64tab[(n >> 12) & 63]! +
         (i + 1 < b.length ? b64tab[(n >> 6) & 63]! : "=") +
         (i + 2 < b.length ? b64tab[n & 63]! : "=");
  }
  return s;
}

function b64decode(s: string): Uint8Array {
  const clean = s.replace(/=+$/, "");
  const out = new Uint8Array(Math.floor(clean.length * 3 / 4));
  let o = 0, buf = 0, bits = 0;
  for (const ch of clean) {
    const v = b64tab.indexOf(ch);
    if (v < 0) throw new Error("base64 inválido");
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[o++] = (buf >> bits) & 255;
    }
  }
  return out.subarray(0, o);
}

function toJson(v: unknown): unknown {
  if (v instanceof Uint8Array) return { $bytes: b64encode(v) };
  if (Array.isArray(v) && (v as number[]).__vec) return { $vec: v };
  if (Array.isArray(v)) {
    // distinguir vector: marcado por vec() helper; los array normales van tal cual
    return v.map(toJson);
  }
  if (v !== null && typeof v === "object") {
    const o: Record<string, unknown> = {};
    for (const [k, x] of Object.entries(v as Record<string, unknown>))
      o[k] = toJson(x);
    return o;
  }
  return v;
}

function fromJson(v: unknown): unknown {
  if (v instanceof Uint8Array) return v; // ya convertido (idempotente)
  if (v !== null && typeof v === "object" && !Array.isArray(v)) {
    const o = v as Record<string, unknown>;
    if (typeof o.$bytes === "string") return b64decode(o.$bytes);
    if (Array.isArray(o.$vec)) return o.$vec; // number[] = vector
    const r: Record<string, unknown> = {};
    for (const [k, x] of Object.entries(o)) r[k] = fromJson(x);
    return r;
  }
  if (Array.isArray(v)) return v.map(fromJson);
  return v;
}

// Decodifica una respuesta JSON del motor (fila | fila[] | objeto) en una
// sola pasada top-down: {$bytes}/{$vec} → Uint8Array/number[].
function decode<T>(s: string): T {
  return fromJson(JSON.parse(s)) as T;
}

export function vec(xs: number[]): number[] {
  (xs as number[] & { __vec?: boolean }).__vec = true;
  return xs;
}

export class GinaError extends Error {
  status: number;
  constructor(status: number, message: string) {
    super(`[${status}] ${message}`);
    this.status = status;
  }
}

export class GinaDB {
  backend: Backend;
  private h: unknown;

  private constructor(backend: Backend, h: unknown) {
    this.backend = backend;
    this.h = h;
  }

  static async open(dir: string, opts: OpenOptions = {}, libPath?: string) {
    const backend = await loadBackend(libPath);
    const h = backend.open(
      dir,
      opts.durable ? 1 : 0,
      opts.checkpointEvery ?? 1000,
      opts.changelogCapacity ?? 4096,
      opts.maxBlobSize ?? 16 << 20,
    );
    if (!h) throw new GinaError(backend.lastStatus(), "no se pudo abrir la base");
    return new GinaDB(backend, h);
  }

  get version(): string { return this.backend.version(); }
  get hasSelect(): boolean { return this.backend.hasSelect; }
  get hasIndexes(): boolean { return this.backend.hasIndexes; }
  get hasCompact(): boolean { return this.backend.hasCompact; }

  private ok(rc: number, what: string): void {
    if (rc !== 0) throw new GinaError(rc, `${what}: ${this.backend.lastError(this.h)}`);
  }
  private parse<T>(s: string | null, what: string): T {
    if (s === null)
      throw new GinaError(this.backend.lastStatus(), `${what}: ${this.backend.lastError(this.h)}`);
    return JSON.parse(s) as T;
  }

  private parse2<T>(s: string | null, what: string): T {
    if (s === null)
      throw new GinaError(this.backend.lastStatus(),
                          `${what}: ${this.backend.lastError(this.h)}`);
    return decode<T>(s);
  }

  // ---- DDL ------------------------------------------------------------------
  createTable(name: string, schema: Schema = {}): void {
    this.ok(this.backend.createTable(this.h, name, JSON.stringify(schema)), "createTable");
  }
  dropTable(name: string): void {
    this.ok(this.backend.dropTable(this.h, name), "dropTable");
  }
  schema(name: string): Schema {
    return this.parse<Schema>(this.backend.schema(this.h, name), "schema");
  }
  tables(): string[] {
    return this.parse<string[]>(this.backend.tables(this.h), "tables");
  }

  // ---- índices (0.6) ------------------------------------------------------------
  createIndex(table: string, column: string): void {
    this.ok(this.backend.createIndex(this.h, table, column), "createIndex");
  }
  dropIndex(table: string, column: string): void {
    this.ok(this.backend.dropIndex(this.h, table, column), "dropIndex");
  }

  // ---- CRUD -----------------------------------------------------------------
  insert(table: string, row: Row): void {
    this.ok(this.backend.insert(this.h, table, JSON.stringify(toJson(row))), "insert");
  }
  get(table: string, key: string): Row | null {
    const s = this.backend.get(this.h, table, key);
    if (s === null) return null; // ENOENT = no encontrado
    return decode<Row>(s);
  }
  update(table: string, key: string, patch: Row): boolean {
    const rc = this.backend.update(this.h, table, key, JSON.stringify(toJson(patch)));
    if (rc === 0) return true;
    if (this.backend.lastStatus() === 1) return false; // ENOENT
    throw new GinaError(rc, `update: ${this.backend.lastError(this.h)}`);
  }
  erase(table: string, key: string): boolean {
    const rc = this.backend.erase(this.h, table, key);
    if (rc === 1) return true;
    if (rc === 0) return false;
    throw new GinaError(rc, `erase: ${this.backend.lastError(this.h)}`);
  }
  all(table: string): Row[] {
    return this.parse2<Row[]>(this.backend.all(this.h, table), "all");
  }
  count(table: string): number {
    const n = this.backend.count(this.h, table);
    if (n < 0) throw new GinaError(this.backend.lastStatus(), "count");
    return n;
  }

  // ---- consultas (0.6: select con planner; 0.5: throw) ---------------------------
  select(table: string, q: SelectQuery = {}): Row[] {
    const s = this.backend.select(this.h, table, JSON.stringify(q));
    return this.parse2<Row[]>(s, "select");
  }

  // ---- batch/transacción ---------------------------------------------------------
  exec(ops: ExecOp[]): void {
    const payload = ops.map((o) =>
      o.op === "put"
        ? { op: "put", table: o.table, row: toJson(o.row ?? {}) }
        : { op: "del", table: o.table, key: o.key },
    );
    this.ok(this.backend.exec(this.h, JSON.stringify(payload)), "exec");
  }

  // ---- multimodal ---------------------------------------------------------------
  knn(table: string, column: string, query: number[] | Float32Array, k: number,
      metric: "cosine" | "l2" = "cosine"): KnnHit[] {
    const q = query instanceof Float32Array ? query : Float32Array.from(query);
    return this.parse<KnnHit[]>(
      this.backend.knn(this.h, table, column, q, q.length, k,
                       metric === "l2" ? 1 : 0),
      "knn",
    );
  }
  search(table: string, column: string, query: string, limit = 50): Row[] {
    return this.parse2<Row[]>(
      this.backend.search(this.h, table, column, query, limit), "search");
  }

  // ---- real-time ------------------------------------------------------------------
  watch(table: string, cb: (ev: GinaEvent) => void): number {
    return this.backend.watch(this.h, table, (json) => cb(JSON.parse(json)));
  }
  unwatch(id: number): void {
    this.ok(this.backend.unwatch(this.h, id), "unwatch");
  }
  changes(table: string, since: number): ChangeList {
    return this.parse<ChangeList>(this.backend.changes(this.h, table, since), "changes");
  }

  // ---- mantenimiento --------------------------------------------------------------
  checkpoint(): void { this.ok(this.backend.checkpoint(this.h), "checkpoint"); }
  compact(): void { this.ok(this.backend.compact(this.h), "compact"); }
  stats(): Stats { return this.parse<Stats>(this.backend.stats(this.h), "stats"); }

  close(): void {
    if (this.h) {
      this.backend.close(this.h);
      this.h = null;
    }
  }
}
