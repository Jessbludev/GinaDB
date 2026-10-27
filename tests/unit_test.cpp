// GinaDB Lite 0.7 — unit test suite (no framework, no deps).
// Covers: CRUD, multimodal types, schema enforcement, custom PK (0.4 bug fix),
// silent-loss fix, transactions, persistence (snapshot + WAL replay + rebase),
// KNN, FTS, watch/changelog, query builder, schemaless, blob overflow,
// drop/recreate, concurrency, JSON round-trip, 0.4 WAL migration.
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#include "gina/db.hpp"
#include "../src/codec.hpp"
#include "../src/json.hpp"

using namespace gina;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) { ++g_pass; }                                         \
        else { ++g_fail; std::cerr << "FAIL " << __FILE__ << ":"       \
              << __LINE__ << "  " << #cond << "\n"; }                   \
    } while (0)
#define CHECK_THROWS(expr, errcode)                                     \
    do {                                                                \
        bool threw_ = false;                                            \
        try { (void)(expr); }                                           \
        catch (const Error& e_) {                                       \
            threw_ = e_.code() == Error::Code::errcode;                 \
            if (!threw_) { ++g_fail; std::cerr << "FAIL " << __LINE__   \
                  << "  wrong code: " << e_.what() << "\n"; }           \
        }                                                               \
        if (threw_) ++g_pass;                                           \
        else if (++g_fail) std::cerr << "FAIL " << __FILE__ << ":"      \
              << __LINE__ << "  no throw " << #errcode << "\n";         \
    } while (0)

static void wipe(const std::string& d) { std::filesystem::remove_all(d); }

// ---------------------------------------------------------------------------
static void test_crud_multimodal() {
    wipe("t-crud");
    Database db{"t-crud"};
    db["m"].create({.schemaless = true});
    Row r;
    r["id"] = "k1";
    r["b"] = true;
    r["i"] = std::int64_t{-42};
    r["f"] = 3.25;
    r["s"] = std::string("hola ñandú");
    r["y"] = Bytes{0x00, 0xFF, 0x10};
    r["a"] = Array{Value(1), Value(std::string("x")), Value(true)};
    r["o"] = Object{{"n", Value(std::int64_t{7})}, {"z", Value()}};
    r["v"] = Vec{1.5f, -2.5f, 0.0f};
    db["m"].insert(r);
    const auto got = db["m"].get("k1");
    CHECK(got.has_value());
    CHECK(got->at("b").as_bool() == true);
    CHECK(got->at("i").as_int() == -42);
    CHECK(got->at("f").as_double() == 3.25);
    CHECK(got->at("s").as_text() == "hola ñandú");
    CHECK(got->at("y").as_bytes() == Bytes({0x00, 0xFF, 0x10}));
    CHECK(got->at("a").as_array().size() == 3);
    CHECK(got->at("o").as_object().at("n").as_int() == 7);
    CHECK(got->at("o").as_object().at("z").is_null());
    CHECK(got->at("v").as_vector() == Vec({1.5f, -2.5f, 0.0f}));
    CHECK(db["m"].erase("k1"));
    CHECK(!db["m"].erase("k1"));            // 0.4 always returned true
    CHECK(!db["m"].get("k1").has_value());
}

