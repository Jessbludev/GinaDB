# Benchmarks GinaDB Lite 0.6 vs 0.5

Metodología: misma máquina (2 vCPU, 4 GiB RAM), mismo compilador y flags
(`g++ 14.2 -O2`, C++20), mismo harness de workload (TypeScript, idéntico código
para ambos motores vía `GINA_LIB`). Cada número es una corrida completa del
workload con la semilla fija del generador; el workload corre sobre el C ABI a
través de FFI (bun:ffi para 0.6/0.5 en Bun, koffi en Node), no hay código
especial por versión — la única diferencia es la librería compartida.

Reproducibilidad:

```
make libginadb.so                      # lib 0.6
cd examples/typescript
GINA_LIB=../../libginadb.so bun src/workload.ts 20000 5000 5000
GINA_LIB=/ruta/a/libginadb-0.5.so bun src/workload.ts 20000 5000 5000
```

## 1. Workload 20.000 usuarios + 5.000 docs (dim 64) — Bun 1.3.14

| Fase | 0.5 | 0.6 | Diferencia |
|---|---:|---:|---|
| Carga users (tx de 500) | 45.416 ops/s | 46.201 ops/s | = |
| Carga docs + embeddings | 25.512 ops/s | 27.013 ops/s | +6% |
| `create_index` sobre tabla llena | — | 731.080 rows/s | nuevo |
| `get` puntual por PK (p50) | 138.200 ops/s (6 µs) | 136.324 ops/s (6 µs) | = |
| **`select` eq email (índice)** | **10 ops/s** (p50 104.7 ms) | **76.382 ops/s** (p50 10 µs) | **×7.638** |
| **`select` rango age + order desc** | 9 ops/s | 1.647 ops/s (p50 597 µs) | **×183** |
| **Rango PK inverso + limit 10** | 9 ops/s | 29.923 ops/s (p50 31 µs) | **×3.325** |
| FTS bio (AND 2 términos) | 720 ops/s | 809 ops/s | +12% |
| KNN k=10 sobre 5.000×64 | 668 ops/s | 826 ops/s | +24% |
| CRUD mixto (1 op/tx) | 74.488 ops/s (p50 14 µs) | 40.011 ops/s (p50 21 µs) | ver nota 1 |
| CRUD mixto **sin índices** (0.6) | — | 67.440 ops/s (p50 14 µs) | ver nota 1 |
| Watch: 1.000 eventos | 34.784 ops/s | 22.398 ops/s | ver nota 1 |
| Checkpoint (500 dirty tras base) | 121–130 ms (siempre completo) | **4,5 ms** (incremental) | **×27** |
| Reabrir (base+delta+WAL) | 185 ms | 234 ms | +26% (nota 2) |

**Nota 1 — el precio de los índices en la escritura:** con los 3 índices
secundarios (email, age, city) cada PUT/UPDATE mantiene los árboles: ~7 µs/op
medidos (67,4K → 40K ops/s). Es **opt-in por columna**: sin índices, el costo
de escritura de 0.6 es el de 0.5 (14 µs p50). El intercambio: ×7.638 en
lecturas indexadas.

**Nota 2 — reapertura más cara:** 0.6 reconstruye los índices secundarios al
abrir (223 ms de los 234 ms extra a escala 100K). Los datos y el árbol PK no
cuestan más (bulk load).

**Por qué eq email pasa de 10 a 76.382 ops/s:** en 0.5 no existe `gina_select`
ni índices secundarios; el camino real de un cliente es `gina_all()` (JSON de
TODA la tabla por FFI) + filtro en el lenguaje huesped: ~105 ms por consulta a
20K filas. En 0.6 el planner resuelve eq/between/ge/le/in por el B+Tree de la
columna y devuelve solo las filas pedidas.

## 2. Escala 100.000 usuarios + 10.000 docs — checkpoints y recuperación

