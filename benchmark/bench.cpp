// GinaDB Lite 0.7 — benchmark suite (same machine/flags methodology as 0.4).
// Measures: write, read/get, select/all, transaction, recovery (reopen),
// knn (dim 64), full-text search. Emits a markdown table + CSV.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "gina/db.hpp"

using namespace gina;
using Clock = std::chrono::high_resolution_clock;

static double elapsed_since(Clock::time_point& t) {
    const auto now = Clock::now();
    const double s = std::chrono::duration<double>(now - t).count();
    t = now;
    return s;
}

struct Row95 {
    std::string test;
    std::size_t ops;
    double seconds;
    double p50, p95;
    std::size_t data_bytes;
};

static std::vector<Row95> g_rows;

static void record(const std::string& name, std::size_t ops, double secs,
                   std::vector<double>& lat_ms, std::size_t data_bytes = 0) {
    std::sort(lat_ms.begin(), lat_ms.end());
    const auto q = [&](double p) {
        if (lat_ms.empty()) return 0.0;
        const auto i = std::min<std::size_t>(
            static_cast<std::size_t>(p * lat_ms.size()), lat_ms.size() - 1);
        return lat_ms[i];
    };
    g_rows.push_back({name, ops, secs, q(0.50), q(0.95), data_bytes});
    std::printf("  %-28s %10zu ops  %10.2f ops/s  p50=%8.4fms p95=%8.4fms\n",
                name.c_str(), ops, secs > 0 ? ops / secs : 0.0, q(0.50), q(0.95));
}

