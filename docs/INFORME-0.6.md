# Informe de ingeniería — GinaDB Lite 0.6

**Objetivo de la iteración** (pedido del usuario): implementar el roadmap 0.6
(free-list persistente + índice primario B+Tree + índices secundarios), con
bindings en **TypeScript** (no Go), **pruebas reales** en entorno Bun/Node —
instalándolos si hacía falta —, **operaciones de datos que sometan la base al
uso para el que fue diseñada** (no solo tests), verificación desde **otros
lenguajes**, y con lo recopilado: **identificar mejoras, optimizar, aplicar y
reportar**.

Este informe documenta el ciclo completo: qué se construyó, qué se midió, qué
cuellos de botella encontraron las mediciones, qué se optimizó (con números
antes/después) y qué queda para la próxima iteración.

---

## 1. Entorno y metodología

| Elemento | Detalle |
|---|---|
| Máquina | contenedor 2 vCPU, 4 GiB RAM, disco local |
| Motor | g++ 14.2, C++20, `-O2 -Wall -Wextra -pthread` |
| Runtimes de prueba | **Bun 1.3.14** (bun:ffi nativo) y **Node 24.21.0** (koffi 3.3.2 vía npm) — ambos ya instalados en el entorno |
| Otros lenguajes | Python 3.12 (ctypes), Zig 0.16.0 (FFI + lector binario puro), C (C ABI) |
| Comparativa | misma lib 0.5 (compilada del tarball 0.5, mismos flags) corriendo EL MISMO harness con `GINA_LIB` — fallback a `all()+filtro` del cliente para las consultas que 0.5 no tiene |

El harness (`examples/typescript/src/workload.ts`) es una sola base de código
TypeScript que corre en ambos runtimes y contra ambos motores; ninguna fase
tiene código especial por versión. Los datos (usuarios con email/edad/ciudad/
tags/bio, documentos con embeddings de 64 dimensiones) se generan con PRNG de
semilla fija: cada corrida es reproducible.

## 2. Qué se construyó

### 2.1 Índice primario B+Tree (`src/btree.hpp`)

Árbol B+ en memoria de orden 64 con hojas enlazadas (recorrido ordenado
directo e inverso), `bulk_load` desde datos ordenados, rangos con parada
temprana y pool de valores con referencias estables (`std::deque`). Sustituye
al `unordered_map` como almacén de filas.

**Por qué importa:** `all()` y `order_by(pk)` salen ya ordenados (0.5 ordenaba
cada consulta y cada checkpoint), los rangos de PK dejan de ser recorrido
completo, y el checkpoint completo escribe directamente del recorrido de
hojas. El borrado (borrow/merge con refresco de separadores) se validó con un
test **diferencial contra `std::map`** (3 semillas × 60.000 operaciones +
50.000 secuenciales + carga masiva + rangos), que además detectó un bug en mi
propio validador de invariantes antes de integrar nada.

### 2.2 Índices secundarios + planner

- Codificación ordenable de valores (`src/idxkey.hpp`): int64 con bit de
  signo invertido, float con el truco IEEE totalOrder (negativos incluidos),
  texto, bytes y bool; null/array/object/vector quedan fuera del índice
  (semántica SQL de NULL).
- Un B+Tree por columna indexada: clave codificada → lista ordenada de PKs.
  Mantenimiento incremental en cada escritura; `create_index`/`drop_index`
  son operaciones WAL atómicas (kind RESCH) que persisten en el schema
  (sección v6, trailing y por tanto compatible con payloads v5).
- Planner (`src/api.cpp`): condiciones tipadas `eq/ne/gt/ge/lt/le/between/in/
  and/or` con selección de vía de acceso — punto PK > rango PK > punto índice
  > rango índice > recorrido — y `order_by` resuelto por el orden del árbol
  con parada temprana en `limit`. `where(lambda)` se conserva para filtros
  libres.

### 2.3 Checkpoint incremental base+delta (`src/delta.{hpp,cpp}`)

El problema de 0.5: cada checkpoint reescribe TODA la base (O(base) siempre;
520 ms a 100K filas, y crece con la base).

Diseño 0.6: `data.pages` sigue siendo el **base** compacto (reescrito solo en
compactación) y `delta.pages` acumula las **versiones de los registros
cambiados** desde el último base:

- Registros versionados (fila / tombstone / schema / drop de tabla) con el
  watermark del checkpoint incremental (IC) que los escribió.
- Encabezado de **doble slot**: el flip atómico (pwrite+fsync) ES el commit;
  las páginas de un IC que no llegó a voltearse se ignoran al abrir.
- **Free-list persistente derivada**: una página está libre si no contiene
  registros vivos (versión supersada o tabla borrada) y no es cadena
  referenciada. No se persiste como estado redundante — se deriva del
  contenido al abrir — de modo que es infalsificable y sus huecos se reutilizan
  (menor id primero). Esta es la interpretación de "free-list persistente"
  elegida deliberadamente: un page-manager en línea con estado redundante
  habría sido un motor nuevo, no una iteración lite.
