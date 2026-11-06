# GinaDB — Bloques de diseño multimodelo e híbrido

Continuación de `GINADB-PRINCIPIOS-DISENO.md` (P1–P12). Este documento fija
la arquitectura que permite integrar cada capacidad futura **como un bloque
con su propio diseño y modelo**, sin convertirlas en cinco motores distintos.

> Premisa: los bloques se construyen **después** de que el core y la
> migración a Zig 0.17.0 estén estables (cierre de F5). Aquí se decide la
> forma, no el calendario.

---

## 1. Decisión central: un kernel, muchos modelos

**Un solo kernel de almacenamiento transaccional; todo modelo de datos es una
capa (layer) sobre él.** Filas, documentos, vectores, texto, grafos y
versiones temporales son *keyspaces* con su propio codec, sus índices y sus
operadores — no motores paralelos.

```
 ┌───────────────────────────────────────────────────────────────┐
 │  C ABI estable (P11)  ·  clientes Python / TS / Zig / otros    │
 ├───────────────────────────────────────────────────────────────┤
 │  Lenguaje de consulta  →  Planner (costos)  →  Executor único  │
 │  Operadores: Scan·IndexScan·Filter·Project·Join·Sort/TopK·     │
 │              KNN·FTS·Traverse·AsOf                             │
 ├───────────┬──────────┬──────────┬────────┬─────────┬──────────┤
 │ Tipado/   │ Documen- │ Vectores │ Texto  │ Grafos  │ Temporal │  ← BLOQUES
 │ relacional│ tal      │          │ (FTS)  │         │          │    (modelos)
 ├───────────┴──────────┴──────────┴────────┴─────────┴──────────┤
 │  Servicios transversales: catálogo · estadísticas · extensiones│
 │  (codecs/funciones) · usuarios/permisos · réplica              │
 ├───────────────────────────────────────────────────────────────┤
 │  KERNEL: B+Tree COW sobre páginas · buffer pool · WAL ·        │
 │  snapshots (P2) · checkpoint (P7) · IO inyectable (P10)        │
 └───────────────────────────────────────────────────────────────┘
```

**Por qué.** Hoy el 0.7 ya hace esto a medias (filas, índices secundarios,
FTS, KNN y schemaless conviven sobre el mismo WAL y el mismo B+Tree). Un
kernel único da: una sola semántica de transacción para *todos* los modelos,
una sola recuperación ante crash, y joins entre modelos sin salir del motor.

### 1.1 Disposición de claves (propuesta)

```
key   := ns(u32) | space_kind(u8) | space_id(u32) | payload…
space_kind :
  0 catálogo        4 vector / grafo ANN
  1 filas (PK)      5 arista saliente
  2 índice secund.  6 arista entrante
  3 postings FTS    7 historial de versiones
```

Reglas: orden lexicográfico = orden lógico (codificación ordenable, ya
existe en `idxkey`); un `ns` = una base lógica / colección de grafos /
inquilino; `space_kind` desconocido se trata como dato opaco (P8, enums no
exhaustivos), de modo que un motor viejo no destruye lo que no entiende.

### 1.2 Contrato de bloque

Todo bloque **debe** declarar, antes de implementarse:

| Ítem | Qué define |
|---|---|
| Modelo | Tipos de dato y semántica de consulta |
| Keyspaces | Qué `space_kind` usa y el layout de sus claves |
| Registros WAL | Nuevos `kind` (no exhaustivos, versionados — P1, P8) |
| Operadores | Qué expone al planner y con qué coste estimado |
| Estadísticas | Qué cardinalidades/histogramas publica |
| Mantenimiento | Compactación/GC propia, y cómo corre fuera del camino del escritor (P7) |
| Formato | Versión y lector independiente (P1) |
| Pruebas | Diferencial, fuzz, crash por punto de corte (P10), benchmark |

Un bloque que no puede llenar esta tabla no está listo para implementarse.

---

## 2. Bloques

### B1 — Funcional, tipado y compilado *(en proceso)*
**Es el núcleo del modelo relacional.** Schemas estrictos, PK, índices
secundarios, tx. Con la migración pasa a ser la capa de referencia que los
demás bloques imitan.
- **Modelo.** Tabla = `schema_id` + columnas ordinales (formato v7, P9); PK
  generalizada (no solo texto: entero, compuesta, bytes).
- **Tipado.** En Zig, los *accessors* tipados se pueden derivar en `comptime`
  desde la definición del schema (reflexión; ojo al cambio a
  struct-of-arrays en 0.17.0). El camino dinámico (JSON / C ABI) sigue
  disponible.
