# Formato de archivos de GinaDB Lite 0.6

Especificación byte a byte. Implementaciones de referencia:
`src/codec.cpp` (C++) y `zig/ginaread.zig` (Zig). Toda la serialización es
**little-endian** y todos los strings son UTF-8 con longitud prefijada.

Este documento existe para que **cualquier lenguaje pueda leer (o escribir)
bases de GinaDB sin el motor C++**, que es parte de la visión del proyecto.

Nuevo en 0.6: checkpoint incremental `delta.pages` (sección 4), índices
secundarios en el schema, kind RESCH en el WAL, versiones 6 con lectura de 4/5.

## 1. Tipos de valor (tags)

| Tag | Tipo | Codificación tras el tag |
|----:|------|--------------------------|
| 0 | null | (nada) |
| 1 | bool | 1 byte (0/1) |
| 2 | int64 | 8 bytes LE |
| 3 | float64 | 8 bytes IEEE-754 (bits en LE) |
| 4 | text | string |
| 5 | bytes | u32 len + bytes crudos |
| 6 | array | u32 n + n valores recursivos |
| 7 | object | u32 n + n pares (string, valor) |
| 8 | vector | u32 dim + dim × f32 LE |

Primitivas comunes:

```
u32/i64: little-endian
string  := u32 len + len bytes
row     := u32 n + n × (string clave, valor)
```

## 2. WAL (`wal.log`)

Archivo append-only de **frames**. Cada transacción escribe sus frames de
operación y al final un frame COMMIT. El recovery solo aplica transacciones
con COMMIT completo.

```
frame   := u32 magic | u32 body_len | u32 crc32(body) | body
magic   := 0x414E4947 ("GINA" en LE)
body    := u8 version | u8 kind | u64 lsn | u64 tx_id | payload
version := 6   (el lector acepta 5 y 4 para migrar bases 0.5/0.4)
```

`lsn` es un contador monótono global (cada frame consume uno). `tx_id`
identifica la transacción dentro del WAL.

### Kinds y payloads

| Kind | Nombre | Payload |
|-----:|--------|---------|
| 1 | PUT | string tabla + row |
| 2 | DEL | string tabla + string clave |
| 3 | COMMIT | (vacío) |
| 4 | SCHEMA | schema (ver abajo) |
| 5 | DROP | string tabla |
| 6 | BASE | u64 checkpoint_lsn (marcador informativo: primer frame tras un rebase) |
| 7 | RESCH | schema (re-schema en vivo: crear/borrar índice; sustituye el def de la tabla) |

**Schema** (payload de SCHEMA/RESCH): `string nombre + string pk + u8 schemaless
+ u32 ncols + por columna: string nombre + u8 tag_tipo + u8 nullable + u8 fts`.
Sección 0.6 (trailing, ausente en payloads v5/v4): `u8 marcador=1 + u32 nindexes
+ nindexes × string nombre_columna`.

### Reglas del recovery

1. Cargar `data.pages` si existe → checkpoint_lsn = C.
2. Cargar `delta.pages` si existe: si `watermark > C`, aplicar sus registros
   y el nuevo punto de partida pasa a ser `watermark` (sección 4.6).
3. Frames con `lsn <=` punto de partida se ignoran (ya están persistidos).
4. Se acumulan las operaciones de la transacción activa; COMMIT aplica.
5. Si llega un frame de una tx **distinta** a la activa, la activa se
   descarta (fue abandonada por un crash a mitad de transacción).
6. Cola corrupta (magic malo, CRC malo, frame cortado): se deja de leer y el
   motor **trunca** el archivo al último byte válido. Nunca se sirve un dato
   sin verificar.

### Durabilidad

- Modo no durable: `write()` por commit (sobrevive a crash del proceso).
- Modo durable: `fsync()` del WAL en cada COMMIT (sobrevive a corte de luz).

## 3. Snapshot (`data.pages`)

Archivo de **páginas fijas de 4096 bytes**. Escrito siempre a un `.tmp`,
`fsync`, renombrado (atómico) y con `fsync` del directorio. Tras el renombrado
el WAL se **rebasea** (se trunca y se escribe un frame BASE), por eso el
recovery es O(filas vivas) y no O(historial).

### 3.1 Página 0 — header del archivo

```
offset 0 : u32 magic = 0x36504E47 ("GNP6"; el lector también acepta 0x35504E47 "GNP5")
offset 4 : u8  version = 6   (el lector acepta 5)
offset 5 : u8  flags (0)
offset 8 : u64 checkpoint_lsn   (LSN cubierto por este snapshot)
offset 16: u64 page_count       (páginas totales del archivo)
offset 24: u32 table_count
offset 28: u32 reservado
offset 32: u64 free_list_head (0 = vacía; reservado para el page manager online)
resto    : ceros hasta 4096
```

Los ids de tabla (1..table_count, orden alfabético de nombre) son estables
dentro de un snapshot y se recalculan en cada checkpoint completo.

### 3.2 Páginas slotted (datos y catálogo)