- **Copy-forward**: página ≥50% muerta → reemisión de los vivos y liberación
  (incluidas cadenas de overflow huérfanas).
- **Compactación** (FC): manual con `db.compact()` y automática cuando el
  delta vivo supera `max(base/4, 64)` páginas. El WAL se rebasea tras cada
  checkpoint, así que el replay post-crash es corto.
- Recovery: base → delta (solo si `watermark > checkpoint_lsn` del base — un
  delta obsoleto de un FC interrumpido se descarta) → WAL.

### 2.4 Interoperabilidad

- C ABI: `gina_select` (JSON con where/order/limit/fields y planner),
  `gina_create_index`, `gina_drop_index`, `gina_compact` (24 → 28 funciones).
- **Cliente TypeScript** tipado con backend dual automático: **bun:ffi** (cero
  dependencias) o **koffi** (Node). Códec completo de valores (bytes en b64,
  vectores), watch con callbacks en JS, errores tipados.
- Python extendido (select/índices/compact) + script de verificación con
  crash real.
- El lector Zig aprende el formato v6 y parsea `delta.pages` completo:
  validación cruzada **byte a byte** contra el motor (misma BD, mismo estado,
  mismas filas) sin usar una sola línea del motor C++.

## 3. Mediciones (los números crudos están en `docs/BENCHMARKS.md`)

### 3.1 Consultas — donde 0.6 cambia la categoría

@20K usuarios, mismo harness, p50 por consulta:

| Consulta | 0.5 (camino real del cliente) | 0.6 | ganancia |
|---|---:|---:|---:|
| eq por email | 104,7 ms | 10 µs | **×7.638** |
| rango age + order desc + limit | ~110 ms | 597 µs | ×183 |
| rango PK inverso + limit 10 | ~110 ms | 31 µs | ×3.325 |
| get puntual por PK | 6 µs | 6 µs | = |

El número de 0.5 no es una paja de paja: sin `gina_select` ni índices, un
cliente debe hacer `gina_all()` (JSON de toda la tabla por FFI) y filtrar en
el lenguaje huesped. Es exactamente lo que mide el harness (fallback
`all+filter`), y es el uso real documentado de 0.5.

### 3.2 Checkpoints — el objetivo del delta

@100K usuarios + 10K docs (base 28,5 MB):

| Operación | 0.5 | 0.6 |
|---|---:|---:|
| Checkpoint con 500 filas dirty | **520–548 ms** (reescribe todo) | **9,2 ms** — **×60** |
| Checkpoint con todo dirty (primera vez) | 520 ms | 932 ms |
| Compactación explícita | — | 618 ms |

La ganancia **crece con el tamaño de la base** porque 0.6 paga O(dirty) y 0.5
paga O(base) siempre. El caso "todo dirty" del arranque es más caro en 0.6
(copia al lote + codificación delta + estado derivado): es el peaje del
formato, pagado una vez.

### 3.3 Durabilidad bajo crash real

Tres rondas de SIGKILL a un proceso que escribía con `durable=1` (fsync por
commit), por runtime:

- 0.5: 120.376 commits sobrevivientes, **prefijo contiguo SÍ**, recuperación
  204–307 ms.
- 0.6: 111.554 commits, **prefijo contiguo SÍ**, 194–358 ms.

La verificación comprueba que los commits presentes son exactamente
`k000000..kN-1` (ningún hueco, ninguna transacción a medias) y que tras la
frontera no hay nada. Ambos motores cumplen; la garantía es del WAL, no del
checkpoint.

### 3.4 Costes medidos (no todo son ganancias)

| Coste | Medida | Causa |
|---|---|---|
| Escritura con 3 índices | 14 → 21 µs/op (67,4K → 40K ops/s) | mantenimiento de los árboles; **opt-in por columna** (sin índices, 0.6 ≈ 0.5 en escritura) |
| Reapertura @100K | 836 → 1.177 ms | reconstruir índices (223 ms) + decodificar filas `std::map` (850 ms, dominante) |
| Primer IC de una base nueva | 520 → 932 ms | copia de filas al lote + doble codificación |

## 4. Cuellos de botella encontrados por las mediciones → optimizaciones aplicadas

El ciclo que pidió el usuario funcionó literalmente: el workload encontró dos
problemas graves de 0.6 y ambos se corrigieron en la iteración.

### 4.1 Los índices no existían (bug encontrado por medición)

**Síntoma:** primera corrida del workload: `select eq email` = 1,9 ms,
`rango age` = 33 ms por consulta. Las fases con índice eran apenas mejores
que el fallback de 0.5.

**Diagnóstico:** `apply_op(K_SCHEMA)` creaba la tabla con `.indexes` en el
def pero **nunca construía las estructuras** (`t->sec` vacío) — el planner no
encontraba el índice y degradaba a recorrido completo. Los tests unitarios no
lo vieron porque probaban `create_index()` (que sí construía) y no el
`.indexes` declarativo del `create()`.

**Corrección:** `sec_build` al crear la tabla. **Antes/después:** eq 1,9 ms →
**10 µs** (×190). Lección: la cobertura de tests debe incluir cada vía de
entrada de una feature (declarativa y dinámica); el workload con datos reales
sí la cubrió.

