# Contrato de bloque — B6: Grafos y colecciones de grafos

Implementado en la **sesión 2** como **capa sobre el kernel congelado** (C ABI
0.7), siguiendo la decisión recomendada en §5 de
`GINADB-BLOQUES-MULTIMODELO.md` ("el grafo es una capa sobre el kernel, no un
motor"). Este documento completa la tabla §1.2 (contrato de bloque) que exige
el documento rector y registra las decisiones reales tomadas.

**Código:** `zig/graph.zig` (capa) · `zig/graph_test.zig` (suite) ·
`zig/graph_demo.zig` (demo + bench). **Oráculo C++ 0.7: sin una línea
modificada.**

---

## Tabla §1.2 — contrato de B6

| Ítem | Decisión implementada |
|---|---|
| **Modelo** | Aristas de primera clase `(src, tipo, dst, props)`. Los nodos NO se duplican: son filas de cualquier tabla B1 (la capa no impone FK — eso llega con B7). Colección de grafos = un par de tablas por grafo (`g.<nombre>.out`, `g.<nombre>.in`). |
| **Keyspaces** | En 0.7: PK compuesta **codificada con byte-stuffing** del delimitador `0x1F` — `out: src|tipo|dst`, `in: dst|tipo|src` (el nodo escaneado va primero y el otro extremo al final, por lo que el vecino siempre es el 3.er componente). En el kernel Zig futuro: `space_kind=5/6` de §1.1 con el MISMO layout lógico; la interfaz pública no cambia. |
| **Registros WAL** | **Ninguno nuevo.** Una arista son dos `K_PUT` (o `K_DEL`) ordinarios dentro de UNA transacción `gina_exec` (el motor valida todas las ops antes de escribir WAL: par atómico). El lector independiente `gina-inspect` y el corpus de P1 quedan intactos. |
| **Operadores** | `neighbors(nodo, dirección, tipos, límite_escaneo)`: range scan **exacto** por prefijo (`and(ge(P), lt(P'))` con `lt` exclusivo y `P'` = prefijo con el último byte incrementado; cero sobre-escaneo, probado contra el motor con probe). `traverse(semillas, dirección, tipos, profundidad, límite_frontera)`: BFS en Zig con dedupe, absorbe ciclos, memoria ∝ frontera. `shortestPath(origen, destino, presupuesto)`: BFS con salida temprana y tope de expansiones. `degree(nodo, dirección)`: conteo perezoso. No añade camino de consulta propio (§3 del rector): se compone con B7 cuando llegue. |
| **Estadísticas** | Por nodo: `degree()` perezoso (escaneo acotado). Histogramas de grado persistidos: **pendientes de B7** (la puerta de entrada del rector). Guardia de supernodos: `scan_limit` por escaneo + `frontier_limit` por nivel, con `truncated` informativo. |
| **Mantenimiento** | Ninguno propio: las aristas son filas, el checkpoint/compactación del kernel las cubre. Sin GC de aristas huérfanas (los nodos borrados dejan aristas colgantes por diseño; integridad referencial = B7). `dropGraph()`: dos DDL, best-effort (documentado). |
| **Formato** | Claves: `<stuff(src)> 0x1F <stuff(tipo)> 0x1F <stuff(dst)> 0x1F` donde `stuff()` duplica cada `0x1F` interno (el delimitador no se puede falsificar; test de no-colisión). Props: columna `props` tipo objeto en la fila `out` (la `in` es espejo mínimo). **Versión del formato del motor intacta** (0.7 v6/v7): sin tags nuevos. |
| **Pruebas** | Suite de 18 tests (codec round-trip, no-falsificación del separador, layout de claves fijado a mano, rango exacto n1≠n10, atomicidad de la doble entrada, direcciones out/in/both, filtro por tipos, traversal con ciclos/límites, camino con presupuesto/inaccesible/src=dst, props, borrado idempotente, grado, persistencia tras reinicio, drop) + demo social + bench 10⁶ aristas con **verificación analítica** del BFS (retícula anular: nivel d = {(d-1)K+1..dK}). |

## Por qué es "capa" y no "motor" (§5 del rector)

Mismo WAL, mismas transacciones, misma recuperación ante crash, y las aristas
se unen a datos relacionales/vectoriales sin exportar nada (cuando B7 llegue).
Las garantías heredadas del kernel se conservan porque el kernel no cambia:
el par de filas escribe/lee con la semántica transaccional existente.

## Límites conocidos de esta implementación (registrados, no bugs)

1. **`degree` y el escaneo de un supernodo cuestan O(grado)**: la vecindad es
   un range scan eficiente, pero leer un millón de aristas de un solo nodo
   cuesta leer un millón de aristas. El guardia operacional es
   `scan_limit`/`frontier_limit`; las estadísticas de grado persistidas son
   parte de B7.
2. **`truncated` por escaneo es heurístico**: se marca si un escaneo devuelve
   exactamente `scan_limit` resultados (un nodo con grado justo igual al
   límite se marca como truncado). Conservador por diseño.
3. **`dropGraph` no es atómico** entre las dos tablas (dos DDL del motor).
4. **Idempotencia**: re-añadir una arista la sobrescribe (upsert de las dos
   filas); re-borrar una arista inexistente no es error. Alineado con la
   semántica upsert del motor 0.7.
5. **NUL (0x00) en ids/tipos se rechaza** con `error.InvalidArgument` (cortaría
   la cadena C silenciosamente — espíritu P8). `0x1F` interno sí está soportado
   vía stuffing.

## Mediciones del banco (sesión 2, criterio §2-B6)

Retícula anular dirigida de 10⁶ aristas (100 000 nodos × fan-out 10) + hub de
20 000 aristas, sobre el motor C++ 0.7 congelado:

| Medida | Resultado |
|---|---|
| Inserción atómica (lotes de 250) | 44.8 s — 22 325 aristas/s |
| Reapertura en frío (2.04 M filas) | 1.44 s |
| **Traversal profundidad 3** | **31 nodos en 1 ms** — frontera exacta 10/10/10, `truncated=false` |
| Camino más corto n0→n40 | 4 saltos (analítico) |
| Supernodo (20 k aristas), tope de frontera 100 | 101 nodos, `truncated=true` |
| Huella en motor | 2 tablas, 2 040 000 filas, 160 MB data, WAL 4.3 MB |

La aceptación de §2-B6 ("traversal de profundidad 3 sobre un grafo de 10⁶
aristas con latencia acotada y memoria proporcional a la frontera, no al
grafo") queda **verificada por construcción analítica**: el BFS toca 31 nodos
y 3 niveles de 10 con un millón de aristas en disco; la latencia no crece con
el tamaño del grafo, solo con la frontera alcanzada.