static void test_schema_enforcement() {
    wipe("t-schema");
    Database db{"t-schema"};
    db["u"].create({.primary_key = "uid",
                    .columns{{.name = "uid", .type = T::Text, .nullable = false},
                             {.name = "age", .type = T::Int},
                             {.name = "score", .type = T::Float},
                             {.name = "nick", .type = T::Text, .nullable = false}}});
    // custom primary key is honoured (0.4 hardcoded "id")
    db["u"].emplace("uid", "x1", "age", 30, "score", 9.5, "nick", "gg");
    CHECK(db["u"].get("x1").has_value());
    CHECK(!db["u"].get("id").has_value());
    CHECK_THROWS(db["u"].emplace("id", "bad"), BadKey);            // pk column
    CHECK_THROWS(db["u"].emplace("uid", "x2", "nope", 1), SchemaViolation);
    CHECK_THROWS(db["u"].emplace("uid", "x3", "age", "viejo"), SchemaViolation);
    CHECK_THROWS(db["u"].emplace("uid", "", "age", 1), BadKey);
    // int -> float widening is accepted (stored as float)
    db["u"].emplace("uid", "x4", "age", 20, "score", 7);
    CHECK(db["u"].get("x4")->at("score").is_float());
    // null on non-nullable rejected; on nullable accepted
    CHECK_THROWS(db["u"].emplace("uid", "x5", "nick", nullptr, "age", 1),
                 SchemaViolation);
    db["u"].emplace("uid", "x6", "age", nullptr, "nick", "ok");
    CHECK(db["u"].get("x6")->at("age").is_null());
    // missing table throws (0.4 silently lost the write)
    CHECK_THROWS(db["nope"].emplace("id", "z"), NoSuchTable);
    CHECK_THROWS(db["nope"].erase("z"), NoSuchTable);
    CHECK_THROWS(db["nope"].all(), NoSuchTable);
    // partial update: unknown column / pk change rejected
    CHECK_THROWS(db["u"].patch("x1", "ghost", 1), SchemaViolation);
    CHECK_THROWS(db["u"].patch("x1", "uid", "otro"), BadKey);
    CHECK(!db["u"].update("missing", {{"age", Value(1)}}));
}

static void test_transactions() {
    wipe("t-tx");
    Database db{"t-tx"};
    db["a"].create({.schemaless = true});
    {
        auto tx = db.begin();
        tx["a"].emplace("id", "1", "v", 10);
        tx["a"].emplace("id", "2", "v", 20);
        tx["a"].erase("1");
        tx.commit();
        CHECK(db["a"].count() == 1);
    }
    {  // rollback discards everything
        auto tx = db.begin();
        tx["a"].emplace("id", "3", "v", 30);
        tx.rollback();
        CHECK(db["a"].count() == 1);
    }
    {  // invalid op aborts the whole tx with no WAL bytes appended
        auto tx = db.begin();
        tx["a"].emplace("id", "4", "v", 40);
        tx["missing"].emplace("id", "5");
        bool threw = false;
        try { tx.commit(); } catch (const Error&) { threw = true; }
        CHECK(threw);
        CHECK(db["a"].count() == 1);
        CHECK(!db["a"].get("4").has_value());  // atomic: nothing applied
    }
    {  // destructor rolls back
        auto tx = db.begin();
        tx["a"].emplace("id", "6", "v", 60);
    }
    CHECK(db["a"].count() == 1);
}

static void test_persistence() {
    wipe("t-persist");
    {
        Database db{"t-persist"};
        db["p"].create({.schemaless = true});
        for (int i = 0; i < 50; i++)
            db["p"].emplace("id", "k" + std::to_string(i), "n", i);
        db.checkpoint();
        for (int i = 50; i < 80; i++)  // post-checkpoint commits → WAL only
            db["p"].emplace("id", "k" + std::to_string(i), "n", i);
    }
    {  // snapshot + WAL replay (0.6: checkpoint incremental → delta.pages)
        Database db{"t-persist"};
        CHECK(db["p"].count() == 80);
        CHECK(db["p"].get("k79")->at("n").as_int() == 79);
        const auto st = db.stats();
        CHECK(st.delta_bytes > 0);          // el delta lleva el checkpoint
        CHECK(st.pages == 0);               // sin base todavía (sin compactar)
        db.compact();                       // compactación → base completo
        const auto st2 = db.stats();
        CHECK(st2.data_bytes > 0);
        CHECK(st2.pages >= 2);
        CHECK(st2.delta_live_pages == 0);   // delta reiniciado
    }
    {  // WAL must have been rebased after the last (destructor) checkpoint
        Database db{"t-persist"};
        db.checkpoint();
        const auto st1 = db.stats();
        const auto sz1 = st1.wal_bytes;
        db["p"].emplace("id", "post", "n", 1);
        const auto sz2 = db.stats().wal_bytes;
        CHECK(sz1 < sz2);  // a single new tx grew the WAL beyond the base frame
    }
    {  // snapshot alone survives WAL loss
        Database db{"t-persist"};
        db.checkpoint();
    }
    std::filesystem::remove_all("t-persist/wal.log");
    {
        Database db{"t-persist"};
        CHECK(db["p"].count() == 81);
    }
}

