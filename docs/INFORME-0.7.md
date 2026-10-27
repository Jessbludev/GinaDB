# Informe de ingeniería — GinaDB Lite 0.7

**Objetivo de la iteración** (pedido del usuario): atacar el **decode de
filas planas** — el mayor cuello de botella medido del 0.6 (~850 ms de
hidratación de `std::map` por fila en la reapertura a 100K filas) — y los
**problemas definidos en el informe 0.6**: coste de construcción de índices
al abrir, corrupción del delta silenciosa, KNN por fuerza bruta y costes
residuales de checkpoints.

Este informe documenta el ciclo completo: qué se midió primero, qué se
construyó, qué cuellos encontraron las mediciones nuevas, qué se corrigió
(con números antes/después) y qué costes residuales quedan medidos para la
próxima iteración.

---

## 1. Metodología: medir antes de tocar

Antes de escribir una línea, se fijó la línea base con un banco focalizado
(`benchmark/reopen_bench.cpp`, misma forma de datos que el workload:
100K usuarios + 10K documentos con embeddings de 64 dimensiones, 4 índices
secundarios, FTS en bio/body) y el desglose por fases que ya trae el motor
compilado con `-DGINA_TRACE`. El desglose descompuso los ~850 ms "de decode"
en tres costes distintos que el número agregado del 0.6 escondía:

| Fase de la reapertura (0.6, esta caja) | ms | Causa raíz |
|---|---:|---|
| CRC32 bit a bit de las páginas del base | ~390 | implementación naive O(8·n) por bit |
| decode filas → `std::map` + **copia** del mapa a la lista | ~250 | nodo por columna + copia oculta en `pending` |
| FTS (tokenización durante el load) | ~120 | interleaved con el decode |
| Lectura del archivo (`istreambuf_iterator`) | (incluido arriba) | copia byte a byte |
| `bulk_load` B+Tree | ~7 | ya óptimo (0.6) |
| `sec_build` (4 índices, acumulación en `std::map`) | ~200 | 4 pasadas + nodos de mapa |
| **Reapertura total** | **1237** | |

**Conclusión del diagnóstico**: el "cuello de las filas" era en realidad
*cuatro* cuellos distintos. Atacar solo la estructura de la fila habría
dejado 610 ms en la mesa. La iteración atacó los cuatro.

## 2. Qué se construyó

### 2.1 Filas planas `SRow` (`src/srow.hpp/.cpp`)

La fila interna deja de ser `std::map<std::string, Value>` (un nodo de árbol
rojo-negro con su string de clave **por columna por fila**) y pasa a ser:

- `flat`: `std::vector<Value>` indexado por **ordinal de columna** del
  schema (una asignación por fila en vez de N);
- `present`: máscara de bits que preserva la distinción **ausente ≠ null**
  que el mapa daba gratis (una fila puede omitir una columna nullable);
- `obj`: mapa residual para tablas **schemaless** y columnas legado
  desconocidas — la semántica de consulta es "primero flat, luego obj", así
  que el caso mixto es siempre correcto y no se descarta ningún dato.

Cada tabla gana un `ColMap` (nombre → ordinal, PK cacheada) reconstruido en
cada create/re-schema. **El formato en disco NO cambia**: los pares
nombre-valor v6 siguen en el archivo; `encode_srow`/`decode_srow` hacen el
puente usando el schema (los nombres salen de `def.columns`). Eso mantiene
abierto el camino 0.5→0.6→0.7 y el lector binario Zig sigue siendo válido
sin cambios de formato — verificado byte a byte (§5).

**Dónde toca**: `Table::rows` es ahora `btree::Map<SRow>`; `apply_row`,
`sec_add/remove`, `keyof`, FTS y el planner trabajan sobre la fila plana por
ordinal; la materialización a `Row` (el contrato público de la API) ocurre
**solo en el borde**: `get()`, `collect()`, eventos de watch/changelog y
JSON del C ABI. El planner evalúa condiciones tipadas contra la fila plana
sin materializar nada — y materializa únicamente los supervivientes al
`limit`.

