# Verificación GinaDB Lite 0.7

Cómo se verificó esta versión: todo lo listado abajo se ejecutó contra el
árbol final empaquetado. Filosofía de la iteración: **medir antes de
construir, someter la base a su carga real desde varios lenguajes, y usar
las mediciones y las validaciones cruzadas para encontrar y arreglar
cuellos de botella y bugs** — no solo tests unitarios.

## 1. Suites automáticas

| Suite | Comprobaciones | Resultado |
|---|---:|---|
| `tests/btree_test` (diferencial vs `std::map` + recorrido mutable nuevo) | ~10⁶ internas | **OK** |
| `tests/unit_test` (177 de 0.6 sin cambios: CRUD, schemas, tx, blobs, KNN, FTS, watch, concurrencia, JSON, migración 0.4, índices/planner) | 177 | **0 fallos** |
| `tests/crash_test` (15 escenarios 0.4/0.5/0.6 + **3 nuevos 0.7**: corrupción de página comprometida del delta, cadena overflow rota, schema supersededo + **fuzz reforzado con 5000 claves** para claves frías) | **75** | **0 fallos** |
| `examples/typescript` (bun test: CRUD, índices, select, tx, bytes, KNN, FTS, watch, persistencia) | 11 | **0 fallos** |
| `examples/python/verify06.py` (24 checks + crash real SIGKILL) | 24 | **0 fallos** |
| `zig build test` (parser: valores, schemas v6, WAL v4/v5/v6, delta + clasificación corrupt/suspect y cadenas rotas) | parser | **OK** |
| `make crc-check` (CRC slice-by-8 idéntico al bit-a-bit: 17 longitudes + patrones) | 18 | **OK** |

```bash
make test                                   # btree + unit + crash
make crc-check                              # equivalencia del CRC nuevo
cd examples/typescript && bun test
cd examples/python && python3 verify06.py
cd zig && zig build test                    # (zig 0.16.0)
```

## 2. Reapertura — banco focalizado (`make bench-reopen`)

100K usuarios + 10K docs (64d), 4 índices, FTS: construcción → IC todo-dirty
→ FC → **reapertura 228 ms** (0.6 en la misma caja: 1237 ms) → 500 updates
+ IC pequeño → **reapertura 312 ms** (0.6: 1364 ms) → verificación funcional
(eq por índice, rango order desc, FTS, KNN, get de fila del delta). El
desglose por fases (`-DGINA_TRACE`) está en `docs/BENCHMARKS.md` §7.

## 3. Workload real multi-runtime

`examples/typescript/src/workload.ts` — 13 fases + 3 rondas de SIGKILL.
**Bug del harness 0.6 corregido**: tres bucles infinitos
(`skipQueries ? 0 : 500` como condición) impedían completar una corrida
full; tras corregirlos, el workload corrió **completo** contra la lib 0.6
(re-mediciones de esta caja, para la comparación) y contra 0.7:

- Bun 1.3.14 @5K+2K: 0.6 y 0.7 completos, 13/13 fases + crash prefijo
  contiguo SÍ en 3/3 rondas (ambos motores). Resultados lado a lado en
  `docs/BENCHMARKS.md` §8.
- Node 24 + koffi @3K+1K contra 0.7: completo, reapertura 13,4 ms @4,5K,
  crash prefijo-contiguo SÍ.
- Python: crash real SIGKILL, 34.126 commits sobrevivientes, prefijo
  contiguo, recuperación 29,5 ms.

## 4. Validación cruzada multi-lenguaje (el corazón de esta iteración)

- **Lector binario Zig vs motor C++** (`make xval`): BD con delta que mezcla
  updates + deletes + tabla creada y borrada + re-schema + blob overflow:
  **12 filas = 12 filas**, u3.age=99, u4 ausente, tabla temporal eliminada,
  3 índices, blob de 9000 B byte a byte. Esta validación **cazó el bug más
  grave de la iteración** (el delta-apply saltaba schemas supersededos que
  eran la creación de la tabla: 12→2 filas en el motor).
- **Paridad de la clasificación de corrupción**: misma BD con una página
  comprometida del delta corrompida deliberadamente → motor:
  `users=2, corrupt=1, broken_ovf=0`; lector Zig independiente:
  `rows=2, corrupt=1, broken_ovf=0`. Idéntico.
- **FFI Zig** (`zig build ffi-demo`) contra 0.7: select con índice, rango,
  compactación y stats nuevos OK.
- **CLI C ABI**: create/select (eq por índice y between+order)/stats
  (contadores nuevos)/compact/count OK.

## 5. Compatibilidad

- **Formato sin cambios**: 0.7 abre bases 0.5 y 0.6 (frames de fila v6 con
  nombres por fila; solo cambia la representación interna).
- Los clientes no cambian: mismo C ABI (28 funciones), mismo cliente TS,
  mismo cliente Python. `Stats` gana dos campos aditivos
  (`delta_corrupt_pages`, `delta_broken_ovf_records`).
- Verificación desde el tarball empaquetado: extracción → `make` → `make
  test` (177+75) → `bun test` (11/11) → `make bench-reopen` → `zig build`.

## 6. Bugs encontrados y corregidos POR las verificaciones de esta iteración

1. Tombstones del IC escritos con layout L_ROW (los borrados resucitaban) —
   crash suite, escenarios de replay sobre delta.
2. Comparador del heap KNN invertido — unit test (`cos[1] != "c"`).
3. Delta-apply saltaba schemas supersededos → pérdida silenciosa de filas
   de ICs anteriores — validación cruzada con el lector Zig; red: escenario
   determinista + fuzz con claves frías.
4. Tres bucles infinitos en el harness workload.ts 0.6 — corrida real.
