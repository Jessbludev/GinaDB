# REGISTRO DE CAMBIOS (bitácora append-only)

> **Regla de esta bitácora:** las entradas solo se AGREGAN al final, nunca se
> reescriben ni se reordenan. Corrige con una entrada nueva, no editando la
> vieja. Cada entrada cita los hashes reales de git; el historial de git es la
> fuente de verdad y esta bitácora es su lectura humana.

---

## Entrada 1 — Sesión 1: Fase F1, port de herramientas Zig a 0.17

**Fecha:** 2026-11-06 (America/Bogota)
**Task Packet:** `TASK-PACKET.md` (aprobado por el usuario en esta sesión)
**Autoridad de diseño:** `docs/GINADB-PRINCIPIOS-DISENO.md` (P1–P12, §4–§5)
**Toolchain:** Zig 0.17.1 (linux x86_64), fijado en `zig/build.zig.zon`
(`minimum_zig_version = "0.17.1"`), persistido en
`/home/z/my-project/tools/zig-x86_64-linux-0.17.1/`.
**Oráculo:** motor C++ 0.7 congelado, sin una línea modificada
(`git diff f498458 -- src/ include/ tests/ tools/ Makefile CMakeLists.txt` = vacío).

### Commits de esta sesión (9, en orden)

| Hash | Asunto |
|---|---|
| `f498458` | Importación tal cual del zip GinaDB-main (baseline; 60 archivos) |
| `48c90fb` | Gobernanza: documentos rectores P1–P12 y bloques B1–B12 en `docs/` + skill scope-guard en `.agent/skills/` |
| `34dd8e1` | TASK-PACKET de la sesión (alcance congelado) |
| `6d3478e` | Anclaje del toolchain: `zig/build.zig.zon` (pin 0.17.1) + `.gitignore` de artefactos |
| `604dd58` | `build.zig`: `.preferred_optimize_mode = .ReleaseSafe` → `.safe` (§4.1) |
| `ec209eb` | Migración mecánica con `zig fmt` 0.17: `@enumFromInt`→`@fromBackingInt` (`ginaread.zig`, era :306), `@intFromEnum`→`@backingInt` (`gina.zig`, era :261) |
| `afe0379` | APIs std eliminadas/deprecadas: `std.fmt.allocPrint`→`Allocator.print` (4 sitios: `ginaread.zig` 788/802/817 y `inspect.zig` 94) y `Allocator.dupeZ`→`dupeSentinel(.., 0)` (3 sitios: `ginaread.zig` 1056, `gina.zig` 129/140) |
| `85f7ef4` | **P8**: `kindName()` seguro (los `@tagName` sobre valores sin nombre son comportamiento ilegal); `inspect --wal` lo usa; test de regresión con kind 99 desconocido; cadena `gina_read_version` → `"ginaread 0.7 (zig 0.17)"` |
| `ef3efeb` | Fingerprint de `build.zig.zon` calculado por el toolchain (0.17 lo valida contra la identidad del paquete; no es aleatorio) |

### Hallazgos de compilación (más allá de la tabla §4.1)

La tabla §4.1 del documento de principios se redactó por búsqueda de patrones
(el propio documento lo advierte). La compilación real con 0.17.1 reveló:

1. **`Allocator.dupeZ` eliminado** en 0.17 (no estaba en §4.1). Reemplazo:
   `Allocator.dupeSentinel(alloc, u8, s, 0)` → `[:0]u8`. 3 sitios.
2. **`std.fmt.allocPrint` sigue existiendo pero deprecado**
   ("Deprecated in favor of `Allocator.print`"). Migrado de todos modos según §4.1.
3. **Fingerprint de `build.zig.zon` validado por el toolchain** contra la
   identidad del paquete: el valor debe ser el que el propio `zig build`
   calcula (un fingerprint aleatorio es rechazado con el valor correcto en el
   mensaje de error).
4. **Sin más rupturas**: `ArrayListUnmanaged`, `StringArrayHashMapUnmanaged`,
   `AutoArrayHashMapUnmanaged`, `std.Io.Threaded`, `std.Io.Dir.readFileAlloc`,
   `std.Io.File.stdout().writer`, `std.process.Init.Minimal`, `callconv(.c)`,
   `std.hash.Wyhash`, `std.base64`, `std.mem.sort`, `errdefer` simple y los
   `@bitCast` escalares int↔int/int→float (`ginaread.zig` 167/170/200,
   auditados, sin impacto según §4.1) compilan y funcionan sin cambios.

### Verificación (criterios acordados)

**Criterio 1 — `zig build test` en verde con 0.17.1:**

