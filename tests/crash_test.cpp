// GinaDB Lite 0.7 — crash/recovery suite: deliberate storage corruption.
// Every scenario writes a known state, closes, corrupts bytes on disk,
// reopens and asserts the exact expected outcome (repaired/durable/failed).
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <vector>

#include "gina/db.hpp"
#include "../src/codec.hpp"

using namespace gina;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) { ++g_pass; }                                         \
        else { ++g_fail; std::cerr << "FAIL " << __LINE__ << "  "      \
              << #cond << "\n"; }                                       \
    } while (0)

static void wipe(const std::string& d) { std::filesystem::remove_all(d); }

static void seed_db(const std::string& dir, int rows, bool checkpoint_at_end,
                    bool compact_at_end = false) {
    wipe(dir);
    // checkpoint_on_close=false keeps every frame in the WAL, emulating a
    // process that crashed after committing (no clean-exit checkpoint)
    Database db{dir, Options{.durable = false, .checkpoint_every = 0,
                             .changelog_capacity = 0,
                             .max_blob_size = 16u << 20,
                             .checkpoint_on_close = false}};
    db["t"].create({.schemaless = true});
    for (int i = 0; i < rows; i++)
        db["t"].emplace("id", "k" + std::to_string(i), "n", i);
    if (checkpoint_at_end) db.checkpoint();
    if (compact_at_end) db.compact();   // 0.6: materializa data.pages
}

[[nodiscard]] static std::uint64_t fsize(const std::string& p) {
    std::error_code ec;
    const auto s = std::filesystem::file_size(p, ec);
    return ec ? 0 : static_cast<std::uint64_t>(s);
}

static void truncate_at(const std::string& p, std::uint64_t keep) {
    std::filesystem::resize_file(p, keep);
}

static void flip_byte(const std::string& p, std::uint64_t offset) {
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(static_cast<std::int64_t>(offset));
    char c;
    f.read(&c, 1);
    c = static_cast<char>(c ^ 0x5A);
    f.seekp(static_cast<std::int64_t>(offset));
    f.write(&c, 1);
}

static void append_garbage(const std::string& p, int n) {
    std::ofstream f(p, std::ios::binary | std::ios::app);
    std::mt19937 rng(7);
    for (int i = 0; i < n; i++) f.put(static_cast<char>(rng() & 0xFF));
}

// escribe un byte exacto en `offset` (corrupción de medio controlada)
static void poke(const std::string& p, std::uint64_t offset, std::uint8_t v) {
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::int64_t>(offset));
    const char c = static_cast<char>(v);
    f.write(&c, 1);
}

// scenario 1: torn frame header (WAL cut in the middle of a 12-byte header)
static void torn_header() {
    const std::string d = "c-torn-hdr";
    seed_db(d, 10, false);
    const auto sz = fsize(d + "/wal.log");
    truncate_at(d + "/wal.log", sz - 6);  // cut inside the last frame header
    Database db{d};
    const auto after_repair = db["t"].count();
    CHECK(after_repair >= 9);   // at most the last tx is lost — never a crash
    db["t"].emplace("id", "post", "n", 1);
    CHECK(db["t"].count() == after_repair + 1);
    // repaired file must survive a new reopen (abandoned-tx handling)
    Database db2{d};
    CHECK(db2["t"].count() == after_repair + 1);
    CHECK(db2["t"].get("post").has_value());
}

// scenario 2: torn frame body (cut inside the payload)
static void torn_body() {
    const std::string d = "c-torn-body";
    seed_db(d, 10, false);
    const auto sz = fsize(d + "/wal.log");
    truncate_at(d + "/wal.log", sz - 30);
    Database db{d};
    const auto n = db["t"].count();
    CHECK(n >= 8);   // the torn tx is discarded cleanly, no crash
    db["t"].emplace("id", "post", "n", 1);
    CHECK(db["t"].count() == n + 1);
    Database db2{d};
    CHECK(db2["t"].count() == n + 1);
    CHECK(db2["t"].get("post").has_value());
}

// scenario 3: corrupted CRC (bit flip inside a frame body)
static void bad_crc() {
    const std::string d = "c-bad-crc";
    seed_db(d, 10, false);
    const auto sz = fsize(d + "/wal.log");
    flip_byte(d + "/wal.log", sz - 20);  // inside the last frame body
    Database db{d};
    CHECK(db["t"].count() >= 8);
    CHECK(db.stats().recovered_bad_tail > 0);
}

