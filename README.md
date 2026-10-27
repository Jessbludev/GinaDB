# GinaDB Lite 0.7

Base de datos **embebida, pequeña y multimodal** en C++20 (con parser de
lectura puro y FFI en Zig 0.16, y cliente TypeScript para Bun/Node). Sin
dependencias externas.

**Visión**: lite, embebida, utilidades mínimas, sintaxis nueva, type-safe,
compilada, WAL óptimo, transacciones, real-time, híbrida
relacional/documental/vectorial — y usable desde cualquier lenguaje.

**Nuevo 0.7**: **filas planas internas** + decode paralelo + CRC32
slice-by-8 + FTS perezoso → **reapertura ×5,4 más rápida** (1237→228 ms
@100K filas); **integridad del delta**: corrupción de medio **clasificada y
reportada** (`corrupt` vs `suspect`), cadenas overflow rotas ya no tumban la
base; KNN con heap top-k y abandono temprano L2 (+22%). **Misma API, mismo
ABI, mismo formato en disco** — 0.7 abre bases 0.5/0.6 y los clientes
(TS/Python/Zig) no cambian.

**De 0.6**: índice primario **B+Tree**, **índices secundarios** persistentes
con planner de consultas, **checkpoint incremental base+delta** con free-list
persistente derivada, `gina_select` (JSON, con pushdown a índices), cliente
**TypeScript** tipado (bun:ffi nativo, koffi en Node) y lector Zig de
`delta.pages`.

```cpp
gina::Database db{"mi-app"};

db["users"].create({.primary_key = "id",
                    .columns{{.name="id",  .type=gina::T::Text, .nullable=false},
                             {.name="email", .type=gina::T::Text},
                             {.name="age", .type=gina::T::Int},
                             {.name="bio", .type=gina::T::Text, .fts=true}},
                    .indexes{"email", "age"}});   // 0.6: índices secundarios

// CRUD con sintaxis nueva: pares "clave", valor variádicos
db["users"].emplace("id","u1", "email","gina@db.io", "age",34,
                    "bio","creadora de bases de datos");
db["users"].patch("u1", "age", 35);                 // update parcial
auto u = db["users"].get("u1");                      // optional<Row>
bool ok = db["users"].erase("u1");                   // true si existía

// 0.6: condiciones TIPADAS — el planner las empuja a los índices
auto ada  = db["users"].where(gina::eq("email", "gina@db.io")).collect();
auto jovenes = db["users"].where(gina::between("age", 18, 29))
                          .order_by("age", true).limit(10).collect();
auto pagina = db["users"].where(gina::ge("id", "u900"))
                        .order_by("id", true).limit(10).collect(); // rangos PK

// queries con lambda libre (recorrido completo) siguen disponibles
auto top = db["users"].where([](auto& r){ return r["age"].as_int() >= 18; })
                       .order_by("age", /*desc=*/true).limit(10).collect();

// 0.6: índices dinámicos, atómicos y persistentes
db["users"].create_index("city");
db["users"].drop_index("city");

// multimodal: vectores + KNN
db["media"].emplace("id","m1", "tag","gina", "vec", gina::Vec{0.1f,0.2f,0.3f});
auto vecinos = db["media"].knn("vec", {0.1f,0.2f,0.3f}, /*k=*/5);

// multimodal: búsqueda de texto (índice invertido en columnas .fts)
auto hits = db["users"].search("bio", "base datos");

// transacciones atómicas
auto tx = db.begin();
tx["users"].emplace("id","u9", "name","Zed", "age",50, "bio","nuevo");
tx["users"].erase("u1");
tx.commit();   // rollback automático si no se llama

// real-time: watch + changelog con cursor
auto sub = db["users"].watch([](const gina::Event& ev){ /* ... */ });
auto cambios = db["users"].changes(cursor);

// 0.6: checkpoints incrementales y compactación
db.checkpoint();   // escribe SOLO lo cambiado (delta.pages)
db.compact();      // VACUUM: reescribe data.pages y reinicia el delta
```

## Qué hay dentro

| Componente | Detalle |
|---|---|
| **Storage** | WAL con CRC32 + LSN + batch por transacción; base en **páginas slotted de 4 KiB** con overflow para blobs; **0.6: checkpoint incremental `delta.pages`** (doble slot atómico, copy-forward, free-list derivada) + compactación automática y manual | 
| **Recovery** | Startup = base + delta + reproducir solo el tramo de WAL posterior; reparación automática de cola corrupta (torn write / CRC malo); ignora IC sin comprometer |
| **Índices 0.6** | **B+Tree primario agrupado** (hojas enlazadas: orden y rangos PK sin ordenar, bulk-load desde base) + **índices secundarios ordenados** por columna (int/float ordenable con signo, texto, bytes, bool) con `create_index`/`drop_index` atómicos y persistentes |
| **Planner 0.6** | Condiciones tipadas `eq/ne/gt/ge/lt/le/between/in/and/or` con pushdown: PK punto > PK rango > índice punto > índice rango > recorrido; `order_by` por PK o por columna indexada usa el orden del árbol con parada temprana en `limit` |
| **Multimodal** | Valores: null, bool, int64, float, texto, bytes, array, objeto anidado y **vector (embedding f32)**; KNN por coseno o L2; full-text con índice invertido; blobs de hasta 16 MiB |
| **Type-safe** | Schemas estrictas validadas en inserción y en commit; widenings explícitos (int→float); PK inmutable; tablas `schemaless` para modo documento híbrido |
| **Real-time** | `watch()` con callbacks por tabla + changelog consultable con cursor y detección de huecos |
| **Concurrencia** | Lectores concurrentes (shared locks por tabla) + un escritor serializado (estilo SQLite) |
| **Interoperabilidad** | `libginadb.so` con **C ABI** (28 funciones, JSON; `gina_select` con planner), CLI `gina` (subprocess amigable), clientes **Python (ctypes)** y **TypeScript** (bun:ffi/koffi, tipado completo) incluidos, **parser de lectura puro en Zig 0.16** (lee base + delta + WAL sin el motor) + FFI Zig |

