# GinaDB — Principios de diseño para la migración a Zig 0.17.0

Base: evaluación técnica de GinaDB Lite 0.7 (C++20) + notas de Zig 0.17.0
(<https://ziglang.org/download/0.17.0/release-notes.html>).

**Cómo leer este documento.** Cada principio tiene una *regla*, el *porqué*
(evidencia del 0.7) y un *criterio de aceptación* ejecutable. Un principio
sin criterio de aceptación no es principio, es deseo.

**Alcance de la verificación.** Los hallazgos marcados **[medido]** los
reproduje compilando y ejecutando el 0.7. Los marcados **[código]** salen de
leer el fuente, sin experimento. El lado Zig **no se compiló** (no había
`zig` en mi entorno): los cambios de 0.17.0 se cruzaron contra tu código por
búsqueda de patrones, no por compilación.

---

## 1. Qué se conserva (no se negocia)

- WAL con CRC + LSN, batch por transacción, reparación de cola corrupta.
- Checkpoint incremental con flip atómico de doble slot; free-list *derivada*
  (sin estado redundante que corromper).
- Formato especificado byte a byte (`docs/FORMATO_ARCHIVOS.md`) + lector
  independiente (`ginaread.zig`). La validación cruzada C++↔Zig cazó el bug
  más grave del 0.7 (delta-apply descartaba schemas superseded: 12→2 filas).
- Clasificación `corrupt` vs `suspect` en la integridad del delta.
- Metodología "medir antes de tocar" (informes 0.6 / 0.7).
- C ABI de 28 funciones como frontera estable (clientes Python / TS / Zig).

## 2. Hallazgos del 0.7 que se vuelven principios

| # | Hallazgo | Tipo |
|---|---|---|
| H1 | Dos procesos sobre el mismo directorio: 2×2000 inserts → al reabrir hay **2001 filas (esperadas 4001), sin error**. No hay `flock`. | medido |
| H2 | Tx multi-tabla no es atómica para lectores: un lector vio `a != b` en **13.587 y 42.781** lecturas (de ~1M) con una tx que actualiza ambas. | medido |
| H3 | `durable=false` por defecto: un commit no es durable. | código |
| H4 | WAL: un `write()` por frame (N+1 syscalls por tx), sin `writev` ni group commit. | código |
| H5 | Checkpoint síncrono dentro de `commit_ops_locked`, bajo `writer_mu`: picos de latencia en el commit. | código |
| H6 | El apply en memoria ocurre *después* del fsync y puede lanzar (p. ej. `bad_alloc`) sin rollback: memoria diverge del WAL hasta reiniciar. | código |
| H7 | Todo el dataset vive en RAM (B+Tree en memoria = autoridad); `Value` = `variant` con `std::map`/`string`/`vector` por celda. | código |
| H8 | El formato repite nombres de columna en cada fila; PK solo texto. | código |
| H9 | `ginaread.zig:306`: `@enumFromInt(body[1])` sobre `Kind = enum(u8)` exhaustivo, tras el CRC. Un kind desconocido pero con CRC válido (writer futuro) es Illegal Behavior. | código |
| H10 | README "Límites" aún dice que B+Tree/secundarios son "siguiente iteración (0.6)": doc desactualizada. | código |

Suites: `btree_test` OK, `unit_test` 177/0 fallos, `crash_test` 75/0 fallos
(ejecutadas). Los tests actuales **no** cubren H1 ni H2.

---

## 3. Principios

### P1 — El formato en disco es el contrato
**Regla.** Spec primero, código después. Todo cambio de formato sube versión
y mantiene lector de las anteriores (v5, v6). El lector independiente es
ciudadano de primera clase, no un extra.
**Porqué.** Es lo que ya salvó al 0.7.
**Aceptación.** Corpus de directorios generados por el motor C++ 0.7
(incluidos los de crash y corrupción); el motor Zig debe abrirlos y producir
el mismo contenido lógico, fila por fila. Test diferencial C++ ↔ Zig en CI
durante toda la migración.

### P2 — Un commit es una publicación atómica
**Regla.** Un lector observa una tx completa o nada. Mecanismo recomendado:
B+Tree *copy-on-write* con swap atómico de raíz (snapshot por lector, sin
locks de lectura), conservando escritor único.
**Porqué.** H2. Hoy el apply toma lock tabla por tabla.
**Aceptación.** El test de torn reads (tx que escribe `a` y `b`, lector que
compara) da **0** en ≥10⁶ lecturas, con TSAN.

### P3 — Tras el COMMIT en el WAL, el apply es infalible
**Regla.** Todo lo que puede fallar (validación, asignación de memoria,
codificación) ocurre **antes** de escribir el primer frame. Después del
COMMIT solo se ejecutan operaciones que no pueden devolver error.
**Porqué.** H6. En Zig es natural: los error sets hacen visible dónde se
puede fallar.
**Aceptación.** El apply no contiene `try`. Test con allocator que falla en
cada punto (`FailingAllocator`): el estado en memoria y el WAL nunca divergen.

### P4 — Durabilidad explícita, segura por defecto
**Regla.** `durable = true` por defecto; modo relajado opt-in y nombrado.
Una tx = un `writev`; group commit entre commits concurrentes; fsync del
directorio al crear/renombrar archivos.
**Porqué.** H3, H4.
**Aceptación.** Syscalls por commit = 1 write + 1 fsync (medido con `strace -c`).
Crash test con SIGKILL: ningún commit confirmado se pierde en modo durable.

### P5 — Un dueño por directorio, impuesto
**Regla.** Al abrir: lock exclusivo del directorio (`flock`/OFD lock). Si ya
está tomado, error explícito, nunca degradación silenciosa.
**Porqué.** H1: es pérdida de datos silenciosa, el peor tipo de fallo.
**Aceptación.** El test de dos procesos falla en la apertura del segundo con
error claro. *Quick win:* se puede añadir ya en el C++ 0.7.

### P6 — La memoria es caché, no autoridad (o declara su tope)
**Regla.** Decisión explícita y escrita: o (a) modo "lite" con `max_memory`
declarado y rechazo limpio al excederlo, o (b) page manager con buffer pool
(el plan 1.0). No quedarse en el medio sin decirlo.
**Porqué.** H7. Para una DB "de propósito general" el límite de RAM es el
límite de la base.
**Aceptación.** Documentado el tope; test que lo excede y verifica error
limpio (no OOM kill).

### P7 — El escritor nunca espera al checkpoint
**Regla.** Checkpoint desde un snapshot (P2), en hilo aparte; el escritor
solo se coordina para el rebase del WAL.
**Porqué.** H5.
**Aceptación.** p99 de latencia de commit con checkpoint activo ≤ 2× el p99
sin checkpoint (benchmark fijo, 100K filas).

### P8 — Los bytes del disco son hostiles
**Regla.** Orden estricto: longitud → CRC → *recién entonces* interpretar.
Enums on-disk son **no exhaustivos** (`enum(u8) { ..., _ }`); un valor
desconocido es un error de datos, nunca un panic ni UB. Límites duros en
toda longitud leída.
**Porqué.** H9. Además, en 0.17.0 `@fromBackingInt` y `@bitCast` a enum
tienen *safety check* de tag inválido: con corrupción o un writer futuro,
revienta en ReleaseSafe y es UB en ReleaseFast.
**Aceptación.** Fuzz del lector (WAL, base, delta) sin un solo panic; test
con kind desconocido → `error.UnknownFrameKind` o frame saltado según spec.

### P9 — Allocators explícitos, ownership por arena
**Regla.** Ninguna función aloca sin recibir un allocator. Arena por
transacción y por snapshot; liberar = soltar la arena. Fila = bytes
empaquetados + `schema_id` + ordinales (formato v7), decodificación perezosa;
`Value` como tagged union sobre slices, no contenedores por celda.
**Porqué.** H7, H8. Es lo que mejor encaja con Zig y lo que más baja el coste
de reapertura (el 0.7 ya demostró que el decode a `std::map` era el cuello).
**Aceptación.** Cero fugas bajo `SafeAllocator` (ver §4) en toda la suite;
reapertura @100K filas ≤ 228 ms (línea base 0.7).

### P10 — IO inyectable y fallos inyectables
**Regla.** El motor no llama al SO directamente: recibe una interfaz de IO
(en 0.16+ ya existe `std.Io`; aprovecharla). En tests se sustituye por una
implementación que falla, trunca o reordena escrituras de forma determinista.
**Porqué.** Los 75 crash tests actuales dependen de SIGKILL y timing; la
inyección los vuelve reproducibles y permite explorar *todos* los puntos de
corte.
**Aceptación.** Para cada punto de fsync/flip/rename del checkpoint, un test
que corta ahí y verifica recuperación a un prefijo contiguo de commits.

### P11 — La C ABI es la frontera estable
**Regla.** El motor Zig exporta las mismas 28 funciones (`export fn`), mismo
`cabi.h`, mismo JSON. Los clientes Python / TS / Zig no cambian.
**Aceptación.** `verify06.py`, `bun test` (11) y `ffi_demo` pasan sin tocar
una línea cliente.

### P12 — Rendimiento por medición, con SIMD explícito
**Regla.** Mantener la disciplina del informe 0.7 (medir, desglosar por fase,
corregir, re-medir). En Zig 0.17.0 la vectorización automática de bucles está
**deshabilitada** (workaround de LLVM 22), así que KNN (L2/coseno) y CRC se
escriben con `@Vector` explícito.
**Aceptación.** KNN y CRC32 no peores que el C++ 0.7 en el mismo hardware
(`make bench` vs `zig build bench`).

---

## 4. Zig 0.17.0 — qué cambió y cómo te toca

Publicada a inicios de oct 2026. Tu código Zig actual está en 0.16
(`std.Io.Threaded`, `std.Io.Dir`, `std.Io.File`).

### 4.1 Impacto directo en tu código (por búsqueda de patrones)

| Cambio en 0.17.0 | Dónde te toca | Acción |
|---|---|---|
| `@enumFromInt` / `@intFromEnum` deprecados → `@fromBackingInt` / `@backingInt` (`zig fmt` lo migra solo) | `ginaread.zig:306`, `gina.zig:261` | `zig fmt`; en `:306` además aplicar P8 (enum no exhaustivo) |
| `std.fmt.allocPrint` pasa a `allocator.print(...)` | `ginaread.zig:788, 802, 817`; `inspect.zig:94` | Reescribir las 4 llamadas |
| `std.heap.DebugAllocator` deprecado → `SafeAllocator` (thread-safe, reporta fugas, detecta double free y writes tras free) | No lo usas aún | Adoptarlo en tests/fuzz (P9) |
| `lang.OptimizeMode` → `lang.Optimize`; tags `.debug/.safe/.fast/.small` (los alias viejos existen pero `==`/`!=` no los aceptan) | `build.zig`: `.preferred_optimize_mode = .ReleaseSafe` | Cambiar a `.safe`; confirmar el nombre del campo compilando |
| `@bitCast` redefinido: opera sobre la representación lógica de bits; arrays/vectores cambian *sin error de compilación*; `extern struct`/`extern union` ya no permitidos | `ginaread.zig:167, 170, 200` son int↔int / int→float: **no afectados** | Auditar todo `@bitCast` nuevo con vectores (SIMD de P12); para type-punning usar `@ptrCast` o `std.mem.readInt(..., .little)` |
| `@cImport` eliminado; `TranslateC` pasa a paquete externo | Tu código **no** usa `@cImport` (verificado) | Sin impacto; para tests diferenciales contra `libginadb.so` usar `extern` a mano o el paquete `translate-c` |

### 4.2 Cambios que importan al diseñar el motor nuevo

| Cambio | Relevancia |
|---|---|
| `std.heap.StackFallbackAllocator` → `BufferFirstAllocator` (buffer pasado como argumento, alineación explícita) | Útil para el patrón "small vec" en claves/filas pequeñas |
| `ArrayList`: `getLast`/`getLastOrNull` → `last()`; chequeo de pointer stability en debug; `debug.SafetyLock` con lock compartido | Caza en debug punteros invalidados (clave para nodos del B+Tree) y mal uso de lectores/escritor |
| `std.Io.Semaphore.waitTimeout` | Coordinación con timeout (group commit, checkpoint en background) |
| `hash.crc`: auditoría completa del namespace | Si sustituyes tu CRC slice-by-8 por `std.hash.crc`, valida contra los vectores de `make crc-check`; el CRC del formato **no puede cambiar** |
| Reflexión estilo struct-of-arrays (`@typeInfo(S).@"struct".field_names/field_types`) | Si derivas codecs/schemas con `comptime` |
| `std.zon.parse` rediseñado (args struct, arena) | Solo si lees configuración `.zon` |
| `a ** b` en arrays eliminado → `@splat`; sin `errdefer \|err\|`, sin `i0`, sin `void{}` | Reglas de estilo nuevo; `zig fmt` no cubre todas |
| `@divCeil` nuevo | Aritmética de páginas/extents sin `catch unreachable` |
| `@import("builtin").cpu/os/abi/object_format` deprecados (se eliminan en 0.18) → `.target.*`; `std.builtin` → `std.lang` | Usar la forma nueva desde el inicio |
| Mínimos de SO: Linux 5.10+, Darwin 15+, Windows 10+, FreeBSD 14+ | Define tu piso de plataforma |

### 4.3 Sistema de build y herramientas

- `zig build` ahora separa *configurer* y *maker*; se elimina la posibilidad
  de sustituir el build runner. Cambios de API: `b.build_root` → `b.root`,
  `b.args` → `addPassthruArgs`, familias `addFileArg` → `addFileArg2`, etc.
  Tu `build.zig` usa `b.path`, `addLibrary`, `addExecutable`, `addTest`,
  `addRunArtifact`, `addInstallArtifact`, `addLibraryPath`, `linkSystemLibrary`,
  `link_libc`: ninguno aparece en las listas de cambios, pero **hay que
  confirmarlo compilando**.
- **ZLS no funciona con 0.17.0** (por la separación configurer/maker); el
  equipo de Zig y el de ZLS trabajan en restaurarlo vía Build Server Protocol.
  Cuenta con editor sin autocompletado de ZLS por ahora.
- Compilación incremental utilizable en `x86_64-linux`:
  `zig build -fincremental --watch`.

### 4.4 Riesgos conocidos de 0.17.0

Las propias notas dicen que esta release contiene bugs, miscompilaciones y
regresiones, y que un proyecto no trivial en Zig 0.17.x puede requerir
participar en el desarrollo. Regresiones listadas: #37006 (compiler-rt soft
float en x86), #36986 (símbolos weak de libc no sobrescribibles), #36444
(response files en `Run` por la separación configurer/maker), #37050
(SPIR-V). Además, la comunidad menciona que algunos desarrolladores
saltaron 0.17.0 por bugs y que esperar a 0.17.1 puede tener sentido.