| Operación | 0.5 | 0.6 |
|---|---:|---:|
| Checkpoint con TODO dirty (primera vez) | 520 ms | 932 ms (nota 3) |
| Checkpoint con 500 filas dirty | **520–548 ms** (reescribe todo) | **9,2 ms** (**×60**) |
| Compactación explícita (FC) | — | 618 ms |
| Reabrir (base 28,5 MB + delta + WAL) | 836 ms | 1.177 ms |
| Archivo tras checkpoint | data.pages 28,5 MB | delta 131 KB |

**Nota 3:** el primer IC de 0.6 es más caro que un checkpoint completo de 0.5
(copia de filas al lote + codificación delta + estado derivado). El caso que
importa en producción es el de estado estable (dirty pequeño entre
checkpoints), donde 0.6 paga 9 ms por lo que 0.5 paga 520 ms — y la ventaja
**crece con el tamaño de la base** (0.6 paga O(dirty), 0.5 paga O(base)).

## 3. Crash real (SIGKILL a proceso durable, 3 rondas)

| Motor | Commits sobrevividos | Prefijo contiguo | Recuperación |
|---|---:|---|---:|
| 0.5 | 120.376 | SÍ | 204–307 ms |
| 0.6 | 111.554 | SÍ | 194–358 ms |

Ambos motores garantizan: ningún commit completado se pierde, ninguna
transacción a medias se aplica (verificación de prefijo k000000..kN-1 y
ausencia estricta tras la frontera). La durabilidad es del WAL (fsync por
commit), no del checkpoint.

## 4. Paridad de runtimes (mismo workload, 0.6)

| Fase | Bun 1.3.14 | Node 24 (koffi) |
|---|---:|---:|
| Carga users | 46.201 ops/s | 39.520 ops/s |
| get PK p50 | 6 µs | 7 µs |
| select eq (índice) p50 | 10 µs | 11 µs |
| CRUD mixto | 40.011 ops/s | 39.765 ops/s |

El cuello de botella es el motor/JSON, no el FFI: ambos runtimes quedan
dentro del ±15%. Mismo código TypeScript, cero cambios.

## 5. Motor C++ puro (referencia, sin FFI/JSON)

- `get` por PK @100K filas: ~1,77M ops/s (0.5) vs ~1,5M ops/s (0.6, B+Tree
  orden 64). El hash puro gana el punto exacto; el B+Tree aporta orden,
  rangos y checkpoints sin ordenar (ver INFORME-0.6).
- `order_by(pk)` @100K filas: 0.5 ordena (~10 ms por consulta); 0.6 recorre
  las hojas ya ordenadas (~1 ms, sin alocar).
- Checkpoint @100K filas: ver sección 2.

## 6. Lectura honesta

- 0.6 no es más rápido en TODO: escritura con índices (+7 µs/op), primer
  checkpoint de una base nueva, y reapertura pagan un costo real, medido y
  documentado arriba.
- Las ganancias estructurales (consultas indexadas ×10²–10⁴, checkpoint
  incremental ×27–×60 creciente con el tamaño) son el objetivo de la versión;
  los costes son acotados y configurables (los índices son opt-in por columna,
  el umbral de compactación es una opción).

---

# GinaDB Lite 0.7

Entorno de medición 0.7: misma caja que 0.6 (contenedor 2 vCPU, 4 GiB RAM),
g++ 14.2 `-O2`, Bun 1.3.14, Node 24.21.0. Los números 0.6 de esta sección
fueron **re-mediciones de esta misma caja** (no los del informe 0.6, que
corrieron en otra) para que la comparación sea limpia.

## 7. Reapertura — el objetivo 0.7 (banco focalizado `make bench-reopen`)

100K usuarios + 10K documentos (embeddings 64d), base 28,5 MB, 4 índices
secundarios, FTS en bio/body:

| | 0.6 | 0.7 | ganancia |
|---|---:|---:|---:|
| Reapertura tras FC | **1237 ms** | **228 ms** | **×5,4** |
| Reapertura con delta (500 updates + IC) | 1364 ms | 312 ms | ×4,4 |

Desglose 0.6 → 0.7 de la reapertura tras FC (GINA_TRACE):