### 2.2 Decode del base en paralelo con registros crudos

El pager ya no decodifica filas: `pager::load` valida páginas/CRC y entrega
**vistas crudas** (puntero+longitud al buffer del archivo, con los cuerpos
overflow ensamblados en un deque estable). El motor decodifica a `SRow` con
`decode_base_records`, en paralelo por **rangos contiguos** de registros
(`open_threads`, por defecto 2, tope 4): el base llega ordenado por
(tabla, PK) y los rangos contiguos preservan ese orden al concatenarse —
requisito del `bulk_load`. Las excepciones de los hilos se capturan y se
relanzan en el hilo principal.

### 2.3 CRC32 slice-by-8 y lectura masiva

El CRC era el mayor coste individual (390 ms por apertura a 28,5 MB — y lo
mismo paga el replay del WAL y la apertura del delta). Se reemplazó por una
implementación **slice-by-8** con tablas de 8×256 entradas generadas en
`constexpr`, verificada **idéntica a la anterior** en 17 longitudes y
patrones (`make crc-check`). La lectura de `data.pages`/`delta.pages`
pasa de `istreambuf_iterator` (copia byte a byte) a una única `read()`
dimensionada con `tellg`.

### 2.4 Índices: una pasada + sort + bulk, en paralelo; FTS perezoso

- `sec_build` recolecta (clave codificada, PK) de **todos** los índices en
  una sola pasada por la tabla (0.6 hacía una pasada por índice) y luego
  ordena y construye cada árbol **en paralelo** (work-stealing entre hilos;
  los árboles se pre-crean para tener punteros estables). Los PK de cada
  run salen ya ordenados — el invariante que exige el `bulk_load`.
- **FTS perezoso**: el índice invertido NO se construye al abrir. La
  primera búsqueda que lo necesita lo construye (una sola vez, bajo lock
  exclusivo con doble chequeo); el mantenimiento incremental solo corre
  cuando ya está construido. Una reapertura que no busca texto no paga los
  ~218 ms que costaba a 100K filas.

### 2.5 Delta como almacén versionado de bytes

La capa delta deja de decodificar filas: `Rec` guarda el **payload crudo**
(bytes v6, schema-agnóstico).

- La PK de cada registro se deriva con un **pk-scan**: un recorrido del
  frame que solo decodifica el valor de la columna PK (espejo exacto del
  value-codec, con salto de payloads). Los registros muertos no cuestan
  decode: el motor solo decodifica los vivos, con el def de su tabla.
- El **copy-forward** del checkpoint re-emite los mismos bytes sin
  decode+encode; el lote del IC llega ya codificado (el motor codifica
  bajo el lock de tabla, con el def vivo) — el "lote listo" del primer IC
  a 110K filas pasó de copiar 110K mapas a codificar bytes.
- Los registros de fila muertos se saltan en el apply; los **estructurales
  se aplican siempre** — la corrección de un bug que precisamente esta
  reorganización destapó (§4.1).

### 2.6 Integridad del delta: clasificar y reportar

La limitación 0.6 era "una página comprometida dañada se cuenta como hueco
y sus registros se pierden **sin error**". El 0.7 distingue:

- `suspect_pages`: ilegible con `page_lsn > watermark` → **torn normal** de
  un IC sin flip (reutilización de un hueco libre); la base está sana.
- `corrupt_pages`: rota **dentro del extent comprometido** (lsn ilegible o
  ≤ watermark) → **corrupción de medio con pérdida real de registros**,
  detectada y reportada en `stats()` (y en JSON, TS y `gina-inspect`).
- `broken_ovf_records`: cadenas overflow rotas — antes mataban la apertura
  con `Corruption`; ahora el registro se pierde **pero se cuenta** y la
  base abre y sigue operativa (el hueco se reutiliza y sanea).

