# Arquitectura de GinaDB Lite 0.6

## Visión general

```
              ┌────────────────────────────────────────────────┐
   API C++    │ db["t"].emplace(...)|get|patch|erase|all       │
   (DSL)      │ where(eq|between|...)|where(lambda)            │
   (0.6)      │ order_by().limit()|knn|search|create_index     │
              │ watch|changes|begin()->commit()|compact()      │
              ├────────────────────────────────────────────────┤
   C ABI      │ gina_open/insert/get/knn/search/watch/...      │── libginadb.so
   (28 fn)    │ gina_select (planner) | índices | gina_compact │── CLI gina
              ├────────────────────────────────────────────────┤
   Motor      │ validación de schema (type-safe)               │
   (0.6)      │ B+Tree PK agrupado │ índices secundarios B+Tree│
              │ planner con pushdown │ FTS invertido            │
              │ changelog ring │ watchers │ dirty-tracking      │
              ├────────────────────────────────────────────────┤
   Storage    │ WAL (frames CRC32+LSN, batch por tx)           │
   (0.6)      │ Base: páginas slotted 4 KiB + overflow         │
              │ Delta: versiones + doble slot + free-list      │
              │ IC incremental │ FC compactación │ rebase WAL   │
              └────────────────────────────────────────────────┘
              Python (ctypes) · TypeScript (bun:ffi/koffi) ·
              Zig (lector binario puro + FFI) · cualquier C
```

## Modelo de datos

Una tabla tiene una **schema estricta** (columnas tipadas, PK de texto) o es
**schemaless** (acepta cualquier documento). Los valores son multimodales:
escalar + bytes + array + objeto anidado + **vector f32** (embeddings). Las
filas viven en un **B+Tree PK→fila** por tabla (0.6; antes `unordered_map`):
la memoria es la autoridad; el disco es WAL + base + delta. Cada escritura
marca la clave como *dirty* para el próximo checkpoint incremental.

## Flujo de escritura (commit)

```
writer_mu (serializa escritores)
  1. validar operación contra la schema actual
  2. append frames de la tx al WAL (una syscall write por frame)
  3. append frame COMMIT
  4. fsync (solo si durable=true)
  5. aplicar en memoria (B+Tree PK + índices secundarios + FTS + dirty-set)
  6. changelog ring (si hay capacidad) + construir eventos
  7. checkpoint periódico si toca (checkpoint_every → IC o FC)
liberar writer_mu
  8. dispatch de watchers (bajo dispatch_mu, fuera del writer)
```

Si algo falla antes del paso 5, el WAL queda con una tx sin COMMIT que el
recovery descarta — la atomicidad es estructural, no requiere deshacer nada
en memoria. Las transacciones multi-operación se validan **completas** antes
de escribir un solo byte: un error aborta sin dejar rastro en el WAL.

## Checkpoint (0.6: incremental base+delta)

Hay dos niveles. El **IC** (incremental) es el camino por defecto:

```
writer_mu
  W = lsn actual
  lote = { schemas dirty (o drops) } ∪ { filas dirty | tombstones }
  delta.pages:
    decidir copy-forward por página (≥50% muerta → reemitir vivos y liberar)
    asignar páginas: free-list derivada primero, luego EOF
    pwrite páginas nuevas → fsync
    flip del slot de encabezado (seq+1, watermark=W) → fsync   ← COMMIT
  limpiar dirty por tabla y catálogo
  rebase del WAL (truncar + frame BASE)
liberar
```

El **FC** (compactación, manual con `db.compact()` o automática cuando el
delta vivo supera `max(base_pages/4, 64)`) reescribe `data.pages` completo
(recorrido ordenado del B+Tree, sin ordenar), recalcula ids de tabla,
recrea el delta vacío y rebasea el WAL.

Crash-safety del IC: las páginas se escriben ANTES del flip y con fsync; el
flip atómico del doble slot es el único punto de commit. Al abrir, páginas
con `page_lsn > watermark` (IC que no llegó a voltearse, o hueco reutilizado
a medias) se ignoran. Crash a mitad de un FC: el delta viejo queda con
`watermark ≤ checkpoint_lsn` del base nuevo y se descarta entero.

La **free-list persistente es derivada**: libre = página sin registros vivos
o cadena overflow sin referencias. No hay estado redundante que corromper;
los huecos se reutilizan (menor id primero). Especificación byte a byte en
`docs/FORMATO_ARCHIVOS.md` §4.