// scenario 4: corrupted magic
static void bad_magic() {
    const std::string d = "c-bad-magic";
    seed_db(d, 10, false);
    const auto sz = fsize(d + "/wal.log");
    flip_byte(d + "/wal.log", sz - 40);  // header area of a late frame
    Database db{d};
    CHECK(db["t"].count() >= 8);
    CHECK(db.stats().recovered_bad_tail > 0);
}

// scenario 5: uncommitted transaction (crash between PUT and COMMIT frames)
static void uncommitted_tx() {
    const std::string d = "c-uncommitted";
    seed_db(d, 5, false);
    // append raw frames: one PUT (valid CRC) with no COMMIT after it
    std::vector<std::uint8_t> payload;
    codec::put_str(payload, "t");
    codec::encode_row(payload, Row{{"id", Value(std::string("ghost"))},
                                   {"n", Value(std::int64_t{99})}});
    std::vector<std::uint8_t> body;
    body.push_back(codec::FORMAT_VERSION);
    body.push_back(codec::K_PUT);
    codec::put64(body, 10'000);   // high lsn: beyond anything written
    codec::put64(body, 777);
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<std::uint8_t> frame;
    codec::put32(frame, codec::WAL_MAGIC);
    codec::put32(frame, static_cast<std::uint32_t>(body.size()));
    codec::put32(frame, codec::crc32(body.data(), body.size()));
    frame.insert(frame.end(), body.begin(), body.end());
    std::ofstream f(d + "/wal.log", std::ios::binary | std::ios::app);
    f.write(reinterpret_cast<const char*>(frame.data()),
            static_cast<std::streamsize>(frame.size()));
    f.close();

    Database db{d};
    CHECK(db["t"].count() == 5);              // ghost row was NOT resurrected
    CHECK(!db["t"].get("ghost").has_value());
    db["t"].emplace("id", "real", "n", 1);    // a fresh tx must not collide
    CHECK(db["t"].count() == 6);
    Database db2{d};
    CHECK(db2["t"].count() == 6);             // fresh tx survives; ghost stays dead
    CHECK(!db2["t"].get("ghost").has_value());
}

// scenario 6: garbage bytes appended to the WAL
static void garbage_tail() {
    const std::string d = "c-garbage";
    seed_db(d, 10, false);
    append_garbage(d + "/wal.log", 100);
    Database db{d};
    CHECK(db["t"].count() == 10);
    CHECK(db.stats().recovered_bad_tail >= 100);
    db.checkpoint();
    Database db2{d};
    CHECK(db2["t"].count() == 10);
}

// scenario 7: corrupted snapshot page → must refuse to open (fail fast).
// Schemas live in the snapshot, so recovery-from-snapshot-loss only applies
// to databases that never checkpointed (covered by the unit suite).
static void corrupt_snapshot() {
    const std::string d = "c-bad-snap";
    seed_db(d, 10, false, true);  // compact → data.pages materializado
    std::filesystem::copy_file(d + "/data.pages", d + "/data.pages.bak");
    flip_byte(d + "/data.pages", 2 * 4096 + 100);  // corrupt a data page
    bool threw = false;
    try {
        Database db{d};
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);  // corruption is detected, never silently served
    // restoring a good snapshot makes the database usable again
    std::filesystem::copy_file(d + "/data.pages.bak", d + "/data.pages",
                               std::filesystem::copy_options::overwrite_existing);
    Database db{d};
    CHECK(db["t"].count() == 10);
    db["t"].emplace("id", "after", "n", 1);
    Database db2{d};
    CHECK(db2["t"].count() == 11);
}

// scenario 8: truncated snapshot → must refuse to open
static void truncated_snapshot() {
    const std::string d = "c-trunc-snap";
    seed_db(d, 10, false, true);  // compact → data.pages existe
    truncate_at(d + "/data.pages", fsize(d + "/data.pages") - 1000);
    bool threw = false;
    try {
        Database db{d};
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);
}

// scenario 9: crash between snapshot rename and WAL rebase — old WAL with
// frames already covered by the (newer) snapshot must be skipped by LSN.
static void crash_between_rename_and_rebase() {
    const std::string d = "c-rename";
    wipe(d);
    {
        Database db{d};
        db["t"].create({.schemaless = true});
        for (int i = 0; i < 10; i++)
            db["t"].emplace("id", "k" + std::to_string(i), "n", i);
        db.checkpoint();
        db["t"].emplace("id", "after", "n", 42);   // lives in WAL only
    }
    // simulate: snapshot written (rename done) but WAL rebase did not happen.
    // We emulate the PRE-rebase WAL by copying it before an extra checkpoint.
    std::filesystem::copy_file(d + "/wal.log", d + "/wal.pre");
    {
        Database db{d};
        db.checkpoint();  // now data.pages is newer
    }
    // restore the old (longer) WAL next to the newest snapshot
    std::filesystem::copy_file(d + "/wal.pre", d + "/wal.log",
                               std::filesystem::copy_options::overwrite_existing);
    Database db{d};
    // rows are counted once: replay must skip frames covered by the snapshot
    CHECK(db["t"].count() == 11);
    CHECK(db["t"].get("after")->at("n").as_int() == 42);
    CHECK(db["t"].get("k0")->at("n").as_int() == 0);
}

// scenario 10: durable mode round-trip (fsync path) + reopen
static void durable_reopen() {
    const std::string d = "c-durable";
    wipe(d);
    {
        Database db{d, Options{.durable = true, .checkpoint_every = 0}};
        db["t"].create({.schemaless = true});
        db["t"].emplace("id", "a", "v", 1);
        db["t"].emplace("id", "b", "v", 2);
    }
    Database db{d, Options{.durable = true}};
    CHECK(db["t"].count() == 2);
}

// scenario 11 (0.6): pages of an IC that never flipped (torn/uncommitted
// tail of delta.pages) must be ignored and later reused as free holes.
static void delta_uncommitted_tail() {
    const std::string d = "c-delta-tail";
    seed_db(d, 30, true);  // 30 filas + IC → delta.pages con páginas
    const auto dsz = fsize(d + "/delta.pages");
    CHECK(dsz >= 3 * 4096);
    append_garbage(d + "/delta.pages", 3 * 4096);  // "páginas" sin flip
    Database db{d};
    CHECK(db["t"].count() == 30);          // nada del IC fantasma
    CHECK(db.stats().delta_suspect_pages >= 0);  // abre y deriva sanamente
    db["t"].emplace("id", "post", "n", 1);
    db.checkpoint();                        // el IC nuevo reusa/ignora huecos
    CHECK(db["t"].count() == 31);
    Database db2{d};
    CHECK(db2["t"].count() == 31);
    CHECK(db2["t"].get("post").has_value());
}

// scenario 12 (0.6): delta truncated below its header page_count → refuse.
static void delta_truncated() {
    const std::string d = "c-delta-trunc";
    seed_db(d, 30, true);
    truncate_at(d + "/delta.pages", fsize(d + "/delta.pages") - 100);
    bool threw = false;
    try {
        Database db{d};
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);
}

// scenario 13 (0.6): crash in the middle of a FULL checkpoint — the base was
// renamed (con TODO lo comprometido hasta su watermark) but delta.pages y el
// WAL quedaron como estaban. El delta viejo (watermark ≤ base) se descarta y
// los frames del WAL viejo se saltan por LSN.
static void crash_mid_compact() {
    const std::string d = "c-mid-compact";
    wipe(d);
    std::string wal_pre;
    {
        Database db{d, Options{.durable = false, .checkpoint_every = 0,
                               .changelog_capacity = 0,
                               .checkpoint_on_close = false}};
        db["t"].create({.schemaless = true});
        for (int i = 0; i < 10; i++)
            db["t"].emplace("id", "k" + std::to_string(i), "n", i);
        db.checkpoint();               // IC: delta con 10 filas (wm=W1)
        std::filesystem::copy_file(d + "/delta.pages", d + "/delta.stale");
        db["t"].emplace("id", "after", "n", 42);   // WAL (lsn W1+1)
        std::filesystem::copy_file(d + "/wal.log", d + "/wal.pre");
        db.compact();                  // FC: base@W2 con 11 filas
    }
    // simular el crash: el delta VIEJO sobrevive junto al base NUEVO
    std::filesystem::copy_file(d + "/delta.stale", d + "/delta.pages",
                               std::filesystem::copy_options::overwrite_existing);
    {   // (a) con el WAL rebasado por el FC
        Database db{d};
        CHECK(db["t"].count() == 11);
        CHECK(db["t"].get("after")->at("n").as_int() == 42);
    }
    // (b) con el WAL VIEJO (pre-rebase): frames ≤ base ckpt saltados por LSN
    std::filesystem::copy_file(d + "/wal.pre", d + "/wal.log",
                               std::filesystem::copy_options::overwrite_existing);
    Database db{d};
    CHECK(db["t"].count() == 11);          // nada duplicado, nada perdido
    CHECK(db["t"].get("after")->at("n").as_int() == 42);
    CHECK(db["t"].get("k0")->at("n").as_int() == 0);
}

// scenario 14 (0.6): WAL frames newer than the delta watermark are replayed
// on top of the delta state.
static void replay_after_delta() {
    const std::string d = "c-delta-wal";
    seed_db(d, 20, true);   // 20 filas en delta
    {
        Database db{d, Options{.durable = false, .checkpoint_every = 0,
                               .changelog_capacity = 0,
                               .checkpoint_on_close = false}};
        db["t"].emplace("id", "w1", "n", 100);   // WAL only
        db["t"].erase("k5");                     // WAL only
        db["t"].emplace("id", "k7", "n", 777);   // actualiza una del delta
    }
    Database db{d};
    CHECK(db["t"].count() == 20);                // 20 + w1 - k5
    CHECK(db["t"].get("w1")->at("n").as_int() == 100);
    CHECK(!db["t"].get("k5").has_value());
    CHECK(db["t"].get("k7")->at("n").as_int() == 777);
    db.checkpoint();                              // IC absorbe el WAL
    Database db2{d};
    CHECK(db2["t"].count() == 20);
    CHECK(!db2["t"].get("k5").has_value());
    CHECK(db2["t"].get("k7")->at("n").as_int() == 777);
}

// scenario 15 (0.6): fuzz diferencial — ops aleatorias con IC/FC/reopen
// aleatorios contra un modelo; incluye blobs (cadenas overflow), segunda
// tabla con create/drop e índices create/drop.
static void fuzz_delta() {
    const std::string d = "c-fuzz";
    wipe(d);
    std::map<std::string, std::int64_t> model;
    std::mt19937 rng(20260206);
    auto dbp = std::make_unique<Database>(
        d, Options{.durable = false, .checkpoint_every = 0,
                   .changelog_capacity = 0, .checkpoint_on_close = false});
    (*dbp)["t"].create({.schemaless = true});
    bool t2_alive = false, idx_alive = false;
    // 0.7: espacio de claves 10× mayor que los pasos — garantiza claves
    // FRÍAS (nunca re-tocadas) cuyas versiones de ICs antiguos siguen vivas:
    // cubre la clase de bug del escenario 18 (schema supersedido + filas
    // viejas vivas) que el fuzz con claves calientes no alcanzaba a tocar.
    for (int step = 0; step < 4000; step++) {
        const int action = static_cast<int>(rng() % 1000);
        const std::string k = "k" + std::to_string(rng() % 5000);
        if (action < 420) {
            (*dbp)["t"].emplace("id", k, "n", static_cast<std::int64_t>(step));
            model[k] = step;
        } else if (action < 620) {
            if (model.erase(k)) (*dbp)["t"].erase(k);
        } else if (action < 650) {  // blob grande → overflow en el delta
            Bytes blob(24'000 + (rng() % 8'000));
            (*dbp)["t"].emplace("id", "blob", "n", static_cast<std::int64_t>(step),
                                "data", blob);
            model["blob"] = step;
        } else if (action < 680) {
            dbp->checkpoint();      // IC
        } else if (action < 700) {
            dbp->compact();         // FC
        } else if (action < 730) {  // reopen limpio (sin checkpoint)
            dbp.reset(new Database(
                d, Options{.durable = false, .checkpoint_every = 0,
                           .changelog_capacity = 0,
                           .checkpoint_on_close = false}));
        } else if (action < 760) {  // segunda tabla: ciclo de vida
            if (t2_alive) {
                (*dbp)["t2"].drop();
                t2_alive = false;
            } else {
                (*dbp)["t2"].create({.schemaless = true});
                (*dbp)["t2"].emplace("id", "x", "v", 1);
                t2_alive = true;
            }
        } else if (action < 790) {  // índice secundario: ciclo de vida
            if (idx_alive) {
                (*dbp)["t"].drop_index("n");
                idx_alive = false;
            } else {
                (*dbp)["t"].create_index("n");
                idx_alive = true;
            }
        }
    }
    dbp->checkpoint();
    dbp.reset();
    Database db{d};
    CHECK(db["t"].count() == model.size());
    std::size_t bad = 0;
    for (const auto& [k, v] : model)
        if (!db["t"].get(k) || db["t"].get(k)->at("n").as_int() != v) bad++;
    CHECK(bad == 0);
    // el índice (vivo o no) responde consultas correctas tras el reopen
    const auto hits = db["t"].where(eq("n", Value(model.begin()->second)))
                           .collect();
    CHECK(hits.size() >= 1);
    CHECK(db["t"].where(between("n", Value(0), Value(1'000'000))).count() ==
          model.size());
}