static void test_blob_overflow() {
    wipe("t-blob");
    const std::size_t big = 100'000;  // >> one 4 KiB page
    Bytes blob(big);
    for (std::size_t i = 0; i < big; i++) blob[i] = static_cast<std::uint8_t>(i * 31);
    {  // 0.6: el blob vive en el delta (IC) y luego en el base (compact)
        Database db{"t-blob"};
        db["blobs"].create({.schemaless = true});
        db["blobs"].emplace("id", "b1", "data", blob);
        db["blobs"].emplace("id", "small", "data", Bytes{1, 2, 3});
        db.checkpoint();
        const auto st2 = db.stats();
        CHECK(st2.delta_pages >= 20);  // el blob ocupa páginas overflow del delta
        db.compact();
        const auto st3 = db.stats();
        CHECK(st3.overflow_pages >= 20);  // y también del base tras compactar
        CHECK(st3.delta_live_pages == 0);
    }
    {
        Database db{"t-blob"};
        const auto got = db["blobs"].get("b1");
        CHECK(got.has_value());
        CHECK(got->at("data").as_bytes() == blob);  // byte-exact after reload
        CHECK(db["blobs"].get("small")->at("data").as_bytes() == Bytes({1, 2, 3}));
    }
}

static void test_knn() {
    wipe("t-knn");
    Database db{"t-knn"};
    db["v"].create({.schemaless = true});
    db["v"].emplace("id", "a", "e", Vec{1, 0, 0});
    db["v"].emplace("id", "b", "e", Vec{0, 1, 0});
    db["v"].emplace("id", "c", "e", Vec{0.9f, 0.1f, 0});
    db["v"].emplace("id", "d", "e", Vec{0, 0, 1});
    db["v"].emplace("id", "wrong", "e", Vec{1, 1});      // different dim: skipped
    db["v"].emplace("id", "notvec", "e", "texto");       // not a vector: skipped
    auto cos = db["v"].knn("e", {1, 0, 0}, 2);
    CHECK(cos.size() == 2);
    CHECK(cos[0].first == "a" && cos[0].second > 0.999);
    CHECK(cos[1].first == "c");
    auto l2 = db["v"].knn("e", {0, 0, 0.1f}, 1, Metric::L2);
    CHECK(l2.size() == 1 && l2[0].first == "d");
    CHECK_THROWS(db["v"].knn("e", {}, 1), BadArgument);
}

static void test_fts() {
    wipe("t-fts");
    Database db{"t-fts"};
    db["docs"].create({.columns{{.name = "id", .type = T::Text, .nullable = false},
                                {.name = "body", .type = T::Text, .fts = true},
                                {.name = "plain", .type = T::Text}}});
    db["docs"].emplace("id", "d1", "body", "La Base de Datos embebida",
                       "plain", "otro texto");
    db["docs"].emplace("id", "d2", "body", "motor de almacenamiento WAL",
                       "plain", "texto plano");
    db["docs"].emplace("id", "d3", "body", "datos y más datos",
                       "plain", "plain search aquí");
    // indexed AND search, case-insensitive, accents preserved
    auto hits = db["docs"].search("body", "base datos");
    CHECK(hits.size() == 1 && hits[0].at("id").as_text() == "d1");
    hits = db["docs"].search("body", "datos más");
    CHECK(hits.size() == 1 && hits[0].at("id").as_text() == "d3");
    CHECK(db["docs"].search("body", "base wal").empty());  // no single doc has both
    // update maintains the index (old tokens removed)
    db["docs"].patch("d1", "body", "ahora habla de índices");
    CHECK(db["docs"].search("body", "base").empty());
    CHECK(db["docs"].search("body", "índices").size() == 1);
    db["docs"].erase("d3");
    CHECK(db["docs"].search("body", "datos").empty());
    // brute-force fallback on a non-indexed column (d3 was erased above)
    hits = db["docs"].search("plain", "plano");
    CHECK(hits.size() == 1 && hits[0].at("id").as_text() == "d2");
}