- **Aceptación.** Suites unit/crash del 0.7 portadas al 100 %; torn reads = 0.

### B2 — Almacenamiento en memoria y en disco *(decisión abierta → recomendada)*
**Decisión recomendada: un solo buffer pool; la "memoria" es política, no un
motor aparte.**
- Modo por tabla: `memory` (páginas *pinned* en el pool, durables vía WAL +
  snapshot, como hoy), `disk` (paginado con evicción) o `tiered` (caliente en
  pool, frío en disco).
- Un único B+Tree sobre `page_id` (el plan 1.0 del documento de arquitectura).
- Cierra P6: el tope de memoria pasa a ser el tamaño del pool, no el tamaño
  de la base.
- **Alternativa descartada:** mantener un B+Tree en memoria + otro en disco
  (duplica recuperación, checkpoints y bugs).
- **Aceptación.** Base 10× mayor que el pool opera sin OOM; tablas `memory`
  mantienen la latencia del 0.7; reapertura ≤ línea base.

### B3 — Documental (colecciones y objetos) *(diseño; implementación futura)*
- **Modelo.** Colección = tabla `schemaless`; documento = `Object` anidado
  (ya existe en `Value`). Añadir: direccionamiento por ruta (`a.b[2].c`),
  *patch* por ruta, e **índices sobre rutas** (secundarios sobre
  `space_kind=2` con la ruta como parte de la clave).
- **Tipado gradual.** Una colección puede "endurecerse": se infiere un schema
  de los documentos existentes y se promueve a tabla tipada (B1) sin
  reescribir datos (migración por nombre, como el `srow_rewidth` del 0.7).
- **Operadores.** `PathScan`, `PathFilter`; comparten `Filter/Project` con B1.
- **Aceptación.** Consulta por ruta indexada ≥ 10× más rápida que recorrido
  (mismo criterio que el planner del 0.6); round-trip JSON exacto.

### B4 — Búsqueda de texto completo y de objetos
- **Modelo.** Analizador (tokenizador + normalización + *stemming* opcional
  por idioma) → listas de *postings* persistidas (`space_kind=3`),
  codificadas por deltas. Ranking BM25. "Objetos": el índice se declara sobre
  una **ruta** de documento (B3), no solo sobre una columna.
- **Cambio frente al 0.7.** Hoy el FTS se reconstruye en memoria
  (perezoso). Persistirlo elimina el coste en la primera búsqueda y lo hace
  transaccional (los postings viajan en la misma tx que la fila).
- **Operadores.** `FtsScan(query) → (pk, score)`; se compone en el planner
  con filtros relacionales (§3).
- **Aceptación.** Resultados idénticos a la implementación en memoria del
  0.7 para consultas sin ranking; mejora medida en primera búsqueda y
  reapertura.

### B5 — Vectores *(diseño; necesita pruebas)*
**Estrategia: primero el oráculo, después el índice.**
1. **Exacto con SIMD** (`@Vector`; en 0.17.0 la autovectorización de bucles
   está deshabilitada) — es lo que existe hoy como fuerza bruta. Se conserva
   **siempre** como oráculo de exactitud.
2. **Aproximado** (HNSW o IVF; elegir con datos, no por moda) persistido como
   keyspace (`space_kind=4`): vecinos por capa, vectores en B1/B3.
3. **Cuantización** opcional (f16 / int8) después de medir recall.
- **Metas de prueba (a medir antes de decidir el índice).** Recall@10 ≥ 0.95
  contra el oráculo exacto; latencia p99 y memoria a 10⁵ y 10⁶ vectores,
  d = 64 y 768; coste de inserción y de reapertura.
- **Híbrido.** Pre-filtro (filtrar por B1/B3 y luego KNN) vs post-filtro (KNN y
  luego filtrar): el planner elige por selectividad estimada (§3).
- **Aceptación.** Se define con las metas anteriores; hasta entonces el bloque
  no sale de "diseño".

### B6 — Grafos y colecciones de grafos *(en discusión → decisión recomendada)*
**Decisión recomendada: el grafo es una capa sobre el kernel, no un motor.**
- **Modelo.** Nodos = filas/documentos (B1/B3). **Aristas de primera clase**:
  `(src, tipo, dst, props)`, guardadas dos veces como claves ordenadas:
  `out: src|tipo|dst` (`space_kind=5`) e `in: dst|tipo|src` (`=6`). Vecindad =
  *range scan* de un prefijo; no hace falta puntero-por-nodo.
