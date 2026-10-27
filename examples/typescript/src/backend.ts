// GinaDB-Lite 0.6 — capa FFI dual:
//   * Bun  → bun:ffi (sin dependencias)
//   * Node → koffi
// Detección automática por runtime; GINA_LIB=path/to/libginadb.so override.
// El backend se crea con load(); los símbolos 0.6 (select/índices/compact) se
// enlazan opcionalmente → hasSelect permite comparar contra una lib 0.5.
export interface Backend {
  name: string;
  hasSelect: boolean; // gina_select disponible (0.6)
  hasIndexes: boolean; // create/drop index (0.6)
  hasCompact: boolean; // gina_compact (0.6)
  open(dir: string, durable: number, ckpt: number, changelog: number,
       maxBlob: number): unknown;
  close(h: unknown): void;
  createTable(h: unknown, name: string, json: string): number;
  dropTable(h: unknown, name: string): number;
  schema(h: unknown, table: string): string | null;
  insert(h: unknown, table: string, json: string): number;
  update(h: unknown, table: string, key: string, json: string): number;
  erase(h: unknown, table: string, key: string): number;
  get(h: unknown, table: string, key: string): string | null;
  all(h: unknown, table: string): string | null;
  count(h: unknown, table: string): number;
  exec(h: unknown, json: string): number;
  knn(h: unknown, table: string, col: string, q: Float32Array, dims: number,
      k: number, metric: number): string | null;
  search(h: unknown, table: string, col: string, q: string, limit: number):
    string | null;
  changes(h: unknown, table: string, since: number): string | null;
  watch(h: unknown, table: string,
        cb: (json: string) => void): number;
  unwatch(h: unknown, id: number): number;
  select(h: unknown, table: string, json: string): string | null;
  createIndex(h: unknown, table: string, col: string): number;
  dropIndex(h: unknown, table: string, col: string): number;
  checkpoint(h: unknown): number;
  compact(h: unknown): number;
  stats(h: unknown): string | null;
  tables(h: unknown): string | null;
  freeStr(s: unknown): void;
  lastError(h: unknown): string;
  lastStatus(): number;
  version(): string;
}

export function defaultLibPath(): string {
  const env = process.env.GINA_LIB;
  if (env) return env;
  // examples/typescript/src → raíz del proyecto
  return new URL("../../../libginadb.so", import.meta.url).pathname;
}