static void test_watch_changelog() {
    wipe("t-rt");
    Database db{"t-rt", Options{.durable = false, .checkpoint_every = 0,
                                .changelog_capacity = 3}};
    std::vector<std::string> seen;
    const auto id1 = db["w"].watch([&](const Event& e) {
        seen.push_back((e.kind == Event::Kind::Put ? "+" : "-") + e.key);
    });
    db["w"].create({.schemaless = true});  // DDL does not fire row events
    db["w"].emplace("id", "a", "v", 1);
    db["w"].emplace("id", "b", "v", 2);
    db["w"].erase("a");
    CHECK(seen == std::vector<std::string>({"+a", "+b", "-a"}));

    auto cl = db["w"].changes(0);          // 3 events fit in capacity=3
    CHECK(cl.events.size() == 3);
    CHECK(cl.next_cursor == 3);
    CHECK(!cl.truncated);
    auto cl2 = db["w"].changes(cl.next_cursor);
    CHECK(cl2.events.empty());

    const auto id2 = db["w"].watch([](const Event&) {});
    db["w"].unwatch(id1);
    db["w"].unwatch(id2);
    db["w"].emplace("id", "c", "v", 3);   // 4th event: ring evicts "+a"
    CHECK(seen.size() == 3);  // unwatched → no more events
    cl = db["w"].changes(0);
    CHECK(cl.truncated);
    CHECK(cl.events.size() == 3);
    CHECK(cl.events.front().ev.key == "b");
}

static void test_query_builder() {
    wipe("t-query");
    Database db{"t-query"};
    db["q"].create({.schemaless = true});
    for (int i = 0; i < 10; i++)
        db["q"].emplace("id", "r" + std::to_string(i), "n", i, "grp",
                        i % 2 == 0 ? "even" : "odd");
    auto even = db["q"].where([](const Row& r) {
                    return r.at("grp").as_text() == "even";
                }).collect();
    CHECK(even.size() == 5);
    auto top3 = db["q"].where([](const Row& r) { return r.at("n").as_int() < 10; })
                    .order_by("n", true).limit(3).collect();
    CHECK(top3.size() == 3);
    CHECK(top3[0].at("n").as_int() == 9);
    CHECK(top3[2].at("n").as_int() == 7);
    auto asc = db["q"].where([](const Row&) { return true; })
                    .order_by("n").limit(2).collect();
    CHECK(asc[0].at("n").as_int() == 0 && asc[1].at("n").as_int() == 1);
    CHECK(db["q"].where([](const Row&) { return true; }).count() == 10);
    const auto found = db["q"].find(
        [](const Row& r) { return r.at("n").as_int() == 7; });
    CHECK(found.has_value() && found->at("id").as_text() == "r7");
    CHECK(db["q"].where([](const Row&) { return false; }).count() == 0);
}