## Recovery (apertura)

```
1. quitar .tmp huérfanos
2. si existe data.pages:
     validar magic/version (GNP5|GNP6) → cargar catálogo → cargar filas
     (bulk_load del B+Tree: el base llega ordenado por PK)
     checkpoint_lsn = C
3. si existe delta.pages:
     slot válido de mayor seq → watermark D
     si D > C: aplicar registros en orden (lsn, página):
       L_SCHEMA/L_DROP (catálogo) + L_ROW/L_OVF/L_TOMB (filas); C = D
     si D <= C (FC interrumpido): descartar el delta
4. reproducir wal.log:
     frames lsn <= C → ignorar
     tx activa → buffer; COMMIT → aplicar
     tx nueva distinta a la activa → la activa se descarta (crash viejo)
     cola corrupta/torn → truncar al último byte válido (reparación)
     lsn = max(lsn, C)   ← nunca regresar bajo el watermark
5. abrir WAL en append
```

## Concurrencia y locks

Orden de adquisición (nunca invertido): `writer_mu → catalog_mu → table.mu`.
Lectores toman shared locks (catálogo + tabla) y corren en paralelo entre sí
y con el escritor. Los callbacks de `watch()` se disparan **después** de
liberar `writer_mu`, bajo `dispatch_mu` (preserva el orden entre commits).
Restricción documentada: no llamar operaciones de escritura de la misma DB
dentro de un callback (el dispatch_mu lo bloquearía).

## Índices

- **PK (0.6)**: B+Tree agrupado por tabla (`src/btree.hpp`, orden 64, hojas
  enlazadas). get O(log N) con localidad de caché; `all()`/`order_by(pk)` y
  los checkpoints leen las hojas ya ordenadas; rangos PK directos e inversos
  con parada temprana. Diferencialmente testeado contra `std::map`.
- **Secundarios (0.6)**: un B+Tree por columna indexada — clave con
  codificación ordenable (int con signo, float totalOrder, texto, bytes,
  bool) → PKs ordenadas. `create_index`/`drop_index` son WAL atómicos (kind
  RESCH) y persisten en el schema. Se reconstruyen al abrir (base) y se
  mantienen por escritura y replay.
- **Planner (0.6)**: `where(eq|ne|gt|ge|lt|le|between|in|and|or)` elige vía
  de acceso (punto PK > rango PK > punto índice > rango índice > recorrido) y
  `order_by` sobre PK o columna indexada se sirve del orden del árbol con
  parada temprana en `limit`. `where(lambda)` = recorrido libre.
- **FTS**: índice invertido `token → set<PK>` por columna `.fts=true`;
  se mantiene en cada PUT/DEL (incluido el replay) y se reconstruye al
  cargar. Columnas de texto sin `.fts` usan fuerza bruta.
- **KNN**: escaneo con distancia coseno o L2, top-k con `partial_sort`.
  O(n·d) — suficiente para el rango "lite"; HNSW queda para >100K vectores.

## Real-time

- `watch(fn)`: registro global de watchers por tabla; los eventos llevan
  tipo (put/del), tabla, clave y fila completa.
- `changes(cursor)`: ring buffer por tabla con secuencias monótonas;
  `truncated=true` avisa que el cursor es más viejo que el historial retenido.
- Ambos son por proceso; no persisten entre reinicios (decisión "lite").

## Zig

- `ginaread.zig`: parser **de solo lectura** del WAL y del snapshot con la
  misma semántica de recovery que el motor (snapshot + replay post-LSN +
  descarte de tx abandonadas). Exporta `gina_read_*` (C ABI) y compila como
  lib estática y compartida. Un lenguaje sin FFI a C++ puede leer los datos
  directamente con este parser o reimplementándolo desde
  `docs/FORMATO_ARCHIVOS.md`.
- `gina.zig`: binding completo de escritura/lectura sobre `libginadb.so`
  (misma C ABI que usa Python ctypes).

## Nota de evolución del diseño

En 0.5 el índice PK era un hash y se razonó que añadir un B+Tree "encima"
del almacenamiento lineal duplicaría trabajo. 0.6 tomó el camino intermedio
lite: B+Tree **en memoria** como almacén de filas (con toda la maquinaria de
páginas del snapshot intacta para el base) + checkpoint incremental delta.
El siguiente paso natural (1.0) sigue siendo el page manager en disco
completo: buffer pool, B+Tree sobre page_id y MVCC.