// ---------------------------------------------------------------------------
// Bun (bun:ffi)
// ---------------------------------------------------------------------------
async function makeBunBackend(libPath: string): Promise<Backend> {
  const ffi = (await import("bun:ffi")) as any;
  const { dlopen } = ffi;
  const enc = new TextEncoder();

  const readCString = (p: unknown): string => {
    const ptr = p as number;
    if (!ptr) return "";
    return String(new (ffi as any).CString(ptr));
  };
  const cstr = (s: string): Uint8Array => enc.encode(s + "\0");
  // Los Callbacks deben vivir mientras el motor los use
  const keepAlive: unknown[] = [];

  const core = {
    gina_open: { args: ["pointer", "int", "int", "u64", "u64"], returns: "pointer" },
    gina_close: { args: ["pointer"], returns: "void" },
    gina_create_table: { args: ["pointer", "pointer", "pointer"], returns: "int" },
    gina_drop_table: { args: ["pointer", "pointer"], returns: "int" },
    gina_schema: { args: ["pointer", "pointer"], returns: "pointer" },
    gina_insert: { args: ["pointer", "pointer", "pointer"], returns: "int" },
    gina_update: { args: ["pointer", "pointer", "pointer", "pointer"], returns: "int" },
    gina_erase: { args: ["pointer", "pointer", "pointer"], returns: "int" },
    gina_get: { args: ["pointer", "pointer", "pointer"], returns: "pointer" },
    gina_all: { args: ["pointer", "pointer"], returns: "pointer" },
    gina_count: { args: ["pointer", "pointer"], returns: "i64" },
    gina_exec: { args: ["pointer", "pointer"], returns: "int" },
    gina_knn: { args: ["pointer", "pointer", "pointer", "pointer", "u32", "u32", "int"], returns: "pointer" },
    gina_search: { args: ["pointer", "pointer", "pointer", "pointer", "u32"], returns: "pointer" },
    gina_changes: { args: ["pointer", "pointer", "u64"], returns: "pointer" },
    gina_watch: { args: ["pointer", "pointer", "pointer", "pointer"], returns: "i64" },
    gina_unwatch: { args: ["pointer", "i64"], returns: "int" },
    gina_checkpoint: { args: ["pointer"], returns: "int" },
    gina_stats: { args: ["pointer"], returns: "pointer" },
    gina_tables: { args: ["pointer"], returns: "pointer" },
    gina_free: { args: ["pointer"], returns: "void" },
    gina_version: { args: [], returns: "pointer" },
    gina_last_error: { args: ["pointer"], returns: "pointer" },
    gina_last_status: { args: [], returns: "int" },
  };
  const opt06 = {
    gina_select: { args: ["pointer", "pointer", "pointer"], returns: "pointer" },
    gina_create_index: { args: ["pointer", "pointer", "pointer"], returns: "int" },
    gina_drop_index: { args: ["pointer", "pointer", "pointer"], returns: "int" },
    gina_compact: { args: ["pointer"], returns: "int" },
  };

  let lib: any;
  let hasSelect = false, hasIndexes = false, hasCompact = false;
  try {
    lib = dlopen(libPath, { ...core, ...opt06 });
    hasSelect = hasIndexes = hasCompact = true;
  } catch {
    lib = dlopen(libPath, { ...core });
  }

  const F = lib.symbols;
  const take = (p: unknown): string | null => {
    if (!p) return null;
    const s = readCString(p);
    F.gina_free(p);
    return s;
  };

  return {
    name: "bun:ffi",
    hasSelect, hasIndexes, hasCompact,
    open: (dir, durable, ckpt, changelog, maxBlob) =>
      F.gina_open(cstr(dir), durable, ckpt, BigInt(changelog), BigInt(maxBlob)),
    close: (h) => F.gina_close(h),
    createTable: (h, n, j) => F.gina_create_table(h, cstr(n), cstr(j)),
    dropTable: (h, n) => F.gina_drop_table(h, cstr(n)),
    schema: (h, t) => take(F.gina_schema(h, cstr(t))),
    insert: (h, t, j) => F.gina_insert(h, cstr(t), cstr(j)),
    update: (h, t, k, j) => F.gina_update(h, cstr(t), cstr(k), cstr(j)),
    erase: (h, t, k) => F.gina_erase(h, cstr(t), cstr(k)),
    get: (h, t, k) => take(F.gina_get(h, cstr(t), cstr(k))),
    all: (h, t) => take(F.gina_all(h, cstr(t))),
    count: (h, t) => Number(F.gina_count(h, cstr(t))),
    exec: (h, j) => F.gina_exec(h, cstr(j)),
    knn: (h, t, c, q, dims, k, m) =>
      take(F.gina_knn(h, cstr(t), cstr(c), q, dims, k, m)),
    search: (h, t, c, q, lim) => take(F.gina_search(h, cstr(t), cstr(c), cstr(q), lim)),
    changes: (h, t, s) => take(F.gina_changes(h, cstr(t), BigInt(s))),
    watch: (h, t, cb) => {
      const c = new (ffi as any).JSCallback(
        (json: unknown) => { cb(readCString(json)); },
        { returns: "void", args: ["pointer", "pointer"] },
      );
      keepAlive.push(c);
      return Number(F.gina_watch(h, cstr(t), c.ptr, null));
    },
    unwatch: (h, id) => F.gina_unwatch(h, BigInt(id)),
    select: hasSelect ? (h, t, j) => take(F.gina_select(h, cstr(t), cstr(j)))
                      : () => { throw new Error("gina_select no disponible (lib 0.5)"); },
    createIndex: hasIndexes ? (h, t, c) => F.gina_create_index(h, cstr(t), cstr(c)) : () => 9,
    dropIndex: hasIndexes ? (h, t, c) => F.gina_drop_index(h, cstr(t), cstr(c)) : () => 9,
    checkpoint: (h) => F.gina_checkpoint(h),
    compact: hasCompact ? (h) => F.gina_compact(h) : () => 9,
    stats: (h) => take(F.gina_stats(h)),
    tables: (h) => take(F.gina_tables(h)),
    freeStr: (s) => F.gina_free(s),
    lastError: (h) => readCString(F.gina_last_error(h)),
    lastStatus: () => F.gina_last_status() as number,
    version: () => readCString(F.gina_version()),
  };
}