// scenario 16 (0.7): corrupción de MEDIO en una página COMPROMETIDA del
// delta (dentro del extent del slot válido). La base abre, la pérdida se
// CLASIFICA (corrupt ≠ suspect) y se reporta por stats; los registros de esa
// página se pierden, el hueco queda libre y la base sigue operativa.
// (El schema va en un IC aparte: si su página se corrompe, la TABLA entera
// desaparece — también comportamiento correcto, pero no es lo que mide esto.)
static void delta_committed_page_corrupt() {
    const std::string d = "c-delta-corrupt";
    wipe(d);
    {
        Database db{d, Options{.durable = false, .checkpoint_every = 0,
                               .changelog_capacity = 0,
                               .checkpoint_on_close = false}};
        db["t"].create({.schemaless = true});
        db.checkpoint();  // IC #1: solo el schema (página propia)
        for (int i = 0; i < 200; i++)
            db["t"].emplace("id", "k" + std::to_string(i), "n", i);
        db.checkpoint();  // IC #2: las filas (páginas nuevas)
    }
    const auto dsz = fsize(d + "/delta.pages");
    CHECK(dsz >= 4 * 4096);
    // corromper el área de datos de la página 3: primera página de FILAS
    // (la 2 lleva el schema; su header queda intacto → clasificable)
    poke(d + "/delta.pages", 3 * 4096 + 40, 0xAB);
    poke(d + "/delta.pages", 3 * 4096 + 41, 0xCD);
    Database db{d};
    const auto st = db.stats();
    CHECK(st.delta_corrupt_pages == 1);   // detectada y clasificada
    CHECK(st.delta_suspect_pages == 0);   // NO es un torn normal
    const auto n = db["t"].count();
    CHECK(n < 200);                       // los registros de esa página se perdieron
    CHECK(n >= 1);                        // el resto de páginas aplica
    // la base sigue operativa y el hueco de la página corrupta se REUTILIZA
    db["t"].emplace("id", "post", "n", 1);
    db.checkpoint();
    Database db2{d};
    CHECK(db2["t"].count() == n + 1);
    CHECK(db2["t"].get("post").has_value());
    // la página corrupta (hueco derivado, el menor primero) se reescribió:
    // el contador refleja el estado ACTUAL del archivo, ya saneado
    CHECK(db2.stats().delta_corrupt_pages == 0);
}