Todo expuesto en `Stats` (`delta_corrupt_pages`, `delta_broken_ovf_records`)
y con **paridad en el lector Zig**: sobre la misma base dañada, motor y
lector independiente clasifican idéntico (§5).

### 2.7 KNN: heap top-k y abandono temprano

El 0.6 puntuaba TODAS las filas (vector gigante de (clave, score) con copia
de cada PK) y ordenaba al final. El 0.7 mantiene un **heap de tamaño k**
(memoria O(k), copia de clave solo para quien entra), con **abandono
temprano por dimensión en L2** (si la distancia parcial ya excede la peor
de las k guardadas, se corta el bucle) y **desempate determinista por
clave** (el orden ante empates exactos ya no depende del algoritmo de
ordenación). El acceso a la columna es por ordinal (sin búsqueda en mapa).

## 3. Mediciones

### 3.1 Reapertura — el objetivo de la iteración

Banco focalizado, 100K usuarios + 10K documentos (base 28,5 MB), misma caja:

| | 0.6 | 0.7 | ganancia |
|---|---:|---:|---:|
| Reapertura tras compactación | **1237 ms** | **228 ms** | **×5,4** |
| Reapertura con delta (500 updates) | 1364 ms | 312 ms | ×4,4 |

Desglose de los 228 ms: lectura+CRC 22 · decode paralelo ~33 · bulk_load 9 ·
índices secundarios 87 · FTS **0** (perezoso) · misc. La ganancia **crece
con el tamaño** (todo lo eliminado era O(base)); a 5K filas la mejora es
×2,5 porque el término constante pesa relativamente más.

### 3.2 Workload real (mismo harness, Bun, 5K+2K, semilla fija)

| Fase | 0.6 | 0.7 | Δ |
|---|---:|---:|---:|
| carga users (lotes 500) | 48.971 ops/s | 63.403 ops/s | +29% |
| carga docs + embeddings | 27.596 | 31.918 | +16% |
| get puntual PK (p50) | 7 µs | 6 µs | ≈ |
| select eq email (índice, p50) | 10 µs | 9 µs | +10% |
| rango age + order desc (p50) | 532 µs | 500 µs | +6% |
| rango PK inverso + limit (p50) | 27 µs | 23 µs | +16% |
| FTS bio (AND 2 términos) | 2.345 ops/s | 2.331 ops/s | ≈* |
| KNN k=10 @2K×64 (p50) | 321 µs | 265 µs | **+22%** |
| CRUD mixto (p50) | 16 µs | 13 µs | +11% |
| watch (eventos/s) | 36.277 | 41.037 | +13% |
| IC con todo dirty | 65 ms | 29 ms | ×2,2 |
| compactación (FC) | 47 ms | 15 ms | ×3,1 |
| segundo IC (dirty pequeño) | 3,7 ms | 1,9 ms | ×1,9 |
| **reabrir (base+delta+WAL)** | **84 ms** | **33 ms** | **×2,5** |
| recuperación tras SIGKILL | 193–288 ms | 136–234 ms | ~×1,25 |

\* El FTS es deliberadamente igual de rápido en estado caliente: el coste
de construcción se movió del arranque a la primera búsqueda (lazy).

### 3.3 Durabilidad bajo crash real

Tres rondas de SIGKILL por runtime (Bun, Node y Python en sus suites):
**prefijo contiguo SÍ** en todas — la garantía del WAL no cambia. La
recuperación mejoró ~×1,25 de rebote del CRC (el replay del WAL verifica
frames con el mismo crc32 slice-by-8).

### 3.4 Costes residuales medidos (no todo son ganancias)

