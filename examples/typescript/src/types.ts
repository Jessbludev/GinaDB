// GinaDB-Lite 0.6 — tipos públicos del cliente TypeScript.
// Espejo exacto del modelo C++ (include/gina/value.hpp + db.hpp):
//   bytes  <-> Uint8Array  (JSON: {"$bytes":"<b64>"})
//   vector <-> number[]    (JSON: {"$vec":[f32,...]})  — embeddings f32

export type GinaValue =
  | null
  | boolean
  | number // int (safe integer) o float
  | string
  | Uint8Array
  | GinaValue[]
  | { [k: string]: GinaValue }
  | number[]; // vector (embedding)

export type Row = { [k: string]: GinaValue };

export type ColumnType =
  | "null"
  | "bool"
  | "int"
  | "float"
  | "text"
  | "bytes"
  | "array"
  | "object"
  | "vector";

export interface Column {
  name: string;
  type: ColumnType;
  nullable?: boolean;
  fts?: boolean;
}

export interface Schema {
  name?: string;
  primary_key?: string;
  schemaless?: boolean;
  columns?: Column[];
  indexes?: string[];
}

export interface OpenOptions {
  durable?: boolean; // fsync del WAL en cada commit (default false)
  checkpointEvery?: number; // commits entre checkpoints (default 1000, 0 = manual)
  changelogCapacity?: number; // eventos retenidos por tabla (default 4096)
  maxBlobSize?: number; // límite por valor bytes (default 16 MiB)
}

// ---- select -----------------------------------------------------------------
export type CondOp =
  | "eq"
  | "ne"
  | "gt"
  | "ge"
  | "lt"
  | "le"
  | "between"
  | "in"
  | "and"
  | "or";

export interface Cond {
  op: CondOp;
  col?: string;
  val?: GinaValue;
  lo?: GinaValue;
  hi?: GinaValue;
  vals?: GinaValue[];
  args?: Cond[];
}

export interface OrderSpec {
  col: string;
  desc?: boolean;
}

export interface SelectQuery {
  where?: Cond;
  order?: (string | OrderSpec)[];
  limit?: number;
  fields?: string[]; // proyección
}

// ---- resultados ---------------------------------------------------------------
export interface KnnHit {
  key: string;
  score: number;
}

export interface GinaEvent {
  type: "put" | "del";
  table: string;
  key: string;
  row: Row | null;
}

export interface ChangeItem {
  seq: number;
  event: GinaEvent;
}

export interface ChangeList {
  next_cursor: number;
  truncated: boolean;
  events: ChangeItem[];
}

export interface Stats {
  tables: number;
  rows: number;
  last_lsn: number;
  commits: number;
  checkpoints: number;
  pages: number;
  overflow_pages: number;
  wal_bytes: number;
  data_bytes: number;
  delta_bytes: number;
  delta_pages: number;
  delta_live_pages: number;
  delta_free_pages: number;
  delta_suspect_pages: number;
  /** 0.7+: páginas del delta rotas DENTRO del extent comprometido (medio) */
  delta_corrupt_pages?: number;
  /** 0.7+: cadenas overflow rotas del delta (registros perdidos) */
  delta_broken_ovf_records?: number;
  compactions: number;
  recovered_bad_tail: number;
}

export interface ExecOp {
  op: "put" | "del";
  table: string;
  row?: Row;
  key?: string;
}

export const Status = {
  OK: 0,
  ENOENT: 1,
  EEXISTS: 2,
  ESCHEMA: 3,
  EKEY: 4,
  ETX: 5,
  EIO: 6,
  ECORRUPT: 7,
  ELIMIT: 8,
  EARG: 9,
  ERROR: -1,
} as const;

export type Status = (typeof Status)[keyof typeof Status];