// ---------------------------------------------------------------------------
// 0.6 — índices secundarios, planner, rangos PK, select JSON
// ---------------------------------------------------------------------------
static void test_secondary_index() {
    wipe("t-secidx");
    {
        Database db{"t-secidx", Options{.checkpoint_every = 0}};
        db["u"].create({.primary_key = "id",
                        .columns{{"id", T::Text},
                                 {"email", T::Text},
                                 {"age", T::Int},
                                 {"city", T::Text},
                                 {"score", T::Float}}});
        for (int i = 0; i < 200; i++)
            db["u"].emplace("id", "user" + std::to_string(100 + i),
                            "email", "u" + std::to_string(i) + "@x.io",
                            "age", 18 + (i % 50), "city",
                            i % 3 == 0 ? "Madrid" : (i % 3 == 1 ? "Lima" : "Bogotá"),
                            "score", 0.5 + (i % 7));
        // índices tras cargar (rebuild desde filas existentes)
        db["u"].create_index("email");
        db["u"].create_index("age");
        db["u"].create_index("city");
        CHECK_THROWS(db["u"].create_index("email"), BadArgument);  // duplicado
        CHECK_THROWS(db["u"].create_index("nope"), BadArgument);   // columna

        // eq por índice == resultado del scan equivalente
        const auto by_idx = db["u"].where(eq("email", "u77@x.io")).collect();
        const auto by_scan = db["u"].where([](const Row& r) {
            return r.at("email").as_text() == "u77@x.io";
        }).collect();
        CHECK(by_idx.size() == 1 && by_scan.size() == 1);
        CHECK(by_idx[0].at("id").as_text() == "user177");

        // rango por índice (age entre 30 y 34, inclusivo)
        const auto rng = db["u"].where(between("age", 30, 34)).collect();
        std::size_t want = 0;
        for (int i = 0; i < 200; i++) {
            const int a = 18 + (i % 50);
            if (a >= 30 && a <= 34) want++;
        }
        CHECK(rng.size() == want);
        CHECK(db["u"].where(gt("age", 66)).count() ==
              db["u"].where([](const Row& r) {
                  return r.at("age").as_int() > 66;
              }).count());
        CHECK(db["u"].where(le("age", 18)).count() ==
              db["u"].where([](const Row& r) {
                  return r.at("age").as_int() <= 18;
              }).count());

        // baja cardinalidad: varias filas por valor, PKs ordenadas
        const auto mad = db["u"].where(eq("city", "Madrid")).collect();
        CHECK(mad.size() == 67);  // ceil? 200/3 → i%3==0 → 67
        CHECK(mad[0].at("id").as_text() < mad.back().at("id").as_text());

        // actualización mantiene el índice
        db["u"].patch("user100", "age", 70);
        CHECK(db["u"].where(eq("age", 70)).count() >= 1);
        CHECK(db["u"].where(eq("age", 18)).count() ==
              db["u"].where([](const Row& r) {
                  return r.at("age").as_int() == 18;
              }).count());  // user100 ya no tiene 18: índice y scan coinciden

        // borrado mantiene el índice
        db["u"].erase("user101");
        CHECK(!db["u"].where(eq("email", "u1@x.io")).count());

        // AND de dos índices + in_
        const auto both = db["u"]
                              .where(all_of({eq("city", "Lima"),
                                             between("age", 20, 25)}))
                              .collect();
        CHECK(both.size() > 0);
        for (const auto& r : both) {
            CHECK(r.at("city").as_text() == "Lima");
            CHECK(r.at("age").as_int() >= 20 && r.at("age").as_int() <= 25);
        }
        const auto inq = db["u"].where(in_("city", {Value("Lima"), Value("Bogotá")}))
                             .count();
        CHECK(inq == db["u"].where([](const Row& r) {
                         return r.at("city").as_text() != "Madrid";
                     }).count());

        // índice float con codificación ordenable (negativos incluidos)
        db["u"].emplace("id", "neg", "email", "neg@x.io", "age", 1,
                        "city", "Lima", "score", -2.5);
        CHECK(db["u"].where(le("score", -2.5)).count() == 1);

        // drop_index: deja de usarse pero las consultas siguen correctas
        db["u"].drop_index("age");
        CHECK_THROWS(db["u"].drop_index("age"), BadArgument);
        CHECK(db["u"].where(between("age", 30, 34)).count() == want);
        db.checkpoint();  // persistir índices (email, city) vía delta
    }
    {  // reopen: índices persistidos y consistentes
        Database db{"t-secidx"};
        const auto st = db["u"].schema();
        CHECK(st.indexes.size() == 2);  // email, city (age fue dropped)
        const auto hit = db["u"].where(eq("email", "u77@x.io")).collect();
        CHECK(hit.size() == 1);
        CHECK(db["u"].where(eq("city", "Madrid")).count() == 67);
        CHECK(db["u"].where(eq("score", -2.5)).count() == 1);  // sin índice → scan
    }
}