- **Colección de grafos** = un `ns` (o `space_id`) por grafo; así un mismo
  nodo puede pertenecer a varios grafos y se aísla por permisos (B10).
- **Operadores.** `Traverse(dirección, tipos, profundidad)` (BFS con límite de
  frontera), camino más corto con presupuesto. Se compone con joins (§3).
- **Por qué no un motor aparte.** Mismas transacciones, mismo WAL, y las
  aristas se pueden unir a datos relacionales/vectoriales sin exportar nada.
- **Riesgo.** Supernodos (grados enormes): requiere paginación de la
  vecindad y estadísticas de grado.
- **Aceptación.** Traversal de profundidad 3 sobre grafo de 10⁶ aristas con
  latencia acotada y memoria proporcional a la frontera, no al grafo.

### B7 — Relaciones, joins y optimización
**Es el bloque que vuelve "híbrido" al sistema**: sin él, cada modelo es una
isla.
- **Álgebra común.** Todos los bloques producen *streams de filas con schema*
  y se componen con los mismos operadores. Joins: *index nested-loop* (usa
  índices de B1/B3), *merge join* (aprovecha el orden de PK del B+Tree) y
  *hash join* (con tope de memoria y derrame a disco, ligado a B2).
- **Planner por costos.** Estadísticas por tabla/índice: cardinalidad,
  histogramas, selectividad, grado medio de aristas, densidad de vectores.
  Mantenimiento incremental, no recorrido completo.
- **Relaciones declarativas.** Claves foráneas opcionales (integridad
  referencial + hint para el planner) y *joins por referencia* en documentos.
- **Consultas híbridas de ejemplo** (el objetivo del bloque):
  `KNN(vec, 20) ⋈ filas WHERE edad>18`, `FTS("x") ⋈ Traverse(amigos)`.
- **Aceptación.** Plan elegido ≤ 2× el mejor plan manual en un banco de
  consultas fijo; `EXPLAIN` determinista.

### B8 — Temporal
Dos acepciones; se recomienda **empezar por la de sistema**:
- **Tiempo de sistema (time-travel).** El kernel COW (P2) ya produce raíces
  inmutables por commit: retener raíces = consultar `AS OF <lsn|timestamp>`
  casi gratis. Política de retención + GC de páginas antiguas fuera del camino
  del escritor (P7). Reutiliza `space_kind=7` solo si se necesitan versiones
  lógicas por fila.
- **Tiempo de validez (bitemporal).** Columnas `valid_from/valid_to` con
  semántica declarativa y restricciones de no solapamiento; se construye
  encima de B1 sin tocar el kernel.
- **Aceptación.** `AS OF` devuelve exactamente el estado de ese commit
  (test diferencial contra reproducción del WAL hasta ese LSN).

### B9 — Extensibilidad: formatos, codecs y utilidades de integración
Interpreto "soporte para formato de librería/utilidades de integración
propia" como **mecanismo de extensión**: si te referías a otra cosa, dímelo
y se ajusta este bloque.
- **Codecs registrables** de valor/columna (un formato propio entra como
  codec, con su `type_tag` reservado y versionado).
- **Funciones y utilidades registrables** (escalares, de agregación, de
  texto/analizador) expuestas al planner con su coste.
- **Importadores/exportadores** como plugins sobre la C ABI (no dentro del
  núcleo).
- **Frontera de seguridad.** Las extensiones corren detrás de la C ABI; una
  extensión nunca escribe páginas directamente ni sobrepasa P3/P8.
- **Aceptación.** Un codec de ejemplo se registra, persiste, sobrevive a
  reinicio y se lee con el lector independiente (o falla limpio si falta).

### B10 — Multiusuario
Separar tres cosas que suelen mezclarse:
1. **Concurrencia de sesiones** (varias sesiones en un proceso): ya la
   habilita P2 (snapshots + escritor único).
2. **Identidad y permisos**: usuarios, roles (RBAC), permisos por
   namespace/colección/tabla; filas con política opcional (RLS).
3. **Multi-proceso**: **se resuelve con modo servidor (daemon)**, no
   compartiendo archivos. El modo embebido conserva "un dueño por directorio"
   (P5). Así no se rompe la garantía que acabas de imponer.
- **Aceptación.** Un rol sin permiso no puede leer ni inferir existencia
  (errores uniformes); auditoría de operaciones.

### B11 — Escalado vertical y horizontal *(release futura)*
Se diseña en este orden; cada etapa es útil por sí sola:
1. **Vertical.** Lecturas paralelas por snapshot (P2); decode/checkpoint en
   hilos (P7); escritura agrupada (P4). Después, escritor por partición.
