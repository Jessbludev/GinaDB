# Changelog

## 0.7.0 — "Filas Planas + Integridad del Delta"

Reapertura **×5,4 más rápida** (1237→228 ms @100K filas) mediante filas
planas internas, decode paralelo del base, CRC32 slice-by-8 y FTS perezoso.
**Misma API, mismo ABI, mismo formato en disco**: 0.7 abre bases 0.5/0.6
sin migración y los clientes (TS/Python/Zig) no cambian.

### Filas planas (SRow)

- La fila interna deja de ser `std::map<string,Value>` (nodo por columna)
  y pasa a `vector<Value>` por **ordinal de columna** con máscara de
  presencia (ausente ≠ null) y mapa residual para schemaless/legado
  (`src/srow.hpp`). El formato en disco NO cambia: los pares nombre-valor
  v6 siguen en el archivo, el schema hace de puente.
- Materialización a `Row` solo en el borde de la API (`get`/`collect`/
  eventos/JSON); el planner evalúa condiciones tipadas contra la fila
  plana por ordinal. `update` fusiona parches sobre copia plana.
- `pager::load` entrega registros crudos; el motor decodifica en paralelo
  por rangos contiguos (`open_threads`, orden preservado para el bulk_load).

### Rendimiento (todo medido, misma caja — ver docs/INFORME-0.7.md)

- **CRC32 slice-by-8** (tablas constexpr, verificado idéntico): el CRC era
  ~390 ms de la apertura de un base de 28 MB; también acelera WAL y delta.
- Lectura masiva de archivos (fin de `istreambuf_iterator` byte a byte).
- **Reapertura @100K+10K: 1237→228 ms (×5,4)**; con delta: ×4,4.
- `sec_build`: una sola pasada de colecta para todos los índices + sort +
  bulk_load por índice **en paralelo**.
- **FTS perezoso**: no se construye al abrir; lo paga la primera búsqueda.
- Delta como almacén de **bytes versionados**: pk-scan sin decodificar,
  copy-forward sin decode+encode, lote del IC codificado bajo lock.
- **KNN**: heap top-k (memoria O(k)) + abandono temprano por dimensión en
  L2 + desempate determinista por clave (+22% medido).
- Workload 5K (bun): carga +29%, CRUD +11% (p50 16→13 µs), IC ×2,2, FC ×3,1.

### Integridad del delta (problema 0.6: "se pierde sin error")

- Clasificación de páginas ilegibles: `suspect` (torn de IC sin flip,
  normal) vs **`corrupt`** (rota dentro del extent comprometido: pérdida
  real, detectada y reportada por `stats()`).
- **Cadenas overflow rotas ya no tumban la base**: el registro se pierde,
  se cuenta (`delta_broken_ovf_records`) y la base sigue operativa.