| Fase | 0.6 | 0.7 | qué cambió |
|---|---:|---:|---|
| lectura + CRC de páginas | ~390 | 22 | CRC32 slice-by-8 + lectura masiva |
| decode de filas | ~250 | ~33 | filas planas + decode paralelo + fin de la copia oculta del mapa |
| bulk_load B+Tree | 7 | 9 | igual (ya era óptimo) |
| índices secundarios | ~200 | 87 | una pasada + sort + bulk por índice en paralelo |
| FTS | ~120 (interleaved) | **0** | perezoso: lo paga la primera búsqueda |

La ganancia crece con el tamaño del base (todo lo eliminado era O(base)).

## 8. Workload real 0.6 vs 0.7 (mismo harness, Bun, 5K users + 2K docs)

| Fase | 0.6 | 0.7 | Δ |
|---|---:|---:|---:|
| carga users (lotes 500) | 48.971 ops/s | 63.403 ops/s | +29% |
| carga docs + embeddings | 27.596 | 31.918 | +16% |
| get puntual PK (p50) | 7 µs | 6 µs | ≈ |
| select eq email (índice, p50) | 10 µs | 9 µs | +10% |
| rango age + order desc (p50) | 532 µs | 500 µs | +6% |
| rango PK inverso + limit 10 (p50) | 27 µs | 23 µs | +16% |
| FTS bio AND-2 | 2.345 ops/s | 2.331 ops/s | ≈ (lazy: construcción fuera del arranque) |
| KNN k=10 @2K×64 (p50) | 321 µs | 265 µs | **+22%** |
| CRUD mixto (p50) | 16 µs | 13 µs | +11% |
| watch eventos/s | 36.277 | 41.037 | +13% |
| IC con todo dirty | 65 ms | 29 ms | ×2,2 |
| compactación (FC) | 47 ms | 15 ms | ×3,1 |
| segundo IC (dirty pequeño) | 3,7 ms | 1,9 ms | ×1,9 |
| reabrir base+delta+WAL | 84 ms | 33 ms | ×2,5 |
| FC final | 35 ms | 11 ms | ×3,2 |
| recuperación tras SIGKILL (3 rondas) | 193–288 ms | 136–234 ms | ~×1,25 |
| RSS durante el workload | 149 MB | 150 MB | ≈ |

Node 24 + koffi contra 0.7 (misma paridad que 0.6): eq p50 9 µs, KNN p50
99 µs @1K docs, reapertura 13,4 ms @4,5K filas, crash prefijo-contiguo SÍ.

## 9. Costes residuales medidos (0.7)

| Coste | Medida | Nota |
|---|---|---|
| delta-apply, updates aleatorios con índice de baja cardinalidad | 106 ms / 500 updates | mantenimiento de índices (vector de ~14K PKs: ~0,5 MB de shift por op) — el MISMO coste del commit normal; estaba oculto en 0.6, ahora aislado |
| primera búsqueda FTS tras abrir | ~218 ms @100K | construcción perezosa única |
| colecta de sec_build | ~27 ms @100K×3 índices | hash de columna por fila e índice |
| KNN | fuerza bruta mejorada (heap + abandono L2) | no escala a millones: HNSW/IVF en roadmap |

## 10. Lectura honesta (0.7)

- Las ganancias de reapertura son estructurales (O(base) → O(páginas CRC +
  decode paralelo + índices)); a bases pequeñas la mejora es menor porque
  el término constante (abrir archivos, schemas) pesa más.
- El delta-apply con updates aleatorios NO mejoró: es coste de
  mantenimiento de índices, no de recovery — está medido y es el ítem 1
  del roadmap (postings O(log n)).
- El FTS no se hizo más rápido: se hizo **diferido**. Las cargas que
  buscan texto inmediatamente tras abrir pagan la construcción una vez.
- Formato en disco sin cambios: 0.7 abre 0.5/0.6. El lector Zig no cambió
  su parser de filas (solo aprendió la clasificación corrupt/suspect).