// ---------------------------------------------------------------------------
// Node (koffi)
// ---------------------------------------------------------------------------
async function makeKoffiBackend(libPath: string): Promise<Backend> {
  const { createRequire } = await import("node:module");
  const require = createRequire(import.meta.url);
  const koffi = require("koffi") as any;
  const lib = koffi.load(libPath);
  const keepAlive: unknown[] = [];

  const fn = (sig: string) => lib.func(sig);
  // char* de vuelta: se declara void* (puntero crudo), se decodifica y libera
  const f = {
    open: fn("void* gina_open(const char*, int32_t, uint32_t, uint64_t, uint64_t)"),
    close: fn("void gina_close(void*)"),
    createTable: fn("int32_t gina_create_table(void*, const char*, const char*)"),
    dropTable: fn("int32_t gina_drop_table(void*, const char*)"),
    schema: fn("void* gina_schema(void*, const char*)"),
    insert: fn("int32_t gina_insert(void*, const char*, const char*)"),
    update: fn("int32_t gina_update(void*, const char*, const char*, const char*)"),
    erase: fn("int32_t gina_erase(void*, const char*, const char*)"),
    get: fn("void* gina_get(void*, const char*, const char*)"),
    all: fn("void* gina_all(void*, const char*)"),
    count: fn("int64_t gina_count(void*, const char*)"),
    exec: fn("int32_t gina_exec(void*, const char*)"),
    knn: fn("void* gina_knn(void*, const char*, const char*, float*, uint32_t, uint32_t, int32_t)"),
    search: fn("void* gina_search(void*, const char*, const char*, const char*, uint32_t)"),
    changes: fn("void* gina_changes(void*, const char*, uint64_t)"),
    watch: fn("int64_t gina_watch(void*, const char*, void*, void*)"),
    unwatch: fn("int32_t gina_unwatch(void*, int64_t)"),
    checkpoint: fn("int32_t gina_checkpoint(void*)"),
    stats: fn("void* gina_stats(void*)"),
    tables: fn("void* gina_tables(void*)"),
    free: fn("void gina_free(void*)"),
    version: fn("void* gina_version()"),
    lastError: fn("void* gina_last_error(void*)"),
    lastStatus: fn("int32_t gina_last_status()"),
  };
  let hasSelect = false, hasIndexes = false, hasCompact = false;
  let fSelect: any = null, fCreateIx: any = null, fDropIx: any = null, fCompact: any = null;
  try {
    fSelect = fn("void* gina_select(void*, const char*, const char*)");
    hasSelect = true;
  } catch { /* lib 0.5 */ }
  try {
    fCreateIx = fn("int32_t gina_create_index(void*, const char*, const char*)");
    fDropIx = fn("int32_t gina_drop_index(void*, const char*, const char*)");
    hasIndexes = true;
  } catch { /* lib 0.5 */ }
  try {
    fCompact = fn("int32_t gina_compact(void*)");
    hasCompact = true;
  } catch { /* lib 0.5 */ }

  const take = (p: unknown): string | null => {
    // punteros de resultados (get/all/select/...): heap del motor → liberar
    if (!p) return null;
    const s = koffi.decode.string(p) as string;
    f.free(p);
    return s;
  };
  // gina_version/gina_last_error devuelven memoria INTERNA (no heap): leer
  // sin liberar (liberarlos sería UB).

  return {
    name: "koffi",
    hasSelect, hasIndexes, hasCompact,
    open: (dir, durable, ckpt, changelog, maxBlob) =>
      f.open(dir, durable, ckpt, BigInt(changelog), BigInt(maxBlob)),
    close: (h) => f.close(h),
    createTable: (h, n, j) => f.createTable(h, n, j),
    dropTable: (h, n) => f.dropTable(h, n),
    schema: (h, t) => take(f.schema(h, t)),
    insert: (h, t, j) => f.insert(h, t, j),
    update: (h, t, k, j) => f.update(h, t, k, j),
    erase: (h, t, k) => f.erase(h, t, k),
    get: (h, t, k) => take(f.get(h, t, k)),
    all: (h, t) => take(f.all(h, t)),
    count: (h, t) => Number(f.count(h, t)),
    exec: (h, j) => f.exec(h, j),
    knn: (h, t, c, q, dims, k, m) => take(f.knn(h, t, c, q, dims, k, m)),
    search: (h, t, c, q, lim) => take(f.search(h, t, c, q, lim)),
    changes: (h, t, s) => take(f.changes(h, t, BigInt(s))),
    watch: (h, t, cb) => {
      const proto = koffi.proto("void ginacb(const char*, void*)");
      const reg = koffi.register((json: unknown) => {
        cb(typeof json === "string" ? json : (koffi.decode.string(json) as string));
      }, koffi.pointer(proto));
      keepAlive.push(reg);
      return Number(f.watch(h, t, reg, null));
    },
    unwatch: (h, id) => f.unwatch(h, BigInt(id)),
    select: hasSelect ? (h, t, j) => take(fSelect(h, t, j))
                      : () => { throw new Error("gina_select no disponible (lib 0.5)"); },
    createIndex: hasIndexes ? (h, t, c) => fCreateIx(h, t, c) : () => 9,
    dropIndex: hasIndexes ? (h, t, c) => fDropIx(h, t, c) : () => 9,
    checkpoint: (h) => f.checkpoint(h),
    compact: hasCompact ? (h) => fCompact(h) : () => 9,
    stats: (h) => take(f.stats(h)),
    tables: (h) => take(f.tables(h)),
    freeStr: (s) => f.free(s),
    lastError: (h) => koffi.decode.string(f.lastError(h)) as string,
    lastStatus: () => f.lastStatus() as number,
    version: () => koffi.decode.string(f.version()) as string,
  };
}

export async function loadBackend(libPath?: string): Promise<Backend> {
  const path = libPath ?? defaultLibPath();
  const isBun = typeof (globalThis as any).Bun !== "undefined";
  if (isBun) return makeBunBackend(path);
  return makeKoffiBackend(path);
}