```
offset 0 : u32 magic = 0x47475047 ("GPGN")
offset 4 : u32 page_id
offset 8 : u64 page_lsn
offset 16: u16 slot_count
offset 18: u16 data_end     (área de datos = [32, 32+data_end))
offset 20: u16 slots_end    (área de slots  = [4096-slots_end, 4096))
offset 22: u16 flags        (0 = datos, 2 = catálogo)
offset 24: u32 crc32(bytes [32,4096))
offset 28: u32 reservado
offset 32: área de datos (crece hacia arriba)
          ... espacio libre ...
final    : slots (crecen hacia abajo desde 4096)
```

Cada **slot** (4 bytes, se leen desde el final): `u16 offset_desde_32 | u16 len`.

Cada **registro** dentro de la página:

```
u32 table_id
u8  layout   (0 = inline, 1 = indirecto)
layout 0: row bytes a continuación
layout 1: u64 total_len + u64 first_overflow_page
```

Los registros que no caben en una página (blobs grandes) se guardan en una
cadena de páginas de overflow y el slot contiene solo el descriptor indirecto.

### 3.3 Páginas de catálogo (flags = 2)

Mismas páginas slotted; cada registro es `u32 table_id + SCHEMA payload`
(el mismo formato que el kind 4 del WAL). **Los ids de tabla se asignan en
orden alfabético de nombre, 1..N**, al momento del checkpoint.

### 3.4 Páginas de overflow

```
offset 0 : u32 magic = 0x564F4E47 ("GNOV")
offset 4 : u32 page_id
offset 8 : u32 chunk_len
offset 12: u32 reservado
offset 16: u64 next_page (0 = fin de la cadena)
offset 24: chunk de datos (máx 4072 bytes)
```

El lector concatena los chunks hasta alcanzar `total_len`; valida magics y
detecta ciclos.

## 4. Checkpoint incremental (`delta.pages`) — NUEVO 0.6

Archivo de páginas fijas de 4096 bytes con las **versiones de los registros
cambiados desde el último checkpoint completo** (el "base" de la sección 3).
El motor escribe aquí en cada checkpoint incremental (IC); el checkpoint
completo (FC, `compact()`) reescribe `data.pages` y reinicia este archivo.

### 4.1 Páginas 0 y 1 — header de doble slot

Dos slots idénticos en formato, escritos alternadamente. El slot válido con
`seq` mayor gana. El "flip" del slot (pwrite + fsync) **es el punto de commit**
de un IC: las páginas escritas antes con `page_lsn > watermark` del slot
vigente son de un IC que no llegó a comprometerse y se ignoran.

```
offset 0 : u32 magic = 0x4C444E47 ("GNDL" LE)
offset 4 : u8  version = 6
offset 5 : u8  slot (0 o 1 — informativo)
offset 6 : u16 reservado
offset 8 : u64 seq              (monótono entre slots)
offset 16: u64 watermark_lsn    (estado cubierto por este IC)
offset 24: u64 base_lsn         (lsn del base al que aplica — informativo)
offset 32: u64 page_count       (páginas totales del archivo; normativo)
offset 40: u64 live_pages       (informativo)
offset 48: u64 free_pages       (informativo)
offset 56: u32 reservado
offset 60: u32 crc32(bytes [0,60))
resto    : ceros hasta 4096
```

Regla de activación: el delta aplica solo si `watermark_lsn > checkpoint_lsn`
del base; si es menor o igual (p. ej. crash a mitad de un FC), se descarta
por completo. La creación del archivo escribe AMBOS slots seq=0 antes de
cualquier página de datos.

### 4.2 Páginas de datos (desde la página 2)

Páginas slotted idénticas a las del base (misma cabecera de 32 bytes, mismos
slots desde el final) con `flags = 6` y `page_lsn = watermark` del IC que las
escribió. Cada registro:

```
u32 table_id
u8  layout
layout 0 (L_ROW)  : row bytes (fila inline)
layout 1 (L_OVF)  : u64 total_len + u64 first_overflow_page (igual que el base)
layout 2 (L_TOMB) : string clave          (la fila fue borrada desde el base)
layout 3 (L_SCHEMA): schema payload        (tabla creada o re-schema — mismo
                                             formato que SCHEMA del WAL, con
                                             table_id = id de la tabla)
layout 4 (L_DROP) : string nombre de tabla (table_id = id que tenía)
```

Las páginas de overflow son idénticas a las del base ("GNOV").

Los `table_id` de tablas nuevas (no presentes en el base) los asigna el motor
en orden creciente y nunca se reutilizan entre encarnaciones de tabla.

### 4.3 Free-list persistente derivada

La free-list NO se persiste como estado redundante: se **deriva** del
contenido al abrir. Una página está libre si y solo si:

1. es una página de datos sin ningún registro **vivo** (un registro está vivo
   si es la última versión de su (table_id, clave) y su tabla no fue borrada
   después), o
2. es una página de overflow no referenciada por ningún registro vivo, o
3. es ilegible (magic/CRC malos: hueco "torn" de un IC sin flip o corrupción
   de medio; se contabiliza como `suspect_pages` en `stats`).