```
$ zig build test --summary all
Build Summary: 3/3 steps succeeded
test success
+- run test success
EXIT: 0
```

Suite del lector: 4 tests preexistentes + 1 nuevo (P8, kind desconocido).

**Criterio 2 — demo contra el oráculo C++ (sin tocar el C++):**

```
$ make libginadb.so                       # motor C++ 0.7 (oráculo)
$ zig build ffi-demo                      # cliente Zig 0.17.1 sobre la C ABI
$ LD_LIBRARY_PATH=. ./zig/zig-out/bin/gina-ffi-demo demo-data
engine: GinaDB Lite 0.7
get(m1) = {"id":"m1","tag":"gina","vec":{"$vec":[0.100000001,0.200000003,0.300000012]}}
knn(vec,q,k=2) = [{"key":"m1","score":1},{"key":"m3","score":0.998713062}]
count after batch del = 2
select eq email (índice) = [{"age":36,"email":"ada@db.io","id":"u2"}]
select rango age (índice) = [{"age":33,"id":"u1"},{"age":36,"id":"u2"}]
compactación OK
stats = {"tables":2,"rows":5,"last_lsn":20,"commits":8,"checkpoints":2,...}
Zig FFI demo OK
```

El lector Zig puro abre los MISMOS archivos escritos por el motor C++
(miniatura del criterio P1 — el formato es el contrato):

```
$ ./zig/zig-out/bin/gina-inspect demo-data
GinaDB Lite 0.7 (Zig reader) — demo-data
  tables: 2
  - media: 2 rows (pk='id', schemaless=false, indexes=1)
  - users: 3 rows (pk='id', schemaless=false, indexes=2)
  rows total: 5
  last_lsn: 20  checkpoint_lsn: 19
  snapshot: 3 pages (+0 overflow)
  wal: 1 frames, bad tail: 0 bytes

$ ./zig/zig-out/bin/gina-inspect --wal demo-data
WAL demo-data/wal.log: 1 frames, 38 good bytes, 0 bad tail bytes
  lsn=    20 tx=   0 kind=   base v6 payload=8B
```

Coherencia cruzada C++ ↔ Zig: `rows:5` ↔ `rows total: 5`;
`last_lsn:20` ↔ `last_lsn: 20`; `pages:3` ↔ `snapshot: 3 pages`;
`wal_bytes:38` ↔ `38 good bytes`.

### Estado del plan de migración (§5 de PRINCIPIOS-DISENO)

| Fase | Estado |
|---|---|
| F0 (quick wins C++, flock/P5, tests H1/H2, README H10) | **pendiente** — el C++ quedó congelado como oráculo en esta sesión |
| **F1 (port herramientas Zig a 0.17 + P8 lector)** | **hecha en esta sesión** (tests + demo verificados) |
| F2 (storage Zig: pager, WAL, delta, free-list) | pendiente |
| F3–F5 | pendientes |

### Próximas sesiones (según el usuario)

- **Sesión 2:** según el usuario. `docs/GINADB-BLOQUES-MULTIMODELO.md` sigue
  intocado y sin implementar nada de B1–B12.
- **Sesiones 3–4:** port de conexión con Haskell (FFI). Solo registrado aquí;
  no se implementó ni se reservó código en esta sesión.

---

## Entrada 2 — Sesión 2: Bloque B6 (grafos como capa)

**Fecha:** 2026-11-06 (America/Bogota)
**Task Packet:** `TASK-PACKET-SESION2.md` (directiva del usuario: 1 bloque por
sesión, empezando por el de integración más rápida, probar→validar→corregir
hasta madurez)
**Bloque elegido:** **B6 — grafos y colecciones de grafos**, implementado como
capa sobre la C ABI del motor C++ 0.7 congelado (la decisión recomendada por
§5 del documento de bloques). Por qué no otro bloque: B1/B3/B4/B8 requieren
cirugía del kernel (PK generalizada, índices por ruta, postings persistentes,
retención de raíces — fases F2–F5 pendientes) y B5 está, por decisión del
propio documento, en "diseño" hasta medir recall; B6 era el único que se
integra sin romper la congelación del oráculo.

**Commits de esta sesión:**

| Hash | Asunto |
|---|---|
| `563742d` | TASK-PACKET sesión 2 (alcance congelado) |
| `f8ef61e` | feat(graph): capa B6 — codec, doble entrada atómica, prefijos exactos, BFS, suite y bench |
| (este commit) | docs(governance): contrato §1.2 de B6 + bitácora entrada 2 + nota de alcance |

### Diseño en 1 párrafo (paso 2 de la skill, congelado en el Task Packet)