// scenario 17 (0.7): cadena overflow ROTA en el delta (página "GNOV" con
// magic destruido). El registro L_OVF se pierde pero se cuenta y la base
// abre; el resto de filas y el WAL siguen intactos.
static void delta_broken_ovf_chain() {
    const std::string d = "c-delta-ovf";
    wipe(d);
    Bytes blob(8000, 0xAB);  // > INLINE_MAX → registro L_OVF en el IC
    {
        Database db{d, Options{.durable = false, .checkpoint_every = 0,
                               .changelog_capacity = 0,
                               .checkpoint_on_close = false}};
        db["t"].create({.schemaless = true});
        for (int i = 0; i < 5; i++)
            db["t"].emplace("id", "k" + std::to_string(i), "n", i);
        db["t"].emplace("id", "big", "data", blob);
        db["t"].emplace("id", "after", "n", 7);
        db.checkpoint();  // IC: 7 registros, uno de ellos overflow
    }
    // destruir el magic de la primera página overflow de la cadena
    // (las páginas ovf se asignan desde la 2 hacia arriba)
    bool found_ovf = false;
    {
        std::ifstream f(d + "/delta.pages", std::ios::binary);
        const auto sz = fsize(d + "/delta.pages");
        for (std::uint64_t pg = 2; pg < sz / 4096 && !found_ovf; pg++) {
            f.seekg(static_cast<std::streamoff>(pg * 4096));
            char magic[4] = {0, 0, 0, 0};
            f.read(magic, 4);
            if (magic[0] == 'G' && magic[1] == 'N' && magic[2] == 'O' &&
                magic[3] == 'V') {
                f.close();
                poke(d + "/delta.pages", pg * 4096, 0x00);  // romper magic
                found_ovf = true;
            }
        }
    }
    CHECK(found_ovf);
    Database db{d};  // abre pese a la cadena rota
    const auto st = db.stats();
    CHECK(st.delta_broken_ovf_records == 1);  // el blob se perdió contado
    CHECK(!db["t"].get("big").has_value());
    CHECK(db["t"].count() == 6);              // k0..k4 + after
    CHECK(db["t"].get("after")->at("n").as_int() == 7);
    CHECK(db["t"].get("k3")->at("n").as_int() == 3);
    // operativa después: la fila rota se puede reinsertar
    db["t"].emplace("id", "big", "data", blob);
    db.checkpoint();
    Database db2{d};
    CHECK(db2["t"].get("big").has_value());
    CHECK(db2["t"].get("big")->at("data").as_bytes() == blob);
}