Esto hace la free-list **infalsificable**: no hay un segundo estado que
pueda corromperse, y los huecos liberados se reutilizan (el asignador toma
primero la página libre de menor id, después extiende al final del archivo).

### 4.4 Copy-forward y supersession

En cada IC, el motor decide por página: si ≥50% de sus registros está muerto
(superido por el lote nuevo, o su tabla fue borrada), reemite los vivos en el
lote nuevo y libera la página; si está 100% muerta solo la libera. Un registro
reemitido conserva su contenido (el descriptor L_OVF puede apuntar a la misma
cadena de overflow). Las cadenas de registros liberados quedan libres.

### 4.5 Compactación (FC)

`compact()` reescribe `data.pages` completo (sección 3), recalcula los ids de
tabla por orden alfabético, recrea `delta.pages` vacío (watermark = lsn del
nuevo base, seq creciente) y rebasea el WAL. Umbral automático: FC cuando
las páginas vivas del delta superan `max(base_pages/4, 64)`.

### 4.6 Recovery con delta

```
estado := cargar base (sección 3)          → checkpoint_lsn = C
si existe delta y watermark > C:
    estado := aplicar registros del delta en orden (lsn, página)
              (L_SCHEMA/L_DROP/L_ROW/L_OVF/L_TOMB)
    C := watermark
replay del WAL desde C (sección 2)
```

Un `L_ROW` necesita la PK de su tabla: se resuelve con el schema del propio
delta (L_SCHEMA por table_id) o, si la tabla viene del base, con el catálogo
del base (id → def). La PK es inmutable por encarnación de tabla.

## 5. Compatibilidad 0.4 / 0.5

- El WAL 0.5 (version 5) y 0.4 (version 4) se aceptan al abrir. SCHEMA v4 no
  tiene `schemaless`/`fts` ni índices; las claves 0.4 se resuelven con
  fallback a `id`.
- `data.pages` 0.5 (magic `GNP5`, version 5) se lee directamente; el primer
  checkpoint 0.6 lo convierte a v6. Los schemas v5 simplemente no tienen
  índices.
- El checkpoint 0.4 (magic `GNIP` 0x50494E47) se ignora: en 0.4 se escribía
  pero nunca se leía, todo vivía en el WAL.
- Una base 0.5 abierta por 0.6 funciona sin migración; a la inversa no (0.5
  no conoce delta.pages ni la version 6 — la ignoraría de forma insegura).

## 6. Checksums

CRC32 IEEE (polinomio 0xEDB88320, init 0xFFFFFFFF, xor final 0xFFFFFFFF,
sin reflexión adicional) — idéntico a zlib. Vector de prueba:
`crc32("123456789") = 0xCBF43926`.

---

## 8. Cambios 0.7 — SIN cambios de formato + clasificación de corrupción

**El formato de archivo NO cambia en 0.7**: `data.pages`, `delta.pages`,
`wal.log` y los frames de fila v6 (pares nombre-valor) son exactamente los
de 0.6. La fila plana (`SRow`) es una representación INTERNA del motor; en
disco los nombres de columna siguen viajando con cada fila, lo que mantiene
el camino de lectura 0.5 → 0.6 → 0.7 y la validez del lector Zig sin
modificar su parser de filas.

### 8.1 Clasificación de páginas ilegibles de delta.pages (semántica nueva)

0.6 contaba toda página ilegible como `suspect` (hueco) sin distinguir
corrupción de medio de torn normal. 0.7 clasifica dentro del extent
comprometido `[2, page_count)` del slot válido:

| Condición | Contador | Significado |
|---|---|---|
| magic "GPGN" legible, flags/crc mal, `page_lsn > watermark` | `delta_suspect_pages` | torn de un IC sin flip (reutilización de hueco libre): **normal**, la base está sana |
| cualquier otra página ilegible | `delta_corrupt_pages` | rotura de **medio** dentro del estado comprometido: pérdida real de registros — **detectada y reportada** |
| registro L_OVF cuya cadena overflow está rota (página "GNOV" ausente/ilegible, longitud inconsistente) | `delta_broken_ovf_records` | el registro se pierde **pero la base abre** y sigue operativa (0.6 lanzaba Corruption) |

Los tres contadores viven en `Stats` (`gina_stats` en el C ABI, JSON
incluido), en el cliente TypeScript (`stats()`) y en `gina-inspect`
(`suspect=… corrupt=… broken_ovf=…`). El lector Zig implementa la misma
clasificación — paridad verificada sobre bases dañadas deliberadamente
(escenarios 16 y 17 de la crash suite).

Notas honestas: los registros de una página `corrupt` se pierden sin
posibilidad de recuperación (el WAL ya está rebaseado tras el IC — el
contenido vive solo ahí); una página corrupta se convierte en hueco
reutilizable y, tras reescribirse, el contador vuelve a reflejar el estado
actual del archivo. Las páginas overflow siguen **sin CRC propio**: una
cadena íntegra con payload dañado pasa inadvertida (limitación conocida,
roadmap: CRC en páginas GNOV como v7).