static void test_pk_ranges() {
    wipe("t-pkrange");
    Database db{"t-pkrange"};
    db["k"].create({.primary_key = "id", .columns{{"id", T::Text}, {"n", T::Int}}});
    for (int i = 0; i < 100; i++) {
        char key[16];
        std::snprintf(key, sizeof key, "k%03d", i);
        db["k"].emplace("id", key, "n", i);
    }
    // all() sale ordenado por PK (B+Tree)
    const auto all = db["k"].all();
    CHECK(all.size() == 100);
    CHECK(all[0].at("id").as_text() == "k000");
    CHECK(all[99].at("id").as_text() == "k099");

    // between inclusivo por PK
    const auto mid = db["k"].where(between("id", "k010", "k019")).collect();
    CHECK(mid.size() == 10);
    CHECK(mid.front().at("id").as_text() == "k010");
    CHECK(mid.back().at("id").as_text() == "k019");

    // gt/lt exclusivos
    CHECK(db["k"].where(gt("id", "k098")).count() == 1);
    CHECK(db["k"].where(ge("id", "k098")).count() == 2);
    CHECK(db["k"].where(lt("id", "k001")).count() == 1);
    CHECK(db["k"].where(le("id", "k001")).count() == 2);

    // eq por PK (PkPoint) + verificación de cond adicional
    const auto one = db["k"].where(all_of({eq("id", "k042"),
                                           gt("n", 40)})).collect();
    CHECK(one.size() == 1 && one[0].at("n").as_int() == 42);

    // order_by PK desc con limit (recorrido inverso, sin ordenar)
    const auto last5 = db["k"].where(ge("id", "k095"))
                           .order_by("id", true)
                           .collect();
    CHECK(last5.size() == 5);
    CHECK(last5[0].at("id").as_text() == "k099");
    CHECK(last5[4].at("id").as_text() == "k095");

    // order por columna NO pk → ordena materializando
    const auto desc_n = db["k"].where(gt("n", 96)).order_by("n", true).collect();
    CHECK(desc_n.size() == 3);
    CHECK(desc_n[0].at("n").as_int() == 99);
}

static void test_select_json() {
    wipe("t-select");
    Database db{"t-select"};
    db["s"].create({.primary_key = "id",
                    .columns{{"id", T::Text}, {"email", T::Text}, {"age", T::Int}},
                    .indexes{"email", "age"}});
    for (int i = 0; i < 50; i++)
        db["s"].emplace("id", "s" + std::to_string(i),
                        "email", "s" + std::to_string(i) + "@d.io",
                        "age", 20 + i);
    // el parser de select produce Cond equivalentes (lo que usa gina_select)
    auto q1 = json::decode_select(
        "{\"where\":{\"op\":\"and\",\"args\":["
        "{\"op\":\"ge\",\"col\":\"age\",\"val\":40},"
        "{\"op\":\"lt\",\"col\":\"age\",\"val\":45}]},"
        "\"order\":[{\"col\":\"age\",\"desc\":true}],\"limit\":3,"
        "\"fields\":[\"id\",\"age\"]}");
    CHECK(q1.where.has_value());
    CHECK(q1.limit == 3);
    CHECK(q1.fields.size() == 2);
    auto rows = db["s"].where(*q1.where).collect();
    CHECK(rows.size() == 5);  // 40..44
    // proyección manual como haría el ABI
    for (const auto& r : rows) {
        CHECK(r.at("age").as_int() >= 40);
        CHECK(r.at("age").as_int() < 45);
    }
    auto q2 = json::decode_select(
        "{\"where\":{\"op\":\"eq\",\"col\":\"email\",\"val\":\"s7@d.io\"}}");
    const auto one = db["s"].where(*q2.where).collect();
    CHECK(one.size() == 1 && one[0].at("id").as_text() == "s7");
    auto q3 = json::decode_select("{}");
    CHECK(!q3.where.has_value() && q3.limit == static_cast<std::size_t>(-1));
    bool threw = false;
    try {
        (void)json::decode_select("{\"where\":{\"op\":\"xor\",\"col\":\"x\"}}");
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);
}