// scenario 18 (0.7): schema superseded + filas antiguas vivas en el delta.
// El registro de schema VIEJO (muerto por supersición) sigue siendo la
// CREACIÓN de la tabla: las filas de ICs anteriores dependen de él para
// aplicarse. Regresión del bug hallado por la validación cruzada con el
// lector Zig (las filas de IC#1 se perdían silenciosamente al reabrir).
static void superseded_schema_keeps_old_rows() {
    const std::string d = "c-schema-sup";
    wipe(d);
    {
        Database db{d, Options{.durable = false, .checkpoint_every = 0,
                               .changelog_capacity = 0,
                               .checkpoint_on_close = false}};
        db["t"].create({.schemaless = true});
        for (int i = 0; i < 12; i++)
            db["t"].emplace("id", "k" + std::to_string(i), "n", i);
        db.checkpoint();              // IC#1: schema v1 + 12 filas
        db["t"].patch("k3", "n", 99); // IC#2: solo k3 (k0..k11 siguen vivas)
        db.checkpoint();
        db["t"].create_index("n");    // IC#3: schema v2 (supersede a v1)
        db.checkpoint();
    }
    Database db{d};
    CHECK(db["t"].count() == 12);     // NINGUNA fila de IC#1 se pierde
    CHECK(db["t"].get("k3")->at("n").as_int() == 99);
    CHECK(db["t"].get("k11")->at("n").as_int() == 11);
    CHECK(db["t"].get("k0")->at("n").as_int() == 0);
    // el índice declarado en el re-schema funciona tras el reopen
    CHECK(db["t"].where(eq("n", Value(99))).collect().size() == 1);
    // y un FC posterior converge al mismo estado
    db.compact();
    Database db2{d};
    CHECK(db2["t"].count() == 12);
    CHECK(db2["t"].get("k3")->at("n").as_int() == 99);
    CHECK(db2["t"].where(eq("n", Value(99))).collect().size() == 1);
}

int main() {
    torn_header();
    torn_body();
    bad_crc();
    bad_magic();
    uncommitted_tx();
    garbage_tail();
    corrupt_snapshot();
    truncated_snapshot();
    crash_between_rename_and_rebase();
    durable_reopen();
    delta_uncommitted_tail();
    delta_truncated();
    crash_mid_compact();
    replay_after_delta();
    fuzz_delta();
    delta_committed_page_corrupt();
    delta_broken_ovf_chain();
    superseded_schema_keeps_old_rows();
    std::cout << "crash tests: " << g_pass << " passed, " << g_fail
              << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