- Contadores nuevos en Stats/JSON/TypeScript/**gina-inspect**, con
  **paridad motor vs lector Zig** verificada sobre la misma BD dañada.

### Correcciones (cazadas por las propias suites de esta iteración)

- Tombstones del IC escritos con layout `L_ROW`: los borrados resucitaban
  al reabrir (crash suite).
- Comparador del heap KNN invertido (unit test).
- Delta-apply saltaba schemas *supersededos* que eran la creación de la
  tabla → las filas de ICs anteriores se perdían (validación cruzada Zig;
  escenario determinista + fuzz con claves frías como red).
- `workload.ts` 0.6 tenía **3 bucles infinitos** (`skipQueries ? 0 : 500`
  como condición) — el harness nunca completó una corrida full.

### Tests

- 177 unit + **75 crash** (+8 escenarios: 2 de corrupción de delta,
  regresión de schema supersededo, fuzz con 5000 claves) + btree
  diferencial; bun 11/11; node smoke; python 24/24; zig build+test;
  bancos nuevos: `make bench-reopen`, `make crc-check`, `make xval`.

## 0.6.0 — "Índices + Checkpoint Incremental"

B+Tree primario, índices secundarios con planner, checkpoint incremental
base+delta con free-list persistente derivada, `gina_select`, y cliente
TypeScript (Bun/Node). **Abre bases 0.5 y 0.4** sin migración.

### Índices

- **B+Tree primario agrupado** (`src/btree.hpp`): hojas enlazadas, orden 64,
  bulk-load, rangos directos e inversos con parada temprana. Reemplaza al
  hash como almacén de filas: `all()` y `order_by(pk)` salen ordenados sin
  ordenar, los rangos PK son O(rango), y el checkpoint base ya no ordena.
  Diferencialmente testeado contra `std::map` (180K+ operaciones, 3 semillas).
- **Índices secundarios** por columna: codificación ordenable de valores
  (int con signo, float IEEE totalOrder, texto, bytes, bool; null/array/
  object/vector fuera de índice), árboles B+Tree reutilizados, mantenimiento
  incremental en cada escritura. `create_index`/`drop_index` atómicos vía WAL
  (kind RESCH) y persistentes en el schema (sección v6, compatible con
  payloads v5).
- **Planner de consultas**: condiciones tipadas `eq/ne/gt/ge/lt/le/between/
  in/all_of/any_of` con pushdown (PK punto > PK rango > índice punto >
  índice rango > recorrido) y `order_by` servido por el orden del árbol
  (PK o columna indexada) con parada temprana en `limit`. `where(lambda)`
  se mantiene como recorrido libre.

### Checkpoint incremental (base + delta)

- `delta.pages`: versiones de registros cambiados desde el último checkpoint
  completo (filas, tombstones, schemas, drops) en páginas slotted con
  `page_lsn`. Encabezado de **doble slot** cuyo flip atómico es el punto de
  commit; las páginas de un IC sin comprometer se ignoran al abrir.
- **Free-list persistente derivada**: las páginas libres se derivan del
  contenido (registros supersados, cadenas sin referencias) — no hay estado
  redundante que corromper; los huecos se reutilizan (menor id primero).
- **Copy-forward** por página (≥50% muerta → reemitir vivos y liberar) y
  **compactación** (FC) manual (`db.compact()`) y automática (delta vivo >
  max(base/4, 64 páginas)). El WAL se rebasea tras cada checkpoint.
- Recovery: base → delta (si watermark > base) → WAL. Crash-mid-FC detectado
  y cubierto por tests (delta obsoleto se descarta).

### Interoperabilidad

- **C ABI**: `gina_select` (JSON: where/order/limit/fields con planner),
  `gina_create_index`, `gina_drop_index`, `gina_compact` (24 → 28 funciones).
- **Cliente TypeScript** (`examples/typescript`): tipado completo de valores
  (bytes/vector round-trip), backend dual **bun:ffi** (sin dependencias) y
  **koffi** (Node), watch con callbacks, `GinaError` con códigos.
- **Workload real de datos** (`src/workload.ts`): 13 fases (carga, gets,
  índices, rangos, FTS, KNN, CRUD, watch, IC/FC, reopen, crash SIGKILL con
  verificación de prefijo contiguo) con métricas p50/p95/p99; corre igual en
  Bun y Node y contra la lib 0.5 (fallback all+filter) para comparar.
- **Python**: `select`, `create_index`/`drop_index`, `compact`, `indexes` en
  create + `verify06.py` (24 comprobaciones incl. crash real).
- **Zig**: lector puro entiende formato v6 y **parsea `delta.pages`** completo
  (validación cruzada byte a byte contra el motor C++); FFI con select/índices/
  compact; `gina-inspect` muestra delta e índices.

### Verificación

- 228 comprobaciones C++ (btree diferencial + 177 unit + 51 crash, incluidos
  5 escenarios nuevos de delta y un fuzz de 4.000 operaciones con IC/FC/
  reopen/índices/blobs), 11 tests TypeScript, 24 comprobaciones Python,
  validación cruzada Zig. Bugs encontrados por el propio workload y
  corregidos: índices no construidos al crear tabla (eq degradaba a scan),
  koffi.decode/pointer bugs del cliente, await de hijo ya muerto en el test
  de crash. Detalle en `VERIFICACION.md` e `INFORME-0.6.md`.

## 0.5.0 — "Multimodal + Real Storage"

Motor de almacenamiento real + multimodalidad + real-time + interoperabilidad
multi-lenguaje. Rompe la API de 0.4 (nueva DSL) pero **abre y migra bases
0.4** automáticamente.

### Storage (el roadmap del verificador 0.4, completo)

- Snapshot en páginas slotted de **4 KiB** con header documentado, CRC32 por
  página, catálogo en páginas y **overflow pages** para blobs.
- **Checkpoint LSN-aware** con escritura atómica (tmp + fsync + rename +
  fsync de directorio).
- **WAL truncation/rebase**: el WAL se vacía tras cada checkpoint (frame
  BASE). Recovery O(filas vivas) en lugar de O(historial).
- Recovery incremental con **reparación automática de cola corrupta**
  (torn writes, CRC/magic corruptos, basura).
- Compatibilidad: abre WAL v4 (bases 0.4) y las convierte al primer
  checkpoint.

### Correcciones sobre 0.4

- El checkpoint ahora **se lee** al arrancar (0.4 lo escribía y jamás lo usaba).
- `primary_key` de la schema se respeta en todo el camino (0.4 hardcodeaba
  `"id"`).
- Operaciones sobre tablas inexistentes lanzan error (0.4 las perdía en
  silencio).
- Decodificación con bounds-check en todos los caminos (0.4 leía fuera de
  límites con frames truncados).
- `erase()` devuelve si la clave existía (0.4 siempre `true`).
- fsync correcto (fd propio) + fsync del directorio tras renombrar.
- Validación de schema real: tipos, nullable, PK inmutable, límite de blob.

### Multimodal

- Nuevo tipo **Vector** (embedding f32) + **KNN** por coseno/L2 con top-k.
- **Full-text search** con índice invertido por columna (`.fts=true`) y
  fallback por escaneo.
- **Blobs** con páginas de overflow (verificación byte-exacto, hasta 16 MiB
  por valor).
- Tablas **schemaless** (modo documento híbrido) junto a tablas estrictas.

### Sintaxis nueva (DSL)

- `db["users"].emplace("id","u1","age",34)` — pares clave/valor variádicos.
- `patch()` para updates parciales; `where().order_by().limit().collect()`.
- Transacciones: `tx["users"]...; tx.commit()` con validación completa antes
  de escribir el WAL (abort sin dejar bytes).
- `watch()` (callbacks por commit) y `changes(cursor)` (changelog con ring).

### Interoperabilidad

- **C ABI** (`libginadb.so`, 24 funciones, JSON, `gina_exec` batch atómico).
- CLI **`gina`** (cualquier lenguaje vía subprocess).
- Cliente **Python** (ctypes) de ejemplo.
- **Zig 0.16**: parser de lectura puro (WAL + snapshot, exporta
  `gina_read_*`), FFI completo (`gina.zig`) e inspector `gina-inspect`.

### Herramientas y pruebas

- 102 pruebas unitarias + 28 de crash/recovery + 4 de parser Zig.
- Benchmark nuevo con p50/p95 por operación y escenario
  recovery-vs-historial.
- Documentación en español: README, ARQUITECTURA, FORMATO_ARCHIVOS
  (especificación binaria byte a byte), BENCHMARKS, VERIFICACION.

### Rendimiento (misma máquina/flags, ver docs/BENCHMARKS.md)

- write ×2.4, transaction ×24.5, recovery con historial ×37, get ×1.4.

## 0.4 — referido

Índice PK `unordered_map`, locks por tabla, batch WAL por transacción,
fsync opcional, checkpoint periódico atómico. Deudas conocidas (verificadas
en esta iteración): checkpoint nunca leído, recovery O(WAL), `keyof` fijo,
pérdida silenciosa en tablas inexistentes, OOB en recovery, Zig decorativo.