static void test_drop_recreate() {
    wipe("t-drop");
    {
        Database db{"t-drop"};
        db["x"].create({.schemaless = true});
        db["x"].emplace("id", "1");
        db["x"].drop();
        CHECK(!db["x"].exists());
        CHECK_THROWS(db["x"].all(), NoSuchTable);
        db["x"].create({.schemaless = true});  // recreate with same name
        db["x"].emplace("id", "2");
    }
    Database db{"t-drop"};
    CHECK(db["x"].count() == 1);
    CHECK(db["x"].get("2").has_value());
}

static void test_concurrency() {
    wipe("t-conc");
    {
        Database db{"t-conc"};
        db["c"].create({.schemaless = true});
        for (int i = 0; i < 200; i++)
            db["c"].emplace("id", "w" + std::to_string(i));
        std::atomic<int> errors{0};
        std::atomic<std::int64_t> reads{0};
        std::vector<std::thread> ths;
        for (int t = 0; t < 4; t++) {
            ths.emplace_back([&, t] {
                std::mt19937 rng(static_cast<unsigned>(t * 7919 + 1));
                for (int i = 0; i < 300; i++) {
                    const auto k = "w" + std::to_string(rng() % 200);
                    try {
                        if (i % 3 == 0) {
                            db["c"].patch(k, "touch", Value(t));
                        } else {
                            if (db["c"].get(k)) ++reads;
                        }
                    } catch (const std::exception&) {
                        ++errors;
                    }
                }
            });
        }
        for (auto& th : ths) th.join();
        CHECK(errors == 0);
        CHECK(reads > 100);
        CHECK(db["c"].count() == 200);
    }
    Database db{"t-conc"};  // everything survived a reopen
    CHECK(db["c"].count() == 200);
}

static void test_json_roundtrip() {
    const Row r{{"id", Value(std::string("j1"))},
                {"n", Value(std::int64_t{123})},
                {"f", Value(2.5)},
                {"b", Value(true)},
                {"z", Value()},
                {"y", Value(Bytes{1, 2, 3, 0xFF})},
                {"v", Value(Vec{0.5f, -1.5f})},
                {"a", Value(Array{Value(1), Value(std::string("dos"))})},
                {"o", Value(Object{{"k", Value(std::string("ñ"))}})}};
    const std::string j = json::encode_row(r);
    const Row back = json::decode_row(j);
    CHECK(back.at("id").as_text() == "j1");
    CHECK(back.at("n").as_int() == 123);
    CHECK(back.at("f").as_double() == 2.5);
    CHECK(back.at("b").as_bool());
    CHECK(back.at("z").is_null());
    CHECK(back.at("y").is_bytes());
    CHECK(back.at("y").as_bytes() == Bytes({1, 2, 3, 0xFF}));
    CHECK(back.at("v").is_vector());
    CHECK(back.at("v").as_vector() == Vec({0.5f, -1.5f}));
    CHECK(back.at("a").as_array().size() == 2);
    CHECK(back.at("o").as_object().at("k").as_text() == "ñ");
    CHECK_THROWS(json::decode("{bad"), BadArgument);
    CHECK_THROWS(json::decode_row("[1,2]"), BadArgument);
    // schema JSON round-trip
    TableDef def;
    def.primary_key = "uid";
    def.columns = {{.name = "uid", .type = T::Text, .nullable = false},
                   {.name = "body", .type = T::Text, .fts = true}};
    const auto sj = json::encode_schema(def);
    const auto back2 = json::decode_schema(sj);
    CHECK(back2.primary_key == "uid");
    CHECK(back2.columns.size() == 2);
    CHECK(back2.columns[1].fts);
    CHECK(back2.columns[0].type == T::Text);
}