2. **Réplica de lectura.** *Log shipping* del WAL (el WAL ya es el formato de
   replicación; LSN monótono + COMMIT por tx).
3. **Alta disponibilidad.** Consenso (Raft) sobre el log.
4. **Sharding.** Particionado por **rango de claves** (el B+Tree ya es
   ordenado); el `ns`/`space` de §1.1 es la unidad de reparto.
- **Aceptación por etapa.** Réplica converge bit a bit con el líder
  (comparación del contenido lógico); failover sin pérdida de commits
  confirmados (con P4).

### B12 — Embebida (propiedad transversal)
La biblioteca embebida es el **producto primario**; servidor, réplica y
cluster son capas opcionales sobre la misma biblioteca. Consecuencia: ningún
bloque puede exigir un proceso externo para funcionar en modo embebido.

---

## 3. Cómo se componen (por qué el sistema es "híbrido")

```
consulta  ──►  planner  ──►  árbol de operadores
                              │
   FtsScan ─┐                 │          todos devuelven (pk, …, score?)
   KNN     ─┼─►  Join / Filter / TopK ─► Project ─► resultado
   Traverse─┤                 ▲
   IndexScan┘          estadísticas de cada bloque
```

Regla: un bloque nuevo **no** añade un camino de consulta propio; añade
operadores y estadísticas al planner. Así el coste de integrar el bloque N+1
no crece con N.

---

## 4. Orden de integración y puertas de entrada

| Orden | Bloque | Estado (tuyo) | Puerta de entrada (debe estar cerrado antes) |
|---|---|---|---|
| 0 | Kernel + migración (P1–P12) | en proceso | — |
| 1 | B1 tipado/relacional | en proceso | F3 (snapshots COW) |
| 2 | B2 memoria/disco | en discusión | B1 estable; buffer pool |
| 3 | B3 documental | diseño | B1 + índices por ruta |
| 4 | B4 FTS/objetos | — | B3 (rutas) + B2 (persistencia) |
| 5 | B7 joins/planner | — | B1, B3; estadísticas |
| 6 | B8 temporal | — | P2 + retención de raíces |
| 7 | B5 vectores | diseño, pruebas | banco de recall + B7 (pre/post-filtro) |
| 8 | B6 grafos | en discusión | B7 (joins) + estadísticas de grado |
| 9 | B9 extensiones | — | C ABI estable (P11) |
| 10 | B10 multiusuario | — | modo servidor |
| 11 | B11 escalado | release futura | WAL estable + B10 |

*Nota sobre el orden.* Vectores va antes que grafos porque ya tiene
implementación base (fuerza bruta) y oráculo; grafos depende más de joins.
Es una recomendación: si tu prioridad de producto es otra, el orden de 5–8 es
intercambiable porque los bloques solo dependen del kernel y de B7, no entre
sí.

## 5. Decisiones abiertas — mi recomendación

| Tema | Recomendación | Qué lo cambiaría |
|---|---|---|
| ¿Grafo = motor o capa? | **Capa** sobre el kernel (B6) | Cargas de grafo puro con traversals de profundidad >6 sobre 10⁹ aristas |
| ¿Memoria/disco? | **Un buffer pool**, modos por tabla (B2) | Requisito de latencia submilisegundo estricta sin evicción → modo `memory` pinned (ya cubierto) |
| ¿Índice de vectores? | **Decidir con medición**: exacto SIMD → HNSW/IVF (B5) | Recall/latencia medidos en tu carga real |
| ¿Multiusuario multi-proceso? | **Modo servidor**, no archivo compartido (B10) | Necesidad real de múltiples procesos embebidos sobre un directorio |
| ¿Temporal? | **Sistema primero** (time-travel por raíces COW) (B8) | Requisito regulatorio de bitemporalidad |
| ¿Escalado horizontal? | **Réplica por WAL → Raft → sharding por rango** (B11) | Volumen que exija sharding antes que HA |

## 6. Qué NO hacer

- No construir un motor por modelo (duplica WAL, recuperación y bugs).
- No dar a un bloque su propio formato de archivo fuera del kernel.
- No exponer un camino de consulta que salte el planner.
- No aprobar un bloque sin su fila completa del contrato (§1.2).
- No empezar ningún bloque antes de que su puerta de entrada (§4) esté
  cerrada: los bloques heredan las garantías de P2–P8 solo si el kernel ya
  las ofrece.