int main() {
    std::printf("GinaDB Lite 0.5 benchmark (C++20, same-machine comparison)\n\n");
    const std::string dir = "bench-data";
    std::mt19937 rng(42);

    for (const int scale : {100, 1000, 10000}) {
        std::printf("[scale %d]\n", scale);
        std::filesystem::remove_all(dir);
        {
            Database db{dir, Options{.checkpoint_every = 0}};
            db["users"].create({.primary_key = "id",
                                .columns{{.name = "id", .type = T::Text, .nullable = false},
                                         {.name = "name", .type = T::Text},
                                         {.name = "age", .type = T::Int},
                                         {.name = "bio", .type = T::Text, .fts = true}}});

            // write (single-row tx per op, same as 0.4 benchmark)
            {
                std::vector<double> lat;
                auto t = Clock::now();
                double total = 0;
                for (int i = 0; i < scale; i++) {
                    const auto a = Clock::now();
                    db["users"].emplace("id", "w" + std::to_string(i),
                                        "name", "u" + std::to_string(i),
                                        "age", static_cast<std::int64_t>(i % 90),
                                        "bio", "bio numero " + std::to_string(i));
                    lat.push_back(std::chrono::duration<double, std::milli>(
                                      Clock::now() - a)
                                      .count());
                }
                total = elapsed_since(t);
                record("write", static_cast<std::size_t>(scale), total, lat);
            }

            // read/get
            {
                const std::size_t reads =
                    static_cast<std::size_t>(scale) * 10;
                std::vector<double> lat;
                auto t = Clock::now();
                for (std::size_t i = 0; i < reads; i++) {
                    const auto a = Clock::now();
                    const auto r = db["users"].get("w" + std::to_string(rng() % scale));
                    if (!r) std::abort();
                    lat.push_back(std::chrono::duration<double, std::milli>(
                                      Clock::now() - a)
                                      .count());
                }
                const double total = elapsed_since(t);
                record("get", reads, total, lat);
            }

            // select/all (10 repetitions)
            {
                std::vector<double> lat;
                auto t = Clock::now();
                double total = 0;
                for (int i = 0; i < 10; i++) {
                    const auto a = Clock::now();
                    const auto all = db["users"].all();
                    if (all.size() != static_cast<std::size_t>(scale))
                        std::abort();
                    lat.push_back(std::chrono::duration<double, std::milli>(
                                      Clock::now() - a)
                                      .count());
                }
                total = elapsed_since(t);
                record("all(x10)", 10, total, lat);
            }

            // transaction (10 ops per tx, same as 0.4 benchmark style)
            {
                std::vector<double> lat;
                auto t = Clock::now();
                double total = 0;
                for (int i = 0; i < scale; i++) {
                    const auto a = Clock::now();
                    auto tx = db.begin();
                    tx["users"].emplace("id", "t" + std::to_string(i),
                                        "name", "t" + std::to_string(i),
                                        "age", static_cast<std::int64_t>(i),
                                        "bio", "tx bio " + std::to_string(i));
                    tx.commit();
                    lat.push_back(std::chrono::duration<double, std::milli>(
                                      Clock::now() - a)
                                      .count());
                }
                total = elapsed_since(t);
                record("transaction", static_cast<std::size_t>(scale), total, lat);
            }

            // full-text search (fts column)
            {
                std::vector<double> lat;
                auto t = Clock::now();
                double total = 0;
                const std::size_t n = 200;
                for (std::size_t i = 0; i < n; i++) {
                    const auto a = Clock::now();
                    const auto hits = db["users"].search("bio", "base datos");
                    (void)hits;
                    lat.push_back(std::chrono::duration<double, std::milli>(
                                      Clock::now() - a)
                                      .count());
                }
                total = elapsed_since(t);
                record("search(fts)", n, total, lat);
            }
        }  // close → destructor checkpoints

        // recovery ×10 (startup from snapshot + short WAL)
        {
            std::vector<double> lat;
            auto t = Clock::now();
            double total = 0;
            for (int i = 0; i < 10; i++) {
                const auto a = Clock::now();
                Database db{dir};
                if (db["users"].count() != static_cast<std::size_t>(scale) * 2)
                    std::abort();
                lat.push_back(std::chrono::duration<double, std::milli>(
                                  Clock::now() - a)
                                  .count());
            }
            total = elapsed_since(t);
            record("recovery(x10)", 10, total, lat);
        }
        std::printf("\n");
    }

    // ---- KNN (dim 64, 1k and 10k rows) --------------------------------------
    for (const int scale : {1000, 10000}) {
        const std::string kdir = "bench-knn";
        std::filesystem::remove_all(kdir);
        Database db{kdir, Options{.checkpoint_every = 0}};
        db["media"].create({.primary_key = "id",
                            .columns{{.name = "id", .type = T::Text, .nullable = false},
                                     {.name = "tag", .type = T::Text},
                                     {.name = "e", .type = T::Vector}}});
        std::mt19937 krng(7);
        std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
        for (int i = 0; i < scale; i++) {
            Vec v(64);
            for (auto& f : v) f = uni(krng);
            db["media"].emplace("id", "m" + std::to_string(i), "tag",
                                "t" + std::to_string(i % 10), "e", v);
        }
        Vec q(64);
        for (auto& f : q) f = uni(krng);
        std::vector<double> lat;
        auto t = Clock::now();
        double total = 0;
        const std::size_t n = 100;
        for (std::size_t i = 0; i < n; i++) {
            const auto a = Clock::now();
            const auto nn = db["media"].knn("e", q, 10);
            if (nn.size() != 10) std::abort();
            lat.push_back(std::chrono::duration<double, std::milli>(
                              Clock::now() - a)
                              .count());
        }
        total = elapsed_since(t);
        std::printf("[knn dim=64 rows=%d]\n", scale);
        record("knn(k=10)", n, total, lat);
        std::printf("\n");
    }

    // ---- CSV ------------------------------------------------------------------
    std::printf("--- CSV ---\n");
    std::printf("test,ops,seconds,ops_per_s,p50_ms,p95_ms\n");
    for (const auto& r : g_rows)
        std::printf("%s,%zu,%.6f,%.2f,%.4f,%.4f\n", r.test.c_str(), r.ops,
                    r.seconds, r.seconds > 0 ? r.ops / r.seconds : 0.0, r.p50,
                    r.p95);
    std::printf("\nbenchmark completed\n");
    return 0;
}