static void test_v4_migration() {
    // Hand-build a 0.4-style database: v4 WAL frames, no snapshot.
    wipe("t-v4");
    std::filesystem::create_directories("t-v4");
    {
        std::vector<std::uint8_t> schema_payload;
        codec::put_str(schema_payload, "old");
        codec::put_str(schema_payload, "id");
        codec::put32(schema_payload, 2);
        codec::put_str(schema_payload, "id");
        schema_payload.push_back(static_cast<std::uint8_t>(T::Text));
        schema_payload.push_back(0);
        codec::put_str(schema_payload, "n");
        schema_payload.push_back(static_cast<std::uint8_t>(T::Int));
        schema_payload.push_back(1);

        std::vector<std::uint8_t> row_payload;
        codec::put_str(row_payload, "old");
        codec::encode_row(row_payload, Row{{"id", Value(std::string("k1"))},
                                           {"n", Value(std::int64_t{7})}});

        std::ofstream f("t-v4/wal.log", std::ios::binary);
        auto wr = [&](std::uint8_t kind, std::uint64_t lsn, std::uint64_t tx,
                      const std::vector<std::uint8_t>& p) {
            // 0.4 encoded frames with version=4
            std::vector<std::uint8_t> body;
            body.push_back(codec::WAL_VERSION_V4);
            body.push_back(kind);
            codec::put64(body, lsn);
            codec::put64(body, tx);
            body.insert(body.end(), p.begin(), p.end());
            std::vector<std::uint8_t> frame;
            codec::put32(frame, codec::WAL_MAGIC);
            codec::put32(frame, static_cast<std::uint32_t>(body.size()));
            codec::put32(frame, codec::crc32(body.data(), body.size()));
            frame.insert(frame.end(), body.begin(), body.end());
            f.write(reinterpret_cast<const char*>(frame.data()),
                    static_cast<std::streamsize>(frame.size()));
        };
        wr(codec::K_SCHEMA, 1, 1, schema_payload);
        wr(codec::K_COMMIT, 2, 1, {});
        wr(codec::K_PUT, 3, 2, row_payload);
        wr(codec::K_COMMIT, 4, 2, {});
    }
    {
        Database db{"t-v4"};  // opens the 0.4 WAL and migrates it
        CHECK(db["old"].count() == 1);
        CHECK(db["old"].get("k1")->at("n").as_int() == 7);
        db.checkpoint();  // converts to 0.5 snapshot + rebased WAL
    }
    {
        Database db{"t-v4"};
        CHECK(db["old"].count() == 1);
        CHECK(db["old"].get("k1")->at("n").as_int() == 7);
    }
}

int main() {
    test_crud_multimodal();
    test_schema_enforcement();
    test_transactions();
    test_persistence();
    test_blob_overflow();
    test_knn();
    test_fts();
    test_watch_changelog();
    test_query_builder();
    test_secondary_index();
    test_pk_ranges();
    test_select_json();
    test_drop_recreate();
    test_concurrency();
    test_json_roundtrip();
    test_v4_migration();
    std::cout << "unit tests: " << g_pass << " passed, " << g_fail
              << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
