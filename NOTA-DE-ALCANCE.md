# NOTA DE ALCANCE — Sesión 1 (Fase F1)

Supuestos, desviaciones y decisiones pendientes. Cada supuesto es reversible;
si alguno contradice lo decidido, se corrige con una entrada nueva en la
bitácora (`REGISTRO-DE-CAMBIOS.md`), sin reescribir esta nota.

## Supuestos tomados (con porqué e impacto)

1. **Toolchain 0.17.1, no 0.17.0.** El documento de principios se redactó
   contra las release notes de 0.17.0, pero su propia política §4.4 recomienda
   fijar la versión exacta y mantener la matriz lista para 0.17.1 (regresiones
   conocidas de 0.17.0: response files en `Run` — used por `zig build test` —,
   símbolos weak de libc, etc.). Se fijó **0.17.1** (parche final de la serie
   0.17, misma semántica de lenguaje/stdlib que las notas de 0.17.0).
   *Impacto:* cambiar a 0.17.0 exacto = editar `minimum_zig_version` y un
   número de CI. Sin cambios de código previstos.
2. **Interpretación de la opción "demo" elegida.** El texto de la opción
   (abrir → append → read_from(seq) → cerrar) describía la API de la capa
   wallet de la skill, que pertenece a las sesiones 3–4 (Haskell) y quedó
   fuera de alcance. La demo ejecutada es la que corresponde a F1:
   ffi-demo contra el motor C++ (C ABI) + lector Zig puro sobre los mismos
   archivos + recorrido del WAL.
3. **Documentos rectores dentro del repo.** El zip no incluía
   `GINADB-PRINCIPIOS-DISENO.md` ni `GINADB-BLOQUES-MULTIMODELO.md`; se
   copiaron verbatim (diff verificado) a `docs/` para que el repo sea
   autodescriptivo en GitHub. La skill `immutable-db-scope-guard` se copió
   verbatim a `.agent/skills/` (elección explícita del usuario).
4. **Idioma:** documentación principal (TASK-PACKET, NOTA-DE-ALCANCE,
   REGISTRO-DE-CAMBIOS, documentos rectores) en español; código, comentarios
   de código y mensajes de commit en inglés (elección explícita del usuario).
5. **Cadena `gina_read_version` actualizada** de `"ginaread 0.5 (zig 0.16)"`
   (desactualizada: decía 0.5 con repo 0.7) a `"ginaread 0.7 (zig 0.17)"`.
   Verificado con grep: ningún test ni cliente depende de su contenido.
6. **El commit base no contiene .git del zip** (los zip de GitHub no lo
   incluyen). El historial real comienza en `f498458` (importación). Si el
   repo remoto del usuario ya tiene historial, su agente debe hacer merge o
   rebase; la bitácora cita hashes del historial local.

## Desviaciones registradas (protocolo de la skill)

- **Ninguna fuera de alcance.** Los únicos cambios de código tocaron los 5
  archivos de `zig/` listados en el TASK-PACKET. Cero cambios en
  `src/`, `include/`, `tests/`, `tools/`, `benchmark/`, `examples/`,
  `Makefile`, `CMakeLists.txt`, `docs/ARQUITECTURA.md`, etc.
- Cambios "adyacentes" mínimos y declarados: `zig/build.zig.zon` (nuevo,
  exigido por la política §4.4 del propio documento), `.gitignore` (nuevo,
  artefactos de build C++/Zig), documentos de gobernanza.
- El fuzzing del lector (P8) NO se implementó: requiere decidir
  infraestructura (fuzzer integrado de 0.17 vs. corpus propio de archivos
  corruptos). Es una **decisión pendiente**, no una omisión silenciosa.

## Decisiones pendientes (no abrir sin el usuario)

| # | Tema | Contexto |
|---|---|---|
| D1 | F0: quick wins en C++ (flock/P5, tests de dos procesos y torn reads, corrección README/H10) | El C++ quedó congelado como oráculo; tocarlo es decisión del usuario |
| D2 | Fuzzing del lector (P8) | 0.17 trae fuzzer integrado; alternativa: corpus de archivos corruptos como fixtures. Elegir con el usuario |
| D3 | Infraestructura de test diferencial C++↔Zig en CI (criterio P1 completo) | La miniatura de esta sesión (ffi-demo + gina-inspect) cubre la cadena manual; el corpus completo y el CI quedan para F1.5/F2 |
| D4 | Port FFI con Haskell (sesiones 3–4) | Registrado en TASK-PACKET; no se reservó código ni infraestructura |
| D5 | Bloques B1–B12 (`docs/GINADB-BLOQUES-MULTIMODELO.md`) | Próxima sesión según el usuario; el documento no se tocó |

## Pendientes menores observados (no corregidos: fuera de alcance)

- `zig/ffi_demo.zig` contiene una consulta `select` muerta con un campo JSON
  vacío (`"\"\":[]"`) y resultado sin liberar (`_ = rng;`) — preexistente,
  cosmético, no rompe nada; se deja tal cual (regla de cambio mínimo).
- `build.zig` mantiene el comentario de cabecera "GinaDB Lite 0.5 — Zig
  build" (desactualizado, preexistente, cosmético).
- ZLS no funciona con 0.17.0/0.17.1 (separación configurer/maker; nota §4.3
  del documento): editar sin autocompletado por ahora.

---

## Sesión 2 — Bloque B6 (grafos como capa)

### Desviación registrada (protocolo de la skill)

1. **Nombre:** empezar B6 con su puerta §4 (B7 joins + estadísticas de grado)
   sin cerrar.
2. **Clase:** decisión de alcance (no es feature nueva: es la forma de
   implementar lo pedido por el usuario con lo que existe).
3. **Por qué es segura:** la capa no toca el kernel ni el formato (cero
   registros WAL nuevos), así que no hereda garantías que el kernel no dé;
   `Traverse` no necesita joins (solo range scans), el guardia de supernodos
   sustituye a las estadísticas de grado mientras B7 llega, y el rector
   declara el orden 5–8 intercambiable por prioridad de producto.
4. **Impacto al cerrar B7:** componer `FtsScan/KNN/Traverse ⋈ filas` con el
   planner y añadir histogramas de grado persistidos; la interfaz pública de
   la capa no cambia.

### Supuestos tomados (con porqué e impacto)

1. **Nodos sin FK**: la capa guarda ids de nodos como cadenas opacas; borrar
   un nodo de su tabla no limpia sus aristas (integridad referencial = B7).
2. **Props solo en la fila `out`**: la fila `in` es espejo mínimo (menos RAM,
   doble entrada más barata); `getEdge` lee siempre de `out`.
3. **`dropGraph` best-effort** (dos DDL del motor, no una tx): documentado en
   el contrato del bloque.
4. **`truncated` heurístico por escaneo**: exactamente `scan_limit`
   resultados marca truncado (conservador; un grado justo igual al límite se
   reporta como truncado).
5. **Bench con `checkpoint_every` por defecto del motor** (1000 commits); no
   se pretende benchmark del motor sino verificación de aceptación de B6
   (frontera acotada, latencia medida).
6. **El motor reporta `GINA_ENOENT`(=1) como valor de `gina_count`** en tabla
   inexistente; la capa lo desambigua vía status. No se "corrige" el C++
   (congelado); queda registrado por si F2 quiere arreglarlo en el motor Zig.
7. **`core.filemode=false`**: el sandbox reciclado restaura mode-bits 755 en
   todo el árbol; es ruido del entorno, no un cambio real (verificado con
   `git diff` de contenido).
