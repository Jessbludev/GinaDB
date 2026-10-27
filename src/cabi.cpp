// GinaDB Lite 0.7 — cabi.cpp: C ABI implementation over the C++ engine.
#include "gina/cabi.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <type_traits>

#include "gina/db.hpp"
#include "json.hpp"

namespace {

using gina::Bytes;
using gina::ChangeList;
using gina::Database;
using gina::Error;
using gina::Event;
using gina::Metric;
using gina::Options;
using gina::Row;
using gina::TableDef;
using gina::Value;
using gina::Vec;

thread_local std::string g_last_error;
thread_local int32_t g_last_status = GINA_OK;

struct DbHandle {
    std::unique_ptr<Database> db;
};
static DbHandle* H(gina_db* db) { return reinterpret_cast<DbHandle*>(db); }

template <class F>
auto guard(F&& fn) -> decltype(fn()) {
    try {
        g_last_status = GINA_OK;
        g_last_error.clear();
        return fn();
    } catch (const Error& e) {
        switch (e.code()) {
            case Error::Code::NoSuchTable: g_last_status = GINA_ENOENT; break;
            case Error::Code::NotFound: g_last_status = GINA_ENOENT; break;
            case Error::Code::TableExists: g_last_status = GINA_EEXISTS; break;
            case Error::Code::SchemaViolation: g_last_status = GINA_ESCHEMA; break;
            case Error::Code::BadKey: g_last_status = GINA_EKEY; break;
            case Error::Code::TxDiscarded: g_last_status = GINA_ETX; break;
            case Error::Code::IoError: g_last_status = GINA_EIO; break;
            case Error::Code::Corruption: g_last_status = GINA_ECORRUPT; break;
            case Error::Code::LimitExceeded: g_last_status = GINA_ELIMIT; break;
            case Error::Code::BadArgument: g_last_status = GINA_EARG; break;
        }
        g_last_error = e.what();
        using R = decltype(fn());
        if constexpr (std::is_integral_v<R>) return static_cast<R>(g_last_status);
        else if constexpr (std::is_pointer_v<R>) return static_cast<R>(nullptr);
        else return R{};
    } catch (const std::exception& e) {
        g_last_status = GINA_ERROR;
        g_last_error = e.what();
        using R = decltype(fn());
        if constexpr (std::is_integral_v<R>) return static_cast<R>(g_last_status);
        else if constexpr (std::is_pointer_v<R>) return static_cast<R>(nullptr);
        else return R{};
    } catch (...) {
        g_last_status = GINA_ERROR;
        g_last_error = "unknown error";
        using R = decltype(fn());
        if constexpr (std::is_integral_v<R>) return static_cast<R>(g_last_status);
        else if constexpr (std::is_pointer_v<R>) return static_cast<R>(nullptr);
        else return R{};
    }
}

// Returns a malloc'd copy of `s`, or nullptr on OOM.
[[nodiscard]] char* dup_cstr(const std::string& s) {
    auto* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (!p) return nullptr;
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

} // namespace

extern "C" {

gina_db* gina_open(const char* dir, int32_t durable, uint32_t checkpoint_every,
                   uint64_t changelog_capacity, uint64_t max_blob_size) {
    return guard([&]() -> gina_db* {
        Options o;
        o.durable = durable != 0;
        o.checkpoint_every = checkpoint_every;
        o.changelog_capacity = static_cast<std::size_t>(changelog_capacity);
        if (max_blob_size) o.max_blob_size = static_cast<std::size_t>(max_blob_size);
        auto* h = new DbHandle{};
        h->db = std::make_unique<Database>(dir ? dir : "gina-data", o);
        return reinterpret_cast<gina_db*>(h);
    });
}

void gina_close(gina_db* db) {
    if (!db) return;
    guard([&]() { delete H(db); return 0; });
}

int32_t gina_create_table(gina_db* db, const char* name, const char* json_schema) {
    if (!db || !name) { g_last_status = GINA_EARG; g_last_error = "null argument"; return GINA_EARG; }
    return guard([&]() -> int32_t {
        TableDef def = json_schema ? gina::json::decode_schema(json_schema)
                                   : TableDef{};
        H(db)->db->operator[](name).create(std::move(def));
        return GINA_OK;
    });
}

int32_t gina_drop_table(gina_db* db, const char* name) {
    if (!db || !name) { g_last_status = GINA_EARG; g_last_error = "null argument"; return GINA_EARG; }
    return guard([&]() -> int32_t {
        H(db)->db->operator[](name).drop();
        return GINA_OK;
    });
}

char* gina_schema(gina_db* db, const char* table) {
    if (!db || !table) { g_last_status = GINA_EARG; g_last_error = "null argument"; return nullptr; }
    return guard([&]() -> char* {
        return dup_cstr(gina::json::encode_schema(
            H(db)->db->operator[](table).schema()));
    });
}

int32_t gina_insert(gina_db* db, const char* table, const char* json_row) {
    if (!db || !table || !json_row) { g_last_status = GINA_EARG; g_last_error = "null argument"; return GINA_EARG; }
    return guard([&]() -> int32_t {
        H(db)->db->operator[](table).insert(gina::json::decode_row(json_row));
        return GINA_OK;
    });
}

int32_t gina_update(gina_db* db, const char* table, const char* key,
                    const char* json_patch) {
    if (!db || !table || !key || !json_patch) { g_last_status = GINA_EARG; g_last_error = "null argument"; return GINA_EARG; }
    return guard([&]() -> int32_t {
        if (!H(db)->db->operator[](table).update(key,
                                              gina::json::decode_row(json_patch))) {
            g_last_status = GINA_ENOENT;
            g_last_error = "key not found";
            return GINA_ENOENT;
        }
        return GINA_OK;
    });
}

int32_t gina_erase(gina_db* db, const char* table, const char* key) {
    if (!db || !table || !key) { g_last_status = GINA_EARG; g_last_error = "null argument"; return GINA_EARG; }
    return guard([&]() -> int32_t {
        return H(db)->db->operator[](table).erase(key) ? 1 : 0;
    });
}

char* gina_get(gina_db* db, const char* table, const char* key) {
    if (!db || !table || !key) { g_last_status = GINA_EARG; g_last_error = "null argument"; return nullptr; }
    return guard([&]() -> char* {
        const auto row = H(db)->db->operator[](table).get(key);
        if (!row) {
            g_last_status = GINA_ENOENT;
            g_last_error = "key not found";
            return static_cast<char*>(nullptr);
        }
        return dup_cstr(gina::json::encode_row(*row));
    });
}

char* gina_all(gina_db* db, const char* table) {
    if (!db || !table) { g_last_status = GINA_EARG; g_last_error = "null argument"; return nullptr; }
    return guard([&]() -> char* {
        return dup_cstr(gina::json::encode_rows(
            H(db)->db->operator[](table).all()));
    });
}

int64_t gina_count(gina_db* db, const char* table) {
    if (!db || !table) { g_last_status = GINA_EARG; g_last_error = "null argument"; return -1; }
    return guard([&]() -> int64_t {
        return static_cast<int64_t>(H(db)->db->operator[](table).count());
    });
}

int32_t gina_exec(gina_db* db, const char* json_ops) {
    if (!db || !json_ops) { g_last_status = GINA_EARG; g_last_error = "null argument"; return GINA_EARG; }
    return guard([&]() -> int32_t {
        const Value ops = gina::json::decode(json_ops);
        if (!ops.is_array())
            throw Error(Error::Code::BadArgument, "expected a JSON array of ops");
        auto tx = H(db)->db->begin();
        for (const auto& op : ops.as_array()) {
            if (!op.is_object())
                throw Error(Error::Code::BadArgument, "op must be an object");
            const auto& o = op.as_object();
            const auto t = o.find("table");
            if (t == o.end() || !t->second.is_text())
                throw Error(Error::Code::BadArgument, "op missing 'table'");
            const auto kind = o.find("op");
            if (kind == o.end() || !kind->second.is_text())
                throw Error(Error::Code::BadArgument, "op missing 'op'");
            if (kind->second.as_text() == "put") {
                const auto r = o.find("row");
                if (r == o.end() || !r->second.is_object())
                    throw Error(Error::Code::BadArgument, "put op missing 'row'");
                tx[t->second.as_text()].insert(r->second.as_object());
            } else if (kind->second.as_text() == "del") {
                const auto k = o.find("key");
                if (k == o.end() || !k->second.is_text())
                    throw Error(Error::Code::BadArgument, "del op missing 'key'");
                tx[t->second.as_text()].erase(k->second.as_text());
            } else {
                throw Error(Error::Code::BadArgument, "unknown op '" +
                                                          kind->second.as_text() + "'");
            }
        }
        tx.commit();
        return GINA_OK;
    });
}

char* gina_knn(gina_db* db, const char* table, const char* column,
               const float* query, uint32_t dims, uint32_t k, int32_t metric) {
    if (!db || !table || !column || (!query && dims)) {
        g_last_status = GINA_EARG; g_last_error = "null argument"; return nullptr;
    }
    return guard([&]() -> char* {
        Vec q(query, query + dims);
        return dup_cstr(gina::json::encode_knn(H(db)->db->operator[](table).knn(
            column, q, k, metric == 1 ? Metric::L2 : Metric::Cosine)));
    });
}

char* gina_search(gina_db* db, const char* table, const char* column,
                  const char* query, uint32_t limit) {
    if (!db || !table || !column || !query) { g_last_status = GINA_EARG; g_last_error = "null argument"; return nullptr; }
    return guard([&]() -> char* {
        return dup_cstr(gina::json::encode_rows(
            H(db)->db->operator[](table).search(column, query, limit ? limit : 50)));
    });
}

char* gina_changes(gina_db* db, const char* table, uint64_t since) {
    if (!db || !table) { g_last_status = GINA_EARG; g_last_error = "null argument"; return nullptr; }
    return guard([&]() -> char* {
        return dup_cstr(gina::json::encode_changes(
            H(db)->db->operator[](table).changes(since)));
    });
}

int64_t gina_watch(gina_db* db, const char* table,
                   void (*cb)(const char*, void*), void* userdata) {
    if (!db || !table || !cb) { g_last_status = GINA_EARG; g_last_error = "null argument"; return -1; }
    return guard([&]() -> int64_t {
        return H(db)->db->operator[](table).watch([cb, userdata](const Event& e) {
            const std::string json = gina::json::encode_event(e);
            cb(json.c_str(), userdata);
        });
    });
}

int32_t gina_unwatch(gina_db* db, int64_t watch_id) {
    if (!db) { g_last_status = GINA_EARG; g_last_error = "null argument"; return GINA_EARG; }
    return guard([&]() -> int32_t {
        // watch ids are engine-global; any TableRef can remove them
        H(db)->db->operator[]("").unwatch(watch_id);
        return GINA_OK;
    });
}

int32_t gina_checkpoint(gina_db* db) {
    if (!db) { g_last_status = GINA_EARG; g_last_error = "null argument"; return GINA_EARG; }
    return guard([&]() -> int32_t {
        H(db)->db->checkpoint();
        return GINA_OK;
    });
}

int32_t gina_compact(gina_db* db) {
    if (!db) { g_last_status = GINA_EARG; g_last_error = "null argument"; return GINA_EARG; }
    return guard([&]() -> int32_t {
        H(db)->db->compact();
        return GINA_OK;
    });
}

char* gina_select(gina_db* db, const char* table, const char* json_query) {
    if (!db || !table) { g_last_status = GINA_EARG; g_last_error = "null argument"; return nullptr; }
    return guard([&]() -> char* {
        const auto q = gina::json::decode_select(json_query ? json_query : "{}");
        auto t = H(db)->db->operator[](table);
        std::vector<Row> rows;
        if (q.where) {
            auto query = t.where(*q.where);
            for (const auto& [col, desc] : q.order)
                query.order_by(col, desc);
            if (q.limit != static_cast<std::size_t>(-1))
                query.limit(q.limit);
            rows = query.collect();
        } else if (!q.order.empty() || q.limit != static_cast<std::size_t>(-1)) {
            auto query = t.where([](const Row&) { return true; });
            for (const auto& [col, desc] : q.order)
                query.order_by(col, desc);
            if (q.limit != static_cast<std::size_t>(-1))
                query.limit(q.limit);
            rows = query.collect();
        } else {
            rows = t.all();
        }
        if (!q.fields.empty()) {  // proyección
            std::vector<Row> proj;
            proj.reserve(rows.size());
            for (const auto& r : rows) {
                Row p;
                for (const auto& f : q.fields) {
                    auto it = r.find(f);
                    if (it != r.end()) p.emplace(f, it->second);
                }
                if (!p.empty()) proj.push_back(std::move(p));
            }
            rows = std::move(proj);
        }
        return dup_cstr(gina::json::encode_rows(rows));
    });
}

int32_t gina_create_index(gina_db* db, const char* table, const char* column) {
    if (!db || !table || !column) { g_last_status = GINA_EARG; g_last_error = "null argument"; return GINA_EARG; }
    return guard([&]() -> int32_t {
        H(db)->db->operator[](table).create_index(column);
        return GINA_OK;
    });
}

int32_t gina_drop_index(gina_db* db, const char* table, const char* column) {
    if (!db || !table || !column) { g_last_status = GINA_EARG; g_last_error = "null argument"; return GINA_EARG; }
    return guard([&]() -> int32_t {
        H(db)->db->operator[](table).drop_index(column);
        return GINA_OK;
    });
}

char* gina_stats(gina_db* db) {
    if (!db) { g_last_status = GINA_EARG; g_last_error = "null argument"; return nullptr; }
    return guard([&]() -> char* {
        return dup_cstr(gina::json::encode_stats(H(db)->db->stats()));
    });
}

char* gina_tables(gina_db* db) {
    if (!db) { g_last_status = GINA_EARG; g_last_error = "null argument"; return nullptr; }
    return guard([&]() -> char* {
        std::string out = "[";
        const auto names = H(db)->db->tables();
        for (std::size_t i = 0; i < names.size(); i++) {
            if (i) out.push_back(',');
            out += '"' + names[i] + '"';
        }
        out += "]";
        return dup_cstr(out);
    });
}

void gina_free(char* s) { std::free(s); }

const char* gina_version(void) { return "GinaDB Lite 0.7"; }

const char* gina_last_error(gina_db*) { return g_last_error.c_str(); }

int32_t gina_last_status(void) { return g_last_status; }

} // extern "C"
