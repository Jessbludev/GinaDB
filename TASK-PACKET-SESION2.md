# TASK-PACKET — Sesión 2: Bloque B6 (grafos y colecciones de grafos)

**Fecha:** 2026-11-06 (America/Bogota)
**Directiva del usuario:** implementar los módulos de
`docs/GINADB-BLOQUES-MULTIMODELO.md` con orden de prioridad, **1 bloque por
sesión**, probando/validando/corrigiendo hasta que quede maduro, empezando
por el que se integra más rápido.
**Autoridad de diseño:** `docs/GINADB-BLOQUES-MULTIMODELO.md` §2-B6, §1.2
(contrato de bloque), §5 (decisión recomendada: *el grafo es una capa sobre
el kernel, no un motor*); `docs/GINADB-PRINCIPIOS-DISENO.md` P1, P3, P5, P8,
P11; skill `immutable-db-scope-guard`.

## Por qué B6 y no otro bloque

Criterio del usuario: el que **se integra rápido**. Evidencia del terreno
(sesión 1 + lectura de `src/`):

- **B6 como capa** necesita solo capacidades ya existentes y verificadas de la
  C ABI: `gina_exec` (tx atómica: el motor valida todas las ops antes de
  escribir WAL, `api.cpp:862–884`), y el pushdown `PkRange` de `make_plan`
  (`api.cpp:89–135`, `lt` exclusivo) que da un **rango exacto por prefijo**
  sobre el B+Tree de la PK. Cero cambios al C++ congelado.
- B1/B3/B4/B8 requieren cirugía del kernel (PK generalizada, índices por ruta,
  postings persistentes, retención de raíces) y B5 está, por decisión del
  propio documento, en "diseño" hasta medir recall/latencia. Ninguno se
  "integra rápido" sin romper la congelación del oráculo.
- B6 es además la decisión recomendada por §5 del documento de bloques y su
  aceptación (traversal profundidad 3, latencia acotada, memoria ∝ frontera)
  es medible hoy.

## Diseño (paso 2 de la skill — 1 párrafo)

Un grafo `<name>` son dos tablas sobre la C ABI (`g.<name>.out`,
`g.<name>.in`) con PK compuesta codificada por *byte-stuffing* del delimitador
`0x1F` (`out: src|tipo|dst`, `in: dst|tipo|src`, la decisión de §2-B6): cada
arista se escribe como **dos filas en una única `gina_exec`** (atómica; si una
op falla, no se escribe nada), la vecindad de un nodo es un **rango exacto
por prefijo** del B+Tree (`gina_select` con `and(ge(P), lt(P'))` sobre la
columna PK, donde `lt` es exclusivo y `P'` = prefijo con el último byte
incrementado, cubriendo exactamente las claves que comparten el prefijo),
`traverse(dirección, tipos, profundidad)` es un BFS en Zig cuya memoria es
proporcional a la frontera (mapas `visited`+`next`, nunca el grafo),
`shortest_path(src, dst, presupuesto)` es el mismo BFS con salida temprana y
tope de expansiones, los props de la arista viven solo en la fila `out` (la
`in` es espejo mínimo de claves), y **no se añade ningún registro WAL nuevo**
(son K_PUT/K_DEL ordinarios), por lo que el lector independiente
(`gina-inspect`) y el oráculo C++ no se tocan; el keyspace físico §1.1
(`space_kind` 5/6) llegará con el kernel Zig (F2–F5) y esta interfaz no
cambiará cuando eso ocurra.

## In-scope

| Archivo | Qué |
|---|---|
| `zig/graph.zig` *(nuevo)* | Capa B6: codec de claves (stuffing 0x1F), `init/create`, `addEdge(s)` atómico, `removeEdge`, `neighbors`, `traverse`, `shortestPath`, `degree`, `getEdge`/`setEdgeProps`, `dropGraph`. Externs propios para las rutas calientes (select/exec/get/count) con búferes reutilizables, para no engordar el arena de `Database`. |
| `zig/graph_test.zig` *(nuevo)* | Suite: round-trip del codec, exactitud del rango por prefijo, atomicidad de la doble entrada, vecindad (out/in/both, filtro por tipos), traversal (profundidad, tipos, límite de frontera, ciclos, supernodo), camino más corto (presupuesto, sin camino, src=dst), props, persistencia tras reinicio. |
| `zig/graph_demo.zig` *(nuevo)* | Demo CLI (grafo social: vecinos → traversal → camino → reinicio) + modo `--bench` (retícula anular de 10⁶ aristas con verificación analítica del BFS, supernodo con tope de frontera, métricas de latencia/memoria). |
| `zig/build.zig` *(edición)* | Targets `graph-test` y `graph-demo` (enlazan `../libginadb.so` con rpath, igual patrón que `ffi-demo`). `zig build test` queda intacto (lector puro). |
| `docs/BLOQUE-B6-CONTRATO.md` *(nuevo)* | Tabla de contrato §1.2 completa del bloque. |
| `TASK-PACKET-SESION2.md`, `REGISTRO-DE-CAMBIOS.md` (entrada 2, append), `NOTA-DE-ALCANCE.md` (sección sesión 2, append) | Gobernanza. |

## Out-of-scope

- **Cualquier cambio en C++**: `src/`, `include/`, `tests/`, `tools/`,
  `Makefile`, `CMakeLists.txt` — el motor 0.7 sigue congelado como oráculo.
- Cambios en `gina.zig`, `ginaread.zig`, `inspect.zig`, `ffi_demo.zig`.
- B7 (joins/planner), estadísticas de grado persistidas, `space_kind`
  físicos 5/6, índices ANN, port de Haskell (sesiones 3–4), B1–B5/B8–B12.
- Modificar los documentos rectores (`docs/GINADB-*.md`).

## Criterios de verificación

1. `zig build test` sigue en verde (lector puro, sin libginadb).
2. `zig build graph-test` en verde contra `libginadb.so` (suite completa).
3. `./zig-out/bin/gina-graph <dir>`: demo con doble entrada atómica,
   vecinos, traversal, camino más corto y persistencia tras reinicio.
4. Bench 10⁶ aristas: traversal de profundidad 3 con **frontera acotada**
   (retícula: 10/nivel, verificación analítica) y latencia medida;
   supernodo contenido por el límite de frontera.
5. `git diff` del árbol C++ contra el baseline de la sesión 1 (`f498458`)
   en `src/ include/ tests/ tools/ Makefile CMakeLists.txt` = vacío.
