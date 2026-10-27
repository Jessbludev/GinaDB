// GinaDB Lite 0.7 — reopen_bench.cpp: banco focalizado de reapertura.
//
// Reproduce la forma de datos del workload TS (100K users + 10K docs 64d,
// índices email/age/city + docs.title, FTS en bio/body) y mide:
//   1) construcción (lotes de 500, como el workload)
//   2) IC inicial + FC (compactación)
//   3) REOPEN tras FC (el caso de los ~850ms de decode del informe 0.6)
//   4) 500 updates + IC pequeño + REOPEN (camino base+delta+WAL)
//   5) FTS/KNN/select de verificación (mismos resultados tras reopen)
//
// Compilar con -DGINA_TRACE para el desglose de fases del recovery.
#include "gina/db.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace gina;

static const int USERS = 100000;
static const int DOCS = 10000;
static const int VEC_DIM = 64;

template <class F>
static double ms(F&& f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0)
        .count();
}

static const char* CITIES[] = {"madrid", "bogota", "lima", "cdmx",
                               "buenos aires", "santiago", "lisboa"};
static const char* WORDS[] = {"base", "datos", "embeddings", "indices", "wal",
                              "snapshot", "motor", "consultas", "libre"};

static Row user_row(int i, std::mt19937_64& rng) {
    Row r;
    r["id"] = Value("u" + std::to_string(i));
    r["name"] = Value("user " + std::to_string(i));
    r["email"] = Value("user" + std::to_string(i) + "@example.io");
    r["age"] = Value(static_cast<std::int64_t>(18 + rng() % 60));
    r["city"] = Value(std::string(CITIES[rng() % 7]));
    Array tags;
    for (int t = 0; t < 3; t++)
        tags.push_back(Value(std::string("tag") + std::to_string(rng() % 50)));
    r["tags"] = Value(std::move(tags));
    r["bio"] = Value(std::string(WORDS[rng() % 9]) + " " +
                     WORDS[rng() % 9] + " " + WORDS[rng() % 9] + " " +
                     WORDS[rng() % 9] + " " + WORDS[rng() % 9] + " " +
                     WORDS[rng() % 9]);
    return r;
}

static Row doc_row(int i, std::mt19937_64& rng) {
    Row r;
    r["id"] = Value("d" + std::to_string(i));
    r["title"] = Value(std::string(WORDS[rng() % 9]) + " " + WORDS[rng() % 9] +
                       " " + std::to_string(i));
    r["body"] = Value(std::string(WORDS[rng() % 9]) + " " + WORDS[rng() % 9] +
                      " " + WORDS[rng() % 9] + " " + WORDS[rng() % 9]);
    Vec v;
    v.reserve(VEC_DIM);
    std::uniform_real_distribution<float> d(-1.f, 1.f);
    for (int j = 0; j < VEC_DIM; j++) v.push_back(d(rng));
    r["embedding"] = Value(std::move(v));
    return r;
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "bench-reopen-db";
    std::filesystem::remove_all(dir);

    std::mt19937_64 rng(42);  // semilla fija: reproducible

    printf("== construcción ==\n");
    double build_ms = 0;
    {
        Database db(dir, Options{.checkpoint_every = 0});
        db["users"].create({.primary_key = "id",
                            .columns = {{"id", T::Text}, {"name", T::Text},
                                        {"email", T::Text}, {"age", T::Int},
                                        {"city", T::Text}, {"tags", T::Array},
                                        {"bio", T::Text, true, true}},
                            .indexes = {"email", "age", "city"}});
        db["docs"].create({.primary_key = "id",
                           .columns = {{"id", T::Text}, {"title", T::Text},
                                       {"body", T::Text, true, true},
                                       {"embedding", T::Vector}},
                           .indexes = {"title"}});
        build_ms = ms([&] {
            for (int i = 0; i < USERS; i += 500) {
                auto tx = db.begin();
                for (int j = i; j < i + 500 && j < USERS; j++)
                    tx["users"].insert(user_row(j, rng));
                tx.commit();
            }
            for (int i = 0; i < DOCS; i += 500) {
                auto tx = db.begin();
                for (int j = i; j < i + 500 && j < DOCS; j++)
                    tx["docs"].insert(doc_row(j, rng));
                tx.commit();
            }
        });
        printf("  carga %d users + %d docs : %8.1f ms\n", USERS, DOCS, build_ms);

        printf("  IC (todo dirty)         : %8.1f ms\n", ms([&] { db.checkpoint(); }));
        printf("  FC (compact)            : %8.1f ms\n", ms([&] { db.compact(); }));
        printf("  filas: users=%zu docs=%zu\n", db["users"].count(),
               db["docs"].count());
    }

    printf("== reopen tras FC (base puro) ==\n");
    std::uint64_t probe_age = 0;
    {
        double rop = ms([&] {
            Database db(dir, Options{.checkpoint_every = 0});
            probe_age = static_cast<std::uint64_t>(db["users"].count());
        });
        printf("  reopen                  : %8.1f ms  (count=%llu)\n", rop,
               (unsigned long long)probe_age);
    }

    printf("== reopen con delta pequeño (500 updates + IC) ==\n");
    {
        Database db(dir, Options{.checkpoint_every = 0});
        for (int i = 0; i < 500; i++)
            db["users"].patch("u" + std::to_string(i), "age",
                              Value(static_cast<std::int64_t>(70 + i % 20)));
        printf("  IC dirty=500            : %8.1f ms\n", ms([&] { db.checkpoint(); }));
    }
    {
        double rop = ms([&dir] {
            Database db(dir, Options{.checkpoint_every = 0});
            (void)db["users"].count();
        });
        printf("  reopen base+delta       : %8.1f ms\n", rop);
    }

    printf("== verificación funcional tras reopen ==\n");
    {
        Database db(dir, Options{.checkpoint_every = 0});
        auto hit = db["users"].where(eq("email", "user4242@example.io")).collect();
        printf("  eq índice email         : %zu fila(s)\n", hit.size());
        auto rngq = db["users"]
                        .where(between("age", Value(std::int64_t(30)),
                                       Value(std::int64_t(39))))
                        .order_by("age", true)
                        .limit(10)
                        .collect();
        printf("  rango age desc limit 10 : %zu filas, primera age=%lld\n",
               rngq.size(),
               rngq.empty() ? -1LL
                            : rngq.front().at("age").as_int());
        auto fts = db["users"].search("bio", "base datos");
        printf("  FTS 'base datos'        : %zu filas\n", fts.size());
        Vec q;
        for (int i = 0; i < VEC_DIM; i++) q.push_back(0.01f * i);
        auto knn = db["docs"].knn("embedding", q, 5);
        printf("  KNN k=5 @%d docs        : %zu hits, top=%s\n", DOCS, knn.size(),
               knn.empty() ? "-" : knn.front().first.c_str());
        auto u = db["users"].get("u1");
        printf("  get u1 (delta)          : age=%lld\n",
               u && u->at("age").is_int() ? u->at("age").as_int() : -1LL);
    }
    printf("== fin ==\n");
    return 0;
}
