// GinaDB Lite 0.7 — demo: full tour of the new-style API (multimodal + real-time).
#include "gina/db.hpp"
#include <iostream>

using namespace gina;

int main() {
    std::filesystem::remove_all("demo-data");
    Database db{"demo-data"};

    // ---- relational: strict schema + type-safe values ----------------------
    db["users"].create({.primary_key = "id",
                        .columns{{.name = "id", .type = T::Text, .nullable = false},
                                 {.name = "name", .type = T::Text, .nullable = false},
                                 {.name = "age", .type = T::Int},
                                 {.name = "bio", .type = T::Text, .fts = true}}});

    // new-style variadic emplace: "key", value, "key", value, ...
    db["users"].emplace("id", "u1", "name", "Gina", "age", 34,
                        "bio", "creadora de bases de datos embebidas");
    db["users"].emplace("id", "u2", "name", "Zed", "age", 50,
                        "bio", "usuario de bases de datos");
    db["users"].emplace("id", "u3", "name", "Ada", "age", 36,
                        "bio", "pionera de la programación");

    auto u = db["users"].get("u1");
    std::cout << "get(u1) = " << u->at("name").as_text()
              << " age=" << u->at("age").as_int() << "\n";

    db["users"].patch("u1", "age", 35);  // partial update
    auto adults = db["users"].where([](const Row& r) {
                      return r.at("age").as_int() >= 35;
                  }).order_by("age", true).limit(2).collect();
    std::cout << "where(age>=35) order_by desc limit 2 -> ";
    for (const auto& r : adults) std::cout << r.at("id").as_text() << " ";
    std::cout << "\n";

    // ---- schema enforcement (type-safe) -------------------------------------
    try {
        db["users"].emplace("id", "bad", "age", "treinta y seis");
    } catch (const Error& e) {
        std::cout << "schema violation caught: " << e.what() << "\n";
    }

    // ---- hybrid: schemaless table (relational OR document mode) -------------
    db["events"].create({.schemaless = true});
    db["events"].emplace("id", "e1", "type", "login", "meta",
                         Object{{"ip", Value("10.0.0.1")}, {"ua", Value("curl")}});

    // ---- multimodal: vectors + KNN ------------------------------------------
    db["media"].create({.primary_key = "id",
                        .columns{{.name = "id", .type = T::Text, .nullable = false},
                                 {.name = "tag", .type = T::Text},
                                 {.name = "vec", .type = T::Vector},
                                 {.name = "data", .type = T::Bytes}}});
    db["media"].emplace("id", "m1", "tag", "gina", "vec", Vec{0.1f, 0.2f, 0.3f},
                        "data", Bytes{0xDE, 0xAD, 0xBE, 0xEF});
    db["media"].emplace("id", "m2", "tag", "db", "vec", Vec{0.9f, 0.1f, 0.0f},
                        "data", Bytes{1, 2, 3, 4});
    db["media"].emplace("id", "m3", "tag", "lite", "vec", Vec{0.12f, 0.19f, 0.31f},
                        "data", Bytes{9, 9});

    const auto nn = db["media"].knn("vec", {0.1f, 0.2f, 0.3f}, 2);
    std::cout << "knn(vec, [0.1,0.2,0.3], k=2) -> ";
    for (const auto& [k, s] : nn) std::cout << k << "(" << s << ") ";
    std::cout << "\n";

    // ---- multimodal: full-text search ---------------------------------------
    const auto hits = db["users"].search("bio", "bases datos");
    std::cout << "search(bio, 'bases datos') -> ";
    for (const auto& r : hits) std::cout << r.at("id").as_text() << " ";
    std::cout << "\n";

    // ---- transactions ---------------------------------------------------------
    {
        auto tx = db.begin();
        tx["users"].emplace("id", "u4", "name", "Linus", "age", 55,
                            "bio", "usuario de bases de datos");
        tx["users"].erase("u2");
        tx.commit();
        std::cout << "after tx: users=" << db["users"].count() << "\n";
    }

    // ---- real-time: watch + changelog ----------------------------------------
    auto sub = db["users"].watch([](const Event& ev) {
        std::cout << "  [watch] " << (ev.kind == Event::Kind::Put ? "PUT " : "DEL ")
                  << ev.table << "/" << ev.key << "\n";
    });
    db["users"].emplace("id", "u5", "name", "Grace", "age", 45, "bio", "programadora");
    db["users"].erase("u5");
    db["users"].unwatch(sub);

    const auto log = db["users"].changes(0);
    std::cout << "changelog: " << log.events.size() << " events, next_cursor="
              << log.next_cursor << "\n";

    // ---- durability -----------------------------------------------------------
    db.checkpoint();
    const auto s = db.stats();
    std::cout << "stats: rows=" << s.rows << " lsn=" << s.last_lsn
              << " pages=" << s.pages << " wal=" << s.wal_bytes << "B"
              << " data=" << s.data_bytes << "B\n";
    std::cout << "demo OK\n";
}