### 4.2 `order_by` ignoraba el orden del índice

**Síntoma:** rango age + `order_by("age", desc)` + `limit 100`: 16 ms.
El índice ya entrega los datos EN orden, pero el planner materializaba las
~3.300 filas del rango y las ordenaba.

**Corrección:** cuando el `order_by` es sobre la misma columna indexada del
rango, recorrer el árbol en orden (directo o inverso) con parada temprana en
`limit`. **Antes/después:** 16 ms → **597 µs** (×27). La consulta se volvió
O(límite).

### 4.3 Reapertura: bulk-load del base

**Síntoma:** reapertura @100K = 1.212 ms, de los que el B+Tree construía por
inserciones individuales.

**Corrección:** el base llega ordenado por PK → `bulk_load` (construcción
bottom-up). La parte del árbol pasó a **7 ms**. El resto del tiempo es
decodificación de filas (`std::map` por fila: ~850 ms) y reconstrucción de
índices (223 ms) — ver trabajo futuro.

### 4.4 Bugs del cliente TypeScript/Node (encontrados ejecutando de verdad)

- `koffi.decode(ptr, "char *")` segfaultea (la firma v3 es
  `koffi.decode.string(ptr)`); descubierto aislando paso a paso.
- Bun moderno expone `JSCallback` (no `Callback`) y hay que pasar `.ptr` al
  parámetro puntero.
- `gina_version`/`gina_last_error` devuelven memoria interna: liberarlas con
  `gina_free` era UB (`munmap_chunk: invalid pointer`).
- El test de crash colgaba esperando el `exit` de un hijo que ya había
  terminado (los inserts durables eran más rápidos que el delay del kill).

Ninguno de estos aparece en un test unitario del motor: aparecen al usar la
base desde el lenguaje objetivo, que es exactamente lo que se pidió probar.

## 5. Verificación multi-lenguaje (resumen; detalle en `VERIFICACION.md`)

| Lenguaje | Mecanismo | Cobertura |
|---|---|---|
| C++ | nativo | 228 comprobaciones (btree diferencial + 177 unit + 51 crash con 5 escenarios de delta y fuzz de 4.000 ops) |
| TypeScript | bun:ffi / koffi | 11 tests funcionales + workload completo de 13 fases en **ambos runtimes** |
| Python | ctypes | 24 comprobaciones incl. crash SIGKILL real con verificación de prefijo |
| Zig | lector binario puro + FFI | parseo de `delta.pages` y **validación cruzada byte a byte** contra el motor (12=12 filas, u3=99 idéntico, u4=nulo, tabla borrada eliminada, live pages 2=2) |
| C | C ABI | la CLI `gina` y el smoke test usan exclusivamente `cabi.h` |

## 6. Limitaciones conocidas (y por qué)

1. **Corrupción de medio en el delta**: una página comprometida dañada se
   cuenta (`delta_suspect_pages`) y se trata como hueco, pero sus registros
   se pierden sin error. Es la misma clase de limitación que las páginas
   overflow sin CRC de 0.5. Mitigación futura: checksum global del delta.
2. **Reapertura**: dominada por la decodificación de filas `std::map`
   (~850 ms @100K). Estructural: el modelo de filas como mapa de strings es
   cómodo y type-safe pero caro de hidratar. Futuro: filas planas/columnares.
3. **KNN por fuerza bruta**: correcto y medido (826 q/s @5K×64), pero no
   escala a millones — HNSW o IVF siguen en el roadmap.
4. **Un proceso por base** (por diseño lite, como 0.4/0.5).
5. **0.5 no abre bases 0.6** (el formato es forward-compatible, no
   backward).

## 7. Trabajo futuro priorizado (por impacto medido)

1. **Filas planas/columnares** para atacar los 850 ms de decodificación en
   reapertura (el mayor coste residual medido).
2. **Índices secundarios con construcción perezosa o paralela** al abrir (los
   223 ms restantes de la reapertura).
3. **Checksum global / paridad del delta** para detectar corrupción de medio
   en páginas comprometidas.
4. **HNSW/IVF** para KNN >100K vectores.
5. **Compactación incremental del base** (reescribir solo páginas sucias del
   base en vez de todo) para bases muy grandes con FC frecuentes.

## 8. Conclusión

Los tres objetivos del roadmap 0.6 están implementados y verificados: B+Tree
primario (diferencialmente testeado), índices secundarios persistentes con
planner (×7.638 en consultas indexadas frente al camino real de 0.5), y
free-list persistente en el marco del checkpoint incremental base+delta
(×60 en checkpoints de estado estable, creciendo con el tamaño de la base).
La base fue sometida a operaciones reales desde Bun y Node con el mismo
código, verificada desde Python, Zig y C, y el ciclo medición→diagnóstico→
optimización produjo dos correcciones mayores (índices inexistentes, order_by
sin usar el índice) más un puñado de bugs de integración que ningún test
unitario habría encontrado. Los costes residuales están medidos, documentados
y convertidos en el roadmap priorizado de arriba.