| Coste | Medida | Causa |
|---|---|---|
| delta-apply con updates aleatorios | 106 ms / 500 updates | **mantenimiento de índices de baja cardinalidad**: cada update hace erase+insert en el vector de PKs de `city` (~14K PKs → ~0,5 MB de shift por operación). Es el MISMO coste que paga el commit normal (no es sobrecoste de recovery) — el 0.6 lo tenía idéntico, ahora está aislado y medido. |
| Primera búsqueda FTS tras abrir | ~218 ms @100K | construcción perezosa única (amortizada; 0 en el arranque) |
| colecta de sec_build | ~27 ms @100K×3 índices | `srow_get` con hash por fila e índice |
| memoria RSS en carga | 149→150 MB | fila plana ≈ mapa en este shape; gana en filas anchas |

## 4. Cuellos encontrados por las pruebas → correcciones

El ciclo pedido (probar → diagnosticar → corregir) produjo **cuatro bugs
nuestros** esta iteración, todos cazados por las propias suites antes de
empaquetar:

### 4.1 Los tombstones perdían su layout (cazado por crash suite)

**Síntoma:** tras "500 updates + IC + reabrir", las claves borradas
**resucitaban** y el conteo fallaba. **Causa:** al unificar la emisión del
IC en `emit_payload` (bytes crudos), los `L_TOMB` se escribían con layout
`L_ROW` — el lector intentaba decodificar la clave serializada como si
fuera una fila. **Corrección:** el layout viaja con el payload; las
tumbas conservan `L_TOMB` en disco. Lección: cuando un tipo se serializa
como "bytes opacos", el discriminante de tipo también es dato.

### 4.2 Comparador del heap KNN invertido (cazado por unit test)

**Síntoma:** `knn` devolvía el *peor* resultado en el tope. El comparador
del heap estaba invertido respecto a la convención de `std::push_heap`
(el frente es el máximo del orden del comparador — para expulsar al peor,
el comparador debe ser `better`). Corregido con desempate determinista.

### 4.3 Los schemas muertos también crean tablas (cazado por el lector Zig)

**Síntoma:** la validación cruzada con el lector binario Zig — el mismo
estado, 12 filas — devolvió **2** en el motor. **Causa:** la optimización
"saltar registros muertos" del delta-apply saltaba el schema **v1
supersededo**; pero ese registro era la *creación* de la tabla que las
filas del IC#1 necesitaban para aplicarse. Las filas caían en silencio.
**Corrección:** solo se saltan registros de FILA muertos (su supersesor
posterior gana); los estructurales se aplican siempre. **Red de seguridad
añadida:** escenario 18 determinista en la crash suite + fuzz con espacio
de claves ×10 (claves frías cuyas versiones viejas siguen vivas) — porque
el fuzz original, con 500 claves calientes, nunca producía la
interlección: todas las filas viejas terminaban supersededas.

### 4.4 El harness 0.6 tenía tres bucles infinitos (cazado al correrlo de verdad)

**Síntoma:** el workload contra la lib 0.6 no terminaba jamás. **Causa:**
`for (let i = 0; skipQueries ? 0 : 500; i++)` — la "condición" es la
constante `500`: bucle infinito cuando las fases de consultas están
activas. Los números de consultas del informe 0.6 salieron de bancos
focalizados; **el harness empaquetado en 0.6 nunca pudo completar una
corrida completa**. Corregido en las tres fases. Lección: un benchmark
que no termina también es un resultado — y los tests deben correr el
camino completo, no solo sus piezas.

## 5. Verificación multi-lenguaje (detalle en `VERIFICACION.md`)