Un grafo son dos tablas (`g.<nombre>.out` / `g.<nombre>.in`) con PK compuesta
codificada por byte-stuffing de `0x1F`; cada arista son dos filas en UNA
`gina_exec` atómica; la vecindad es un rango exacto por prefijo del B+Tree
(`and(ge(P), lt(P'))`, `lt` exclusivo); `traverse` es BFS con memoria ∝
frontera; sin registros WAL nuevos (K_PUT/K_DEL ordinarios), oráculo C++ y
lector independiente intactos; `space_kind` 5/6 llegará con el kernel Zig sin
cambiar esta interfaz.

### Hallazgos de implementación (correcciones hasta madurez)

1. **Alias de búfer** (el bug más grave de esta sesión): `nodePrefix`
   devolvía un slice DENTRO del búfer de trabajo y `scanPrefix` construía la
   consulta en ese mismo búfer, sobrescribiendo el prefijo al escaparlo
   (resultados basura o errores intermitentes). Fix: búferes separados
   (`buf` para consultas, `pbuf` para prefijos).
2. **`ArenaAllocator` rewind**: liberar slices individuales de un arena puede
   rebobinar el último bloque y corregir sibling slices (perdía nodos del
   BFS). Fix: con arena nunca se libera ítem por ítem (lección documentada en
   el código).
3. **`gina_count` filtra `GINA_ENOENT`(=1) como valor de retorno** para tabla
   inexistente (rareza congelada de la C ABI 0.7). Fix en la capa:
   desambiguar con `gina_last_status()`.
4. **Migración 0.17.1 confirmada**: `ArrayListUnmanaged{}` ya no compila
   (`.empty` con anotación de tipo), `**` de arrays eliminado,
   `std.time.nanoTimestamp` eliminado → `std.Io.Timestamp.now(io, .awake)` +
   `durationTo`, `io` va por valor, `allocSentinel` exige free del slice con
   centinela.
5. **Probe del oráculo**: ante fallos contradictorios, se sondeó el motor
   directamente por ctypes (`scripts/probe_select.py`, fuera del repo) antes
   de tocar la capa: el motor respondía correcto → el bug era de la capa
   (hallazgo 1). Higiene de logs: solo primera línea + path:línea.

### Verificación (criterios del Task Packet, todos en verde)

1. `zig build test`: **5/5** (lector puro intacto, sin libginadb).
2. `zig build graph-test`: **18/18** contra `libginadb.so` (codec, rango
   exacto n1≠n10/n11, atomicidad de la doble entrada con lote rechazado,
   direcciones, tipos, ciclos, límites de frontera/escaneo, camino con
   presupuesto, props, borrado, grado, reinicio, drop).
3. Demo `gina-graph`: grafo social → vecinos → traversal → camino → props →
   borrado → **reinicio** (5 aristas persistidas, traversal idéntico).
4. **Bench 10⁶ aristas** (aceptación §2-B6): retícula anular, inserción atómica
   22 325 aristas/s, reapertura en frío 1.44 s, **traversal profundidad 3:
   31 nodos en 1 ms con frontera 10/10/10** (verificación analítica: nivel d
   = {(d-1)K+1..dK}), camino n0→n40 = 4 saltos, supernodo de 20 k aristas
   contenido por el tope de frontera (101 nodos, `truncated`).
5. **Oráculo intacto**: `git diff f498458 -- src/ include/ tests/ tools/
   Makefile CMakeLists.txt` = vacío (contenido; los mode-bits del sandbox se
   silenciaron con `core.filemode=false`).

### Estado del plan (§4 del documento de bloques)

| Bloque | Estado |
|---|---|
| B6 grafos (capa) | **hecho y maduro en esta sesión** (contrato §1.2 completo: `docs/BLOQUE-B6-CONTRATO.md`) |
| B1, B3, B4, B8 | requieren kernel Zig (F2–F5) o cambios en el C++ congelado — siguientes por prioridad del usuario |
| B7 joins/planner | pendiente; es la puerta formal de B6 (la composición con joins llega aquí) |

### Próximas sesiones

- **Sesión 3:** según el usuario. Los bloques de integración rápida sobre la
  C ABI que quedan son escasos (B9 extensiones necesita registro en motor);
  B7 o F2 (storage Zig) son los siguientes grandes. La puerta de B6 (B7 +
  estadísticas de grado) queda documentada como desviación registrada en
  `NOTA-DE-ALCANCE.md`.
- **Sesiones 3–4:** port de conexión con Haskell (FFI). Solo registrado; no
  se implementó nada ni se reservó código.