## Compilar

```bash
make            # libginadb.a + libginadb.so + herramientas + CLI
make test       # 228 comprobaciones (btree + unit + crash/recovery)
make bench      # benchmark
make zig        # parser/inspector/FFI Zig (requiere zig 0.16.0)
cd examples/typescript && bun test   # suite funcional TypeScript (11 tests)
```

Con CMake (>= 3.20):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j && cd build && ctest
```

Requisitos: compilador C++20 (probado con g++ 14.2) y pthreads. Zig es
opcional (solo para el parser/inspector).

## Usar desde otros lenguajes

**Cualquier lenguaje, vía subprocess** (sin linking):

```bash
./gina midb put users '{"id":"u1","name":"Gina"}'
./gina midb get users u1
./gina midb select users '{"where":{"op":"eq","col":"email","val":"u1@x.io"}}'
./gina midb knn media vec 0.1,0.2,0.3 5
```

**TypeScript (Bun con bun:ffi, Node con koffi)** — ver `examples/typescript`:

```ts
import { GinaDB, vec } from "./src/client.ts";

const db = await GinaDB.open("midb");
db.createTable("users", {
  primary_key: "id",
  columns: [{ name: "id", type: "text" }, { name: "email", type: "text" },
            { name: "age", type: "int" }],
  indexes: ["email", "age"],
});
db.insert("users", { id: "u1", email: "gina@db.io", age: 34 });
const hit = db.select("users",
  { where: { op: "eq", col: "email", val: "gina@db.io" } });
db.insert("media", { id: "m1", vec: vec([0.1, 0.2, 0.3]) });
const vecinos = db.knn("media", "vec", [0.1, 0.2, 0.3], 5);
db.close();
```

```bash
cd examples/typescript
bun  src/workload.ts 20000 5000 5000   # workload real de datos (bun)
node src/workload.ts 20000 5000 5000   # el mismo en node
```

**Python (ctypes sobre la C ABI)** — ver `examples/python/gina_client.py`:

```python
from gina_client import GinaDB, Vec
with GinaDB("midb") as db:
    db["users"].create(columns={"id": "text", "email": "text"},
                       pk="id", indexes=["email"])
    db["users"].insert({"id": "u1", "email": "gina@db.io"})
    print(db["users"].select({"where": {"op": "eq", "col": "email",
                                      "val": "gina@db.io"}}))
```

```bash
cd examples/python && python3 verify06.py   # 24 comprobaciones + crash SIGKILL
```

**Cualquier lenguaje, leyendo los archivos directamente**: el formato binario
está especificado byte a byte en `docs/FORMATO_ARCHIVOS.md` (WAL, base y delta)
y `zig/ginaread.zig` es la implementación de referencia (exporta una C ABI
propia de solo lectura).

## Herramientas

| Comando | Uso |
|---|---|
| `./demo` | Tour completo de la API (CRUD, KNN, FTS, tx, watch, checkpoint) |
| `./gina <dir> <cmd>` | CLI JSON sobre la C ABI (`select`, `index`, `compact` incluidos) |
| `./ginadump <dir>` | Inspector de almacenamiento |
| `zig/zig-out/bin/gina-inspect <dir>` | Inspector offline en Zig puro (lee base + delta + WAL sin el motor) |

## Documentación

- `docs/ARQUITECTURA.md` — componentes, flujo de commit/checkpoint/recovery, locking
- `docs/FORMATO_ARCHIVOS.md` — especificación binaria exacta (WAL, base, delta, free-list)
- `docs/BENCHMARKS.md` — metodología y comparación 0.5 → 0.6
- `docs/INFORME-0.6.md` — informe de ingeniería de esta iteración: mediciones,
  cuellos de botella encontrados, optimizaciones aplicadas y trabajo futuro
- `VERIFICACION.md` — verificación, hallazgos y correcciones de esta iteración
- `CHANGELOG.md`

## Límites actuales (por diseño "lite")

- Un proceso por base de datos (sin bloqueo entre procesos)
- Un escritor serializado; lectores concurrentes
- KNN por fuerza bruta O(n·d) (sin HNSW/IVF); FTS sin stemming
- El changelog y los watchers son por proceso (no persisten entre reinicios)
- B+Tree e índices secundarios: siguiente iteración (0.6) — ver `VERIFICACION.md`

## Licencia

MIT (ver el encabezado de cada archivo).