| Lenguaje | Mecanismo | Resultado |
|---|---|---|
| C++ | nativo | btree diferencial ✓ + **177** unit ✓ + **75** crash ✓ (8 escenarios nuevos: 2 de corrupción de delta, 1 de regresión de schema supersededo, fuzz con claves frías) |
| TypeScript | bun:ffi | 11/11 ✓ + workload completo de 13 fases + 3 SIGKILL con prefijo contiguo |
| TypeScript | Node 24 + koffi | workload completo ✓ (reapertura 13 ms @4,5K, crash ✓) |
| Python | ctypes | 24/24 ✓ incl. SIGKILL real (34.126 commits, prefijo contiguo, 29,5 ms) |
| Zig | lector binario puro | misma BD con delta: **12 filas = 12** motor, u3=99, u4 ausente, tabla borrada fuera, blob 9000 B intacto; sobre BD **corrupta**: motor y Zig clasifican idéntico (corrupt=1, 2 filas) — paridad de la nueva semántica |
| Zig | FFI (`gina.zig`) | demo completa contra 0.7 (select con índice, rango, compact, stats nuevos) |
| C | C ABI (`cabi.h`) | CLI `gina` smoke completo (create/select/index/compact/stats) |

Formato en disco: **sin cambios** — 0.7 abre bases 0.5 y 0.6; el lector Zig
sin modificar el parser de filas (solo aprendió la clasificación nueva).

## 6. Limitaciones conocidas (actualizadas)

1. **Páginas overflow sin CRC** (base y delta): el payload de un blob
   corrompido pasa inadvertido — la cadena rota se detecta, el contenido
   dañado con cadena intacta no. Mitigación futura: CRC en páginas GNOV
   (cambio de formato v7).
2. **Corrupción de delta pierde los registros de esa página** — ahora se
   **detecta y reporta** (este era el problema definido del informe 0.6),
   pero la recuperación del contenido sigue siendo imposible por diseño
   sin redundancia (el WAL ya está rebaseado tras el IC).
3. **Índices de baja cardinalidad**: los vectores de PKs pagan O(n) en
   insert/erase aleatorio (§3.4). Estructural para el diseño actual de
   postings; roadmap.
4. **KNN sigue siendo fuerza bruta** (mejorada: heap + abandono temprano
   L2 + acceso por ordinal, +22% medido) — HNSW/IVF siguen en el roadmap
   para >100K vectores.
5. **Un proceso por base** (por diseño lite, como 0.4–0.6).

## 7. Trabajo futuro priorizado (por impacto medido)

1. **Postings de índice con inserción/borrado O(log n)** (árbol pequeño o
   vector con huecos): elimina el coste dominante de escritura con índices
   de baja cardinalidad (los ~0,5 MB de shift por update medidos).
2. **CRC en páginas overflow** (formato v7): cierra el último hueco de
   detección de corrupción de medio.
3. **Decode columnar del base**: los valores de columnas fijas podrían
   decodificarse por bloques (SIMD-able) en vez de registro a registro —
   siguiente salto del reopen tras los 33 ms actuales de decode.
4. **HNSW/IVF** para KNN a escala (>100K vectores).
5. **`sec_build` perezoso opcional** para tablas con índices que no se
   consultan al arranque (hoy el reopen siempre los construye).

## 8. Conclusión

El objetivo — matar el decode de filas — se cumplió con holgura y, al
medir antes, resultó ser cuatro objetivos: la reapertura a 100K bajó de
**1237 a 228 ms (×5,4)** eliminando el CRC naive (390→22 ms), la
hidratación de mapas (~250→33 ms, filas planas + decode paralelo + fin de
la copia oculta), el FTS del arranque (218→0, perezoso) y la construcción
de índices (200→87, una pasada + paralelo). Los problemas definidos del
informe 0.6 quedaron atendidos: corrupción del delta **clasificada y
reportada** con paridad motor/Zig, cadenas overflow rotas **dejan de tumbar
la base**, y el KNN midió +22% con heap top-k y abandono temprano. El
ciclo probar→diagnosticar→corregir produjo además cuatro correcciones de
bugs propios (tombstones, heap KNN, schemas supersededos, harness infinito)
— tres de ellas cazadas por las suites de esta misma iteración y una por la
validación cruzada multi-lenguaje, que es exactamente para lo que existe.