**Política recomendada:**
1. Fijar la versión exacta (`minimum_zig_version` en `build.zig.zon`) y
   compilar en CI con esa versión.
2. Mantener la matriz de CI lista para 0.17.1 (cambiar un número).
3. El motor no debe depender de nada de la lista de regresiones: evitar
   sobrescribir símbolos de libc, no usar response files, no apuntar a
   soft-float x86 ni a SPIR-V.
4. Mientras el motor C++ 0.7 exista, es el **oráculo** de conformidad: si
   una regresión del compilador rompe el motor Zig, el test diferencial lo
   delata.

---

## 5. Plan de migración con criterios de salida

| Fase | Contenido | Sale cuando |
|---|---|---|
| **F0** | Congelar C++ 0.7 como oráculo. Quick wins en C++: `flock` (P5), test de torn reads y de dos procesos en la suite. Corregir README (H10). | Los dos tests nuevos existen y *fallan* de forma conocida (H1/H2) o pasan tras el fix. |
| **F1** | Portar `ginaread.zig` / `inspect.zig` / `gina.zig` a 0.17.0 (tabla 4.1) + P8 (enum no exhaustivo, fuzz). | `zig build test` verde en 0.17.0; corpus de P1 abre idéntico al C++. |
| **F2** | Storage en Zig: pager, WAL, delta, free-list. Mismas secuencias de operaciones que el C++ → mismos bytes (golden files). | Diferencial byte a byte; P4 (writev + durable) y P10 (IO inyectable). |
| **F3** | Motor: B+Tree COW + snapshots (P2), tipos/schemas, planner, índices secundarios, FTS, KNN SIMD (P12). | Torn reads = 0; suites unit/crash portadas al 100 %. |
| **F4** | Escritor/checkpoint: apply infalible (P3), checkpoint en background (P7), decisión de memoria (P6). | p99 de commit con checkpoint ≤ 2×; FailingAllocator sin divergencia. |
| **F5** | C ABI exportada (P11) + clientes sin cambios + formato v7 opcional (P9). | `verify06.py`, `bun test`, `ffi_demo` verdes; reapertura ≤ 228 ms @100K. |

**Regla de oro de la migración:** nunca se retira el oráculo C++ hasta que F5
cierre y el corpus de conformidad lleve un ciclo completo sin diferencias.
