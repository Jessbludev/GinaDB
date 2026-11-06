# TASK-PACKET — Sesión 1: Fase F1 de la migración a Zig 0.17

**Estado:** APROBADO (respuestas del usuario, 2026-11-06).
**Autoridad de diseño:** `docs/GINADB-PRINCIPIOS-DISENO.md` (P1–P12; plan de
migración §5). Ante cualquier conflicto, manda ese documento.
**Fuera de esta sesión:** `docs/GINADB-BLOQUES-MULTIMODELO.md` (bloques
B1–B12) — no se toca ni se implementa nada de él en esta sesión.
**Reglas de conducta del agente:** `.agent/skills/immutable-db-scope-guard/SKILL.md`
(alcance congelado, higiene de logs, cambios mínimos).

## Objetivo

Ejecutar la **Fase F1** del plan de migración (§5 de PRINCIPIOS-DISENO):
portar el código Zig existente (`zig/ginaread.zig`, `zig/inspect.zig`,
`zig/gina.zig`, `zig/build.zig`) de Zig 0.16 a Zig 0.17 según la tabla §4.1,
más el endurecimiento **P8** en el lector (kinds WAL desconocidos = dato,
nunca UB ni panic), dejando `zig build test` en verde y una demo ejecutable
contra el oráculo C++ 0.7 (congelado, sin modificaciones).

## In-scope (explícito)

1. Toolchain Zig **0.17.1** persistente en `/home/z/my-project/tools/`;
   versión exacta fijada en `zig/build.zig.zon` (`minimum_zig_version`),
   política §4.4 del documento de principios.
2. Migración de APIs según tabla §4.1:
   - `@enumFromInt` / `@intFromEnum` → `@fromBackingInt` / `@backingInt`
     (`ginaread.zig:306`, `gina.zig:261`).
   - `std.fmt.allocPrint` → `Allocator.print` (`ginaread.zig:788/802/817`,
     `inspect.zig:94`).
   - `build.zig`: `.preferred_optimize_mode = .ReleaseSafe` → `.safe`
     (alias viejos deprecados; se confirma compilando, §4.1).
   - Auditoría de `@bitCast` (`ginaread.zig:167/170/200`): son castings
     escalares int↔int / int→float, **sin impacto** según §4.1 — se
     documentan, no se reescriben.
3. **P8 en el lector:** kind WAL desconocido con CRC válido se trata como
   dato (frame saltado en replay según spec); nombre seguro sin `@tagName`
   (que es comportamiento ilegal para valores sin nombre); test con kind
   desconocido.
4. Cambios adicionales de API exigidos **solo por el compilador 0.17.1**
   (la tabla §4.1 se redactó por búsqueda de patrones, no compilando; el
   propio documento exige "confirmarlo compilando"). Ej.: `Allocator.dupeZ`
   eliminado → `dupeSentinel`. Cada descubrimiento se registra en la
   bitácora con archivo:línea.
5. Criterio de verificación 1: `zig build test` exit 0 con Zig 0.17.1.
6. Criterio de verificación 2 (demo contra el oráculo):
   `make libginadb.so` (C++ 0.7 sin tocar) → `zig build ffi-demo` →
   `gina-ffi-demo` (cliente Zig vía C ABI del motor C++) → `gina-inspect`
   sobre los archivos escritos por el C++ (lector Zig puro; miniatura del
   criterio P1: el formato es el contrato) → `gina-inspect --wal`.
7. Registro: git (commits atómicos, mensajes en inglés) +
   `REGISTRO-DE-CAMBIOS.md` append-only (español) + `NOTA-DE-ALCANCE.md`
   (español). Entrega en un único ZIP con `.git` incluido.
8. Gobernanza en repo: documentos rectores en `docs/`, skill en
   `.agent/skills/`, este TASK-PACKET en la raíz.

## Out-of-scope (explícito)

- **Bloques B1–B12** y cualquier edición de `docs/GINADB-BLOQUES-MULTIMODELO.md`
  (próxima sesión, decisión del usuario).
- **Port FFI con Haskell (sesiones 3–4):** solo se REGISTRA aquí y en la
  bitácora como decisión futura; NO se implementa, NO se reserva código, NO
  se crea infraestructura.
- **F0** (quick wins en C++: `flock`/P5, tests de dos procesos y torn reads,
  corrección del README/H10): el C++ 0.7 queda congelado como oráculo; sin
  cambios en esta sesión.
- **F2–F5** (storage/pager/WAL en Zig, motor B+Tree COW, checkpoint,
  exportación de la C ABI de 28 funciones desde Zig, formato v7).
- **Fuzzing del lector (P8):** pendiente; requiere decisión de
  infraestructura (fuzzer integrado de 0.17 vs. corpus propio) — ver
  NOTA-DE-ALCANCE.
- Cualquier refactor, abstracción, dependencia o archivo adyacente no
  listado arriba.

## Criterios de verificación (acordados con el usuario)

1. `zig build test` con Zig 0.17.1 → exit 0.
2. Demo con salida correcta y verificable:
   - `gina-ffi-demo` crea una BD vía la C ABI del motor C++ 0.7 y ejecuta
     CRUD + KNN + select con planner + checkpoint/compact.
   - `gina-inspect <dir>` (lector Zig puro) abre esos mismos archivos y
     reporta tablas/filas/delta/WAL coherentes con lo escrito por el C++.
3. `git log` legible y atómico; bitácora append-only (nunca se reescribe).
4. Un único ZIP con el repo completo, incluida `.git`.

## Registro de la sesión dentro del plan

| Sesión | Contenido | Estado |
|---|---|---|
| 1 (esta) | F1: port de las herramientas Zig a 0.17 + P8 lector | en curso |
| 2 | Según el usuario (BLOQUES sigue intocado; el plan prevé F2+) | pendiente |
| 3–4 | Port de conexión con Haskell (FFI) — NO en esta sesión | pendiente |