---

## Evolución 0.7 — filas planas y reapertura

```
                    apertura (recover)
                          │
        ┌─────────────────┼──────────────────┐
        ▼                 ▼                  ▼
  pager::load       decode_base_records   delta + WAL
  (bytes crudos,    (PARALELO por         (payload crudo,
   CRC slice-by-8)   rangos contiguos,      pk-scan, solo
        │            rangos contiguos →     registros vivos)
        │            SRow plano)                 │
        │                 │                      │
        └────────┐        ▼                      ▼
                 └── bulk_load(Map<SRow>)   apply_row/erase
                                │           (sec incremental;
                                ▼            FTS perezoso)
                        sec_build (una pasada
                        + sort + bulk por índice,
                        EN PARALELO)
```

### Fila plana (SRow) — representación interna

- `flat: vector<Value>` indexado por **ordinal** de `def.columns`;
  `present: vector<u64>` máscara de bits (ausente ≠ null); `obj: Row`
  residual para schemaless/columnas legado. Materializar a `Row` (el
  contrato público) ocurre solo en el borde: `get/collect/eventos/JSON`.
- `ColMap` (nombre→ordinal, pk cacheada) por tabla, reconstruido en cada
  create/re-schema. Un re-schema con columnas distintas redistribuye las
  filas **por nombre** (`srow_rewidth`) sin descartar datos; con las mismas
  columnas (el único K_RESCH que emite el motor: índices) es no-op.
- Formato en disco SIN CAMBIOS: `encode_srow` escribe los nombres desde el
  schema en orden ordinal; `decode_srow` resuelve nombre→ordinal. Un
  archivo v6 escrito por 0.6 se lee igual (los nombres están en el archivo).

### Reapertura: cuatro costes eliminados

1. **CRC32 slice-by-8** (codec.cpp): tablas 8×256 constexpr; el CRC por-bit
   era ~390 ms @28 MB y se pagaba en base, WAL y delta.
2. **Decode paralelo**: `pager::load` entrega `RowView` crudos (punteros al
   buffer del archivo; overflow ensamblado en un deque estable); el motor
   decodifica por rangos contiguos en `open_threads` hilos (default 2) —
   el orden (tabla asc, PK asc) se preserva al concatenar, requisito del
   `bulk_load`.
3. **FTS perezoso**: `fts_built` por tabla; la primera búsqueda construye
   (unique_lock + doble chequeo); `index_row` se salta si no está construido.
4. **sec_build**: una pasada única de colecta (enc,pk) para TODOS los
   índices + sort + bulk_load por índice en paralelo (work-stealing; los
   árboles se pre-crean para punteros estables sin rehash).

### Delta: almacén versionado de bytes

- `Rec.payload` guarda el registro crudo; la PK se deriva con **pk-scan**
  (recorrido del frame que solo decodifica el valor de la columna PK).
- El lote del IC llega YA codificado (el motor codifica bajo el lock de
  tabla con el def vivo); el copy-forward re-emite bytes sin decode+encode.
- Apply: solo registros de FILA vivos se decodifican (con el def de su
  tabla); los estructurales se aplican SIEMPRE (un schema supersededo sigue
  siendo la creación de la tabla — bug 0.7 corregido, ver INFORME §4.3).

### Integridad del delta — clasificación de páginas ilegibles

```
página ilegible (magic/flags/CRC mal) dentro del extent comprometido [2, pc)
 ├─ magic DATA legible y page_lsn > watermark → suspect  (torn de IC sin
 │                                                flip: normal, hueco libre)
 └─ cualquier otra cosa → corrupt (rotura de MEDIO: pérdida real de
                          registros, reportada en stats)
cadena overflow rota → broken_ovf_records (registro perdido, la base ABRE)
```

El lector Zig implementa la misma clasificación (paridad verificada sobre
la misma BD dañada) y `gina-inspect` muestra los tres contadores.

### KNN (mejora de la fuerza bruta)

Heap top-k con comparador `better` (el frente del heap es el PEOR de los k
guardados — el candidato a expulsar); abandono temprano por dimensión en L2
contra el frente; desempate determinista por clave. Acceso a la columna
por ordinal.
