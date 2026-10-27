// GinaDB Lite 0.7 — db.cpp: engine core (WAL, apply, recovery, checkpoints).
#include "impl.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <thread>

namespace gina {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::string keyof(const TableDef& def, const Row& r,
                                bool legacy_fallback) {
    auto it = r.find(def.primary_key);
    if (it != r.end() && it->second.is_text() && !it->second.as_text().empty())
        return it->second.as_text();
    if (legacy_fallback) {  // 0.4 keyed everything by "id"
        auto id = r.find("id");
        if (id != r.end() && id->second.is_text()) return id->second.as_text();
        return {};
    }
    if (it == r.end())
        throw Error(Error::Code::BadKey,
                    "row missing primary key '" + def.primary_key + "'");
    if (!it->second.is_text())
        throw Error(Error::Code::BadKey,
                    "primary key '" + def.primary_key + "' must be text");
    throw Error(Error::Code::BadKey, "primary key must not be empty");
}

[[nodiscard]] Row validate_row(const TableDef& def, Row row, std::size_t max_blob,
                               bool partial) {
    if (row.empty())
        throw Error(Error::Code::SchemaViolation, "empty row");
    auto pk = row.find(def.primary_key);
    if (!partial) {
        if (pk == row.end())
            throw Error(Error::Code::BadKey,
                        "row missing primary key '" + def.primary_key + "'");
        if (!pk->second.is_text())
            throw Error(Error::Code::BadKey,
                        "primary key '" + def.primary_key + "' must be text");
        if (pk->second.as_text().empty())
            throw Error(Error::Code::BadKey, "primary key must not be empty");
    } else if (pk != row.end()) {
        throw Error(Error::Code::BadKey, "primary key is immutable");
    }

    for (auto& [k, v] : row) {
        if (v.is_bytes() && v.as_bytes().size() > max_blob)
            throw Error(Error::Code::LimitExceeded,
                        "blob '" + k + "' exceeds max_blob_size");
        if (def.schemaless) continue;
        const Column* col = nullptr;
        for (const auto& c : def.columns)
            if (c.name == k) { col = &c; break; }
        if (!col)
            throw Error(Error::Code::SchemaViolation,
                        "unknown column '" + k + "' in table '" + def.name + "'");
        if (v.is_null()) {
            if (!col->nullable)
                throw Error(Error::Code::SchemaViolation,
                            "column '" + k + "' is not nullable");
            continue;
        }
        if (v.type() == col->type) continue;
        if (col->type == Type::Float && v.is_int()) {  // widening: int -> float
            v = Value(static_cast<double>(v.as_int()));
            continue;
        }
        throw Error(Error::Code::SchemaViolation,
                    "column '" + k + "' expects " + type_name(col->type) +
                        ", got " + type_name(v.type()));
    }
    return row;
}

// ---------------------------------------------------------------------------
// secondary index maintenance
// ---------------------------------------------------------------------------

void DbEngine::sec_build(Table& t, unsigned nthreads) {
    t.sec.clear();
    for (const auto& col : t.def.indexes) t.sec[col] = SecTree{};
    if (t.def.indexes.empty()) return;
#ifdef GINA_TRACE
    const auto tsb0 = std::chrono::steady_clock::now();
#endif
    // 0.7: UNA sola pasada por todas las filas recolectando (enc, pk) por
    // índice; luego sort + bulk_load — mucho más rápido que acumular en un
    // std::map por índice. Las filas vienen ordenadas por pk, así que los pk
    // de cada run quedan ordenados (invariante que exige el bulk_load).
    const std::size_t nix = t.def.indexes.size();
    std::vector<std::vector<std::pair<std::string, std::string>>> acc(nix);
    for (auto& a : acc) a.reserve(t.rows.size());
    t.rows.for_each([&](const std::string& key, const SRow& r) {
        for (std::size_t i = 0; i < nix; i++) {
            const Value* v = srow_get(t.cmap, r, t.def.indexes[i]);
            if (!v) return btree::Visit::Next;
            const auto enc = idx::encode(*v);
            if (!enc.empty()) acc[i].emplace_back(std::move(enc), key);
        }
        return btree::Visit::Next;
    });
#ifdef GINA_TRACE
    const auto tsb1 = std::chrono::steady_clock::now();
#endif
    // árboles por índice ya creados: punteros estables (no se inserta nada
    // más en t.sec, así que no hay rehash) — cada hilo trabaja el suyo
    std::vector<SecTree*> trees(nix);
    for (std::size_t i = 0; i < nix; i++) trees[i] = &t.sec[t.def.indexes[i]];
    auto build_one = [&](std::size_t i) {
        auto& pairs = acc[i];
        std::sort(pairs.begin(), pairs.end());
        // runs por clave idéntica → vector de pks (ya ordenados por pk)
        std::vector<std::pair<std::string, std::vector<std::string>>> sorted;
        sorted.reserve(pairs.size());
        for (std::size_t j = 0; j < pairs.size();) {
            std::size_t e = j;
            std::vector<std::string> pks;
            pks.push_back(std::move(pairs[j].second));
            for (e = j + 1; e < pairs.size() && pairs[e].first == pairs[j].first;
                 e++)
                pks.push_back(std::move(pairs[e].second));
            sorted.emplace_back(std::move(pairs[j].first), std::move(pks));
            j = e;
        }
        trees[i]->bulk_load(std::move(sorted));
    };
    // sort+build por índice en paralelo (work-stealing) cuando conviene:
    // varios índices, tabla grande y hilos disponibles. La colecta fue una
    // sola pasada compartida; los árboles son independientes entre sí.
    unsigned nt = nthreads > 4 ? 4 : nthreads;
    const unsigned hw = std::thread::hardware_concurrency();
    if (nt > hw) nt = hw;
    if (nt <= 1 || nix == 1 || t.rows.size() < 8192) {
        for (std::size_t i = 0; i < nix; i++) build_one(i);
    } else {
        std::atomic<std::size_t> next{0};
        std::mutex err_mu;
        std::exception_ptr err;
        std::vector<std::thread> ts;
        ts.reserve(nt);
        for (unsigned k = 0; k < nt; k++)
            ts.emplace_back([&] {
                for (;;) {
                    const std::size_t i = next.fetch_add(1);
                    if (i >= nix) break;
                    try {
                        build_one(i);
                    } catch (...) {
                        std::lock_guard<std::mutex> g(err_mu);
                        if (!err) err = std::current_exception();
                    }
                }
            });
        for (auto& th : ts) th.join();
        if (err) std::rethrow_exception(err);
    }
#ifdef GINA_TRACE
    std::fprintf(stderr,
                 "[SEC] %-20s %7.1f ms (%.1f colecta) %zu índices, hilos=%u\n",
                 t.def.name.c_str(),
                 std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - tsb0).count(),
                 std::chrono::duration<double, std::milli>(tsb1 - tsb0).count(),
                 nix, nt);
#endif
}

void DbEngine::fts_build(Table& t) {
    if (t.fts.empty()) { t.fts_built = true; return; }
#ifdef GINA_TRACE
    const auto tf0 = std::chrono::steady_clock::now();
#endif
    for (auto& [col, idx] : t.fts) {
        idx = {};  // rebuild: índice limpio
        t.rows.for_each([&](const std::string& key, const SRow& r) {
            const Value* v = srow_get(t.cmap, r, col);
            if (v && v->is_text()) idx.index(key, v->as_text());
            return btree::Visit::Next;
        });
    }
    t.fts_built = true;
#ifdef GINA_TRACE
    std::fprintf(stderr, "[FTS] %-20s %7.1f ms (build)\n", t.def.name.c_str(),
                 std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - tf0).count());
#endif
}

void DbEngine::sec_add(Table& t, const std::string& key, const SRow& r) {
    for (auto& [col, tree] : t.sec) {
        const Value* v = srow_get(t.cmap, r, col);
        if (!v) continue;
        const auto enc = idx::encode(*v);
        if (enc.empty()) continue;
        auto* pks = tree.find(enc);
        if (pks) {
            pks->insert(std::lower_bound(pks->begin(), pks->end(), key), key);
        } else {
            tree.insert_or_assign(enc, std::vector<std::string>{key});
        }
    }
}

void DbEngine::sec_remove(Table& t, const std::string& key, const SRow& r) {
    for (auto& [col, tree] : t.sec) {
        const Value* v = srow_get(t.cmap, r, col);
        if (!v) continue;
        const auto enc = idx::encode(*v);
        if (enc.empty()) continue;
        auto* pks = tree.find(enc);
        if (!pks) continue;  // inconsistencia silenciosa imposible salvo corrupción
        auto pos = std::lower_bound(pks->begin(), pks->end(), key);
        if (pos != pks->end() && *pos == key) pks->erase(pos);
        if (pks->empty()) tree.erase(enc);
    }
}

// Registra una tabla nueva (cmap+fts construidos). Sin locks: recovery o
// catalog_mu ya tomada por el llamador.
DbEngine::Table* DbEngine::adopt_table(const TableDef& def, std::uint32_t id) {
    auto t = std::make_unique<Table>();
    t->def = def;
    t->id = id;
    t->rebuild_cmap();
    for (const auto& c : t->def.columns)
        if (c.fts) t->fts[c.name] = {};
    if (id + 1 > next_table_id) next_table_id = id + 1;
    Table* raw = t.get();
    by_id[id] = raw;
    tables.emplace(t->def.name, std::move(t));
    return raw;
}

// ---------------------------------------------------------------------------
// DbEngine — lifecycle
// ---------------------------------------------------------------------------

DbEngine::DbEngine(std::filesystem::path d, Options o)
    : dir(std::move(d)), opt(o) {
    std::filesystem::create_directories(dir);
    data_path = dir / "data.pages";
    wal_path = dir / "wal.log";
    delta_path = dir / "delta.pages";
    std::error_code ec;  // remove stale temporaries from a crashed checkpoint
    std::filesystem::remove(data_path.string() + ".tmp", ec);
    recover();
    wal_fd = ::open(wal_path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC,
                    0644);
    if (wal_fd < 0)
        throw Error(Error::Code::IoError, "cannot open WAL " + wal_path.string());
}

DbEngine::~DbEngine() {
    try {
        if (opt.checkpoint_on_close && commits_since_ckpt > 0) {
            std::lock_guard<std::mutex> w(writer_mu);
            checkpoint_locked();
        }
    } catch (...) {  // destructor must not throw; WAL replay still recovers
    }
    if (wal_fd >= 0) ::close(wal_fd);
}

[[nodiscard]] DbEngine::Table* DbEngine::must_table(
    const std::string& name) const {
    if (auto* t = find_table(name)) return t;
    throw Error(Error::Code::NoSuchTable, "table '" + name + "' does not exist");
}

// ---------------------------------------------------------------------------
// WAL
// ---------------------------------------------------------------------------

static void write_all(int fd, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    std::size_t off = 0;
    while (off < n) {
        const auto w = ::write(fd, b + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            throw Error(Error::Code::IoError,
                        std::string("wal write: ") + std::strerror(errno));
        }
        off += static_cast<std::size_t>(w);
    }
}

void DbEngine::append_frame(std::uint8_t kind, std::uint64_t tx,
                                const std::vector<std::uint8_t>& payload) {
    const auto frame = codec::encode_frame(kind, ++lsn, tx, payload);
    write_all(wal_fd, frame.data(), frame.size());
}

void DbEngine::sync_wal() {
    if (opt.durable && wal_fd >= 0 && ::fsync(wal_fd) != 0)
        throw Error(Error::Code::IoError,
                    std::string("wal fsync: ") + std::strerror(errno));
}

void DbEngine::rebase_wal(std::uint64_t snapshot_lsn) {
    if (wal_fd >= 0) { ::close(wal_fd); wal_fd = -1; }
    const int fd = ::open(wal_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC |
                                             O_CLOEXEC, 0644);
    if (fd < 0)
        throw Error(Error::Code::IoError, "cannot rebase WAL");
    std::vector<std::uint8_t> payload;
    codec::put64(payload, snapshot_lsn);
    const auto frame = codec::encode_frame(codec::K_BASE, ++lsn, 0, payload);
    write_all(fd, frame.data(), frame.size());
    if (opt.durable) ::fsync(fd);
    wal_fd = fd;
}

// ---------------------------------------------------------------------------
// apply
// ---------------------------------------------------------------------------

void DbEngine::index_row(Table& t, const std::string& key,
                           const SRow* old_row, const SRow* new_row) {
    if (t.fts.empty() || !t.fts_built) return;  // perezoso: aún sin construir
    for (auto& [col, idx] : t.fts) {
        if (old_row) {
            if (const Value* v = srow_get(t.cmap, *old_row, col);
                v && v->is_text())
                idx.remove(key, v->as_text());
        }
        if (new_row) {
            if (const Value* v = srow_get(t.cmap, *new_row, col);
                v && v->is_text())
                idx.index(key, v->as_text());
        }
    }
}

void DbEngine::log_changes(Table& t, const std::vector<Event>& events) {
    if (opt.changelog_capacity == 0 || events.empty()) return;
    std::lock_guard<std::mutex> l(t.log_mu);
    for (const auto& e : events) {
        if (e.table != t.def.name) continue;
        t.log.push_back({++t.last_seq, e});
        while (t.log.size() > opt.changelog_capacity) t.log.pop_front();
    }
}

// PUT compartido por WAL-replay, delta-apply y commit. t->mu en exclusiva.
void DbEngine::apply_row(Table& t, const std::string& key, SRow row,
                           std::vector<Event>* events, const std::string& tname) {
    const SRow* oldp = t.rows.find(key);
    if (oldp) sec_remove(t, key, *oldp);
    sec_add(t, key, row);               // antes de mover la fila
    index_row(t, key, oldp, &row);      // FTS: fuera la vieja, dentro la nueva
    const bool fresh = t.rows.insert_or_assign(key, std::move(row)).second;
    if (events) {
        const SRow* stored = t.rows.find(key);
        events->push_back({Event::Kind::Put, tname, key,
                           stored ? row_from_srow(t.def, *stored) : Row{}});
    }
    (void)fresh;
}

// DEL compartido. Devuelve true si la fila existía. t->mu en exclusiva.
bool DbEngine::apply_erase(Table& t, const std::string& key,
                              std::vector<Event>* events, const std::string& tname) {
    const SRow* oldp = t.rows.find(key);
    if (!oldp) return false;
    sec_remove(t, key, *oldp);
    index_row(t, key, oldp, nullptr);
    t.rows.erase(key);
    if (events)
        events->push_back({Event::Kind::Del, tname, key, {}});
    return true;
}

// PK con tolerancia legacy (WAL 0.4): si falta la pk nombrada, prueba "id";
// si tampoco está, devuelve "" (el op se descarta — una fila sin pk no es
// direccionable). Para frames modernos propaga el error (corrupción real).
[[nodiscard]] static std::string srow_keyof_replay(const TableDef& def,
                                                  const ColMap& cm,
                                                  const SRow& r,
                                                  bool legacy) {
    try {
        return srow_keyof(def, cm, r);
    } catch (const Error&) {
        if (!legacy) throw;
        const Value* v = srow_get(cm, r, "id");
        return v && v->is_text() ? v->as_text() : std::string();
    }
}

void DbEngine::apply_op(std::uint8_t kind,
                            const std::vector<std::uint8_t>& payload,
                            std::vector<Event>* events, bool legacy) {
    std::size_t pos = 0;
    const std::string name =
        codec::get_str(payload.data(), payload.size(), pos);

    if (kind == codec::K_SCHEMA || kind == codec::K_RESCH) {
        std::size_t p0 = 0;  // decode_schema re-reads the name from offset 0
        TableDef def =
            codec::decode_schema(payload.data(), payload.size(), p0, legacy);
        std::unique_lock cat(catalog_mu);
        auto it = tables.find(def.name);
        if (kind == codec::K_SCHEMA && it != tables.end()) return;
        if (it != tables.end()) {  // K_RESCH: re-schema (índices) en vivo
            Table* t = it->second.get();
            std::unique_lock lock(t->mu);
            TableDef old_def = t->def;
            t->def = def;
            t->rebuild_cmap();
            bool cols_same = old_def.columns.size() == def.columns.size();
            for (std::size_t i = 0; cols_same && i < old_def.columns.size(); i++)
                cols_same = old_def.columns[i].name == def.columns[i].name;
            if (!cols_same)  // redistribuir filas planas (no descarta datos)
                t->rows.for_each_mut([&](const std::string&, SRow& r) {
                    srow_rewidth(old_def, t->def, t->cmap, r);
                    return btree::Visit::Next;
                });
            sec_build(*t, open_threads);
            if (!cols_same) t->fts_built = false;  // FTS perezoso: invalidar
            catalog_dirty[def.name] = {false, t->id};
            return;
        }
        Table* t = adopt_table(def, next_table_id++);
        sec_build(*t, open_threads);  // árboles de índices declarados en el create
        catalog_dirty[def.name] = {false, t->id};
        return;
    }
    if (kind == codec::K_DROP) {
        std::unique_lock cat(catalog_mu);
        auto it = tables.find(name);
        if (it == tables.end()) return;
        catalog_dirty[name] = {true, it->second->id};
        by_id.erase(it->second->id);
        tables.erase(it);
        return;
    }

    // PUT / DEL
    std::shared_lock cat(catalog_mu);
    auto it = tables.find(name);
    if (it == tables.end()) return;  // dropped earlier in the same tx
    Table* t = it->second.get();
    std::unique_lock lock(t->mu);
    if (kind == codec::K_PUT) {
        SRow row = codec::decode_srow(t->def, t->cmap, payload.data(),
                                      payload.size(), pos);
        const std::string key = srow_keyof_replay(t->def, t->cmap, row, legacy);
        if (key.empty()) return;  // legacy sin pk direccionable → descartar
        apply_row(*t, key, std::move(row), events, name);
        t->dirty.insert(key);
    } else if (kind == codec::K_DEL) {
        const std::string key =
            codec::get_str(payload.data(), payload.size(), pos);
        if (apply_erase(*t, key, events, name))
            t->dirty.insert(key);
    }
}

// ---------------------------------------------------------------------------
// commit pipeline
// ---------------------------------------------------------------------------

std::vector<Event> DbEngine::commit_ops_locked(
    std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> ops) {
    std::vector<Event> events;
    bool want_events = opt.changelog_capacity > 0;
    {
        std::shared_lock w(watch_mu);
        want_events = want_events || !watchers.empty();
    }
    const auto tx = ++tx_counter;
    for (const auto& [kind, payload] : ops) append_frame(kind, tx, payload);
    append_frame(codec::K_COMMIT, tx, {});
    sync_wal();  // durability barrier before touching memory
    for (const auto& [kind, payload] : ops)
        apply_op(kind, payload, want_events ? &events : nullptr, false);
    ++commits;
    ++commits_since_ckpt;
    if (want_events && !events.empty()) {
        std::set<std::string> touched;
        for (const auto& e : events) touched.insert(e.table);
        std::shared_lock cat(catalog_mu);
        for (const auto& tn : touched) {
            auto it = tables.find(tn);
            if (it != tables.end()) log_changes(*it->second, events);
        }
    }
    if (opt.checkpoint_every && commits_since_ckpt >= opt.checkpoint_every)
        checkpoint_locked();
    return events;  // caller dispatches after releasing writer_mu
}

void DbEngine::dispatch(const std::vector<Event>& events) {
    if (events.empty()) return;
    std::lock_guard<std::mutex> d(dispatch_mu);
    for (const auto& e : events) {
        std::vector<WatchFn> targets;
        {
            std::shared_lock w(watch_mu);
            for (const auto& [id, tw] : watchers)
                if (tw.first == e.table) targets.push_back(tw.second);
        }
        for (const auto& fn : targets) {
            try {
                fn(e);
            } catch (...) {  // user callbacks must not break the engine
            }
        }
    }
}

// ---------------------------------------------------------------------------
// recovery
// ---------------------------------------------------------------------------

// Decodifica los registros crudos del base a filas planas, en paralelo si
// open_threads > 1. El base llega (tabla asc, pk asc) — los rangos contiguos
// preservan ese orden al concatenarse, requisito del bulk_load.
void DbEngine::decode_base_records(
    const pager::LoadResult& res,
    std::unordered_map<std::uint32_t,
                       std::vector<std::pair<std::string, SRow>>>& out) {
    // tid → (def, cmap) resueltos UNA vez (cmap copiada: lectura sin locks)
    std::unordered_map<std::uint32_t, std::pair<const TableDef*, ColMap>> tdefs;
    tdefs.reserve(by_id.size());
    for (const auto& [tid, t] : by_id)
        tdefs.emplace(tid, std::make_pair(&t->def, t->cmap));

    auto decode_range = [&](std::size_t begin, std::size_t end, auto& sink) {
        for (std::size_t i = begin; i < end; i++) {
            const auto& rv = res.rows[i];
            const auto td = tdefs.find(rv.tid);
            if (td == tdefs.end())
                throw Error(Error::Code::Corruption,
                            "record with unknown table id");
            std::size_t pos = 0;
            SRow row = codec::decode_srow(*td->second.first,
                                          td->second.second, rv.body,
                                          rv.body_len, pos);
            std::string key;
            try {
                key = srow_keyof(*td->second.first, td->second.second, row);
            } catch (const Error&) {
                throw Error(Error::Code::Corruption,
                            "snapshot row without text pk");
            }
            sink[rv.tid].emplace_back(std::move(key), std::move(row));
        }
    };

    const std::size_t n = res.rows.size();
    std::uint32_t nth = open_threads > 4 ? 4 : open_threads;
    if (nth <= 1 || n < 4096) {
        decode_range(0, n, out);
        return;
    }
    // decode paralelo por rangos contiguos (excepciones: primera gana)
    using Part = std::unordered_map<std::uint32_t,
                                    std::vector<std::pair<std::string, SRow>>>;
    std::vector<Part> parts(nth);
    std::vector<std::exception_ptr> errs(nth);
    std::vector<std::thread> ts;
    ts.reserve(nth);
    for (std::uint32_t t = 0; t < nth; t++) {
        const std::size_t b = n * t / nth, e = n * (t + 1) / nth;
        ts.emplace_back([&, b, e, t] {
            try {
                decode_range(b, e, parts[t]);
            } catch (...) {
                errs[t] = std::current_exception();
            }
        });
    }
    for (auto& th : ts) th.join();
    for (auto& e : errs)
        if (e) std::rethrow_exception(e);
    // concatenar EN ORDEN → orden global preservado por tabla
    for (auto& part : parts)
        for (auto& [tid, vec] : part) {
            auto& dst = out[tid];
            dst.reserve(dst.size() + vec.size());
            for (auto& p : vec) dst.push_back(std::move(p));
        }
}

[[nodiscard]] std::uint64_t DbEngine::load_snapshot() {
    std::error_code ec;
    if (!std::filesystem::exists(data_path, ec)) return 0;
    {  // 0.4 legacy checkpoints were never read back — ignore them entirely
        std::ifstream f(data_path, std::ios::binary);
        char magic[4] = {0, 0, 0, 0};
        f.read(magic, 4);
        std::uint32_t m = 0;
        for (int i = 0; i < 4; i++)
            m |= static_cast<std::uint8_t>(magic[i]) << (i * 8);
        if (m == 0x50494E47u) return 0;  // "GNIP" 0.4 write-only checkpoint
    }
#ifdef GINA_TRACE
    const auto tr0 = std::chrono::steady_clock::now();
    auto rlap = [&](const char* what) {
        const auto t = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[RECOVER] %-22s %7.1f ms\n", what,
            std::chrono::duration<double, std::milli>(t - tr0).count());
    };
#endif
    // registros crudos: el pager valida páginas/crc y entrega vistas sin
    // decodificar — el decode a filas planas (paralelo) es cosa del motor
    const auto res = pager::load(
        data_path,
        [this](const TableDef& def, std::uint32_t id) {
            adopt_table(def, id);
        },
        nullptr);
#ifdef GINA_TRACE
    rlap("pager::load (crudo)");
#endif
    std::unordered_map<std::uint32_t,
                       std::vector<std::pair<std::string, SRow>>>
        pending;
    decode_base_records(res, pending);
#ifdef GINA_TRACE
    rlap("decode filas planas");
#endif
    for (auto& [tid, t] : by_id) {
        auto it = pending.find(tid);
        if (it != pending.end() && !it->second.empty())
            t->rows.bulk_load(std::move(it->second));
    }
#ifdef GINA_TRACE
    rlap("bulk_load btree");
#endif
    snap_pages = res.pages;
    snap_ovf = res.overflow_pages;
    snap_bytes = static_cast<std::uint64_t>(
        std::filesystem::file_size(data_path, ec));
    for (auto& [name, t] : tables) {
#ifdef GINA_TRACE
        const auto tt0 = std::chrono::steady_clock::now();
#endif
        // 0.7: FTS perezoso — NO se construye al abrir; lo paga la primera
        // búsqueda que lo use. El reopen solo construye los secundarios.
        sec_build(*t, open_threads);  // sort+build por índice en paralelo
#ifdef GINA_TRACE
        std::fprintf(stderr, "[RECOVER]   %-22s %7.1f ms  (fts=%zu ix=%zu filas=%zu)\n",
                     name.c_str(),
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - tt0).count(),
                     t->fts.size(), t->def.indexes.size(), t->rows.size());
#endif
    }
#ifdef GINA_TRACE
    rlap("fts_build+sec_build");
#endif
    return res.checkpoint_lsn;
}

// Aplica los registros del delta (orden lsn). Recovery: sin locks (un hilo).
// 0.7: los registros llegan como BYTES crudos — solo los VIVOS se decodifican
// (los muertos están supersededos por uno posterior que sí se aplica) y con
// el def vivo de la tabla. Los saltados con r.dead no cuestan decode.
void DbEngine::apply_delta() {
    for (const auto& r : dstore.records()) {
        // 0.7: los registros de FILA muertos se saltan sin perder corrección
        // (su supersesor posterior para la misma clave se aplica y gana).
        // Los ESTRUCTURALES (schema/drop) se aplican SIEMPRE: un schema
        // supersedido sigue siendo la CREACIÓN de la tabla que las filas
        // anteriores necesitan para poder aplicarse (bug hallado por la
        // validación cruzada con el lector Zig: 12 filas → 2).
        if (r.dead && (r.layout == delta::L_ROW || r.layout == delta::L_OVF ||
                       r.layout == delta::L_TOMB))
            continue;
        if (r.layout == delta::L_SCHEMA) {
            auto it = tables.find(r.def.name);
            if (it != tables.end()) {
                Table* t = it->second.get();
                by_id.erase(t->id);
                TableDef old_def = t->def;
                t->def = r.def;
                t->id = r.tid;
                t->rebuild_cmap();
                bool cols_same = old_def.columns.size() == r.def.columns.size();
                for (std::size_t i = 0; cols_same && i < old_def.columns.size(); i++)
                    cols_same = old_def.columns[i].name == r.def.columns[i].name;
                if (!cols_same)
                    t->rows.for_each_mut([&](const std::string&, SRow& row) {
                        srow_rewidth(old_def, t->def, t->cmap, row);
                        return btree::Visit::Next;
                    });
                t->fts.clear();
                for (const auto& c : t->def.columns)
                    if (c.fts) t->fts[c.name] = {};
                t->fts_built = false;  // perezoso: la próxima búsqueda reconstruye
                sec_build(*t, open_threads);
                by_id[t->id] = t;
            } else {
                Table* t = adopt_table(r.def, r.tid);
                sec_build(*t, open_threads);
            }
        } else if (r.layout == delta::L_DROP) {
            auto it = tables.find(r.name);
            if (it != tables.end()) {
                by_id.erase(it->second->id);
                tables.erase(it);
            }
        } else if (r.layout == delta::L_ROW || r.layout == delta::L_OVF) {
            auto bt = by_id.find(r.tid);
            if (bt == by_id.end()) continue;
            Table* t = bt->second;
            if (r.key.empty()) continue;
            std::size_t pos = 0;
            SRow row;
            try {
                row = codec::decode_srow(t->def, t->cmap, r.payload.data(),
                                         r.payload.size(), pos);
            } catch (const Error&) {
                continue;  // cadena contada como rota al abrir; nada que aplicar
            }
            apply_row(*t, r.key, std::move(row), nullptr, t->def.name);
        } else if (r.layout == delta::L_TOMB) {
            auto bt = by_id.find(r.tid);
            if (bt == by_id.end()) continue;
            apply_erase(*bt->second, r.key, nullptr, bt->second->def.name);
        }
    }
    lsn = std::max(lsn, dstore.watermark());
    for (const auto& [tid, t] : by_id)
        if (tid + 1 > next_table_id) next_table_id = tid + 1;
}

void DbEngine::replay_wal(std::uint64_t ckpt_lsn) {
    std::ifstream f(wal_path, std::ios::binary);
    if (!f) return;
    std::uint64_t active_tx = 0, max_tx = 0, good_bytes = 0;
    bool bad_tail = false;
    std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> pending;
    auto payload_of = [](const codec::FrameView& fr) {
        return std::vector<std::uint8_t>(fr.payload,
                                         fr.payload + fr.payload_len);
    };
    while (true) {
        std::uint8_t hdr[12];
        f.read(reinterpret_cast<char*>(hdr), 12);
        if (f.gcount() == 0) break;                    // clean EOF
        if (f.gcount() < 12) { bad_tail = true; break; }  // torn header
        std::uint32_t magic = 0, len = 0, crc = 0;
        for (int i = 0; i < 4; i++)
            magic |= static_cast<std::uint32_t>(hdr[i]) << (i * 8);
        for (int i = 0; i < 4; i++)
            len |= static_cast<std::uint32_t>(hdr[4 + i]) << (i * 8);
        for (int i = 0; i < 4; i++)
            crc |= static_cast<std::uint32_t>(hdr[8 + i]) << (i * 8);
        if (magic != codec::WAL_MAGIC || len > codec::MAX_FRAME_BODY) {
            bad_tail = true;
            break;
        }
        std::vector<std::uint8_t> body(len);
        f.read(reinterpret_cast<char*>(body.data()), len);
        if (static_cast<std::size_t>(f.gcount()) != len) {
            bad_tail = true;  // torn body
            break;
        }
        if (codec::crc32(body.data(), body.size()) != crc) {
            bad_tail = true;  // crc mismatch
            break;
        }
        try {
            const auto fr = codec::decode_frame_body(body.data(), body.size());
            const bool legacy = fr.version == codec::WAL_VERSION_V4;
            lsn = std::max(lsn, fr.lsn);
            max_tx = std::max(max_tx, fr.tx);
            if (fr.lsn <= ckpt_lsn) {  // already covered by the snapshot
                good_bytes += 12 + len;
                continue;
            }
            if (fr.kind == codec::K_COMMIT) {
                if (active_tx != 0 && fr.tx == active_tx) {
                    for (const auto& [k, p] : pending)
                        apply_op(k, p, nullptr, legacy);
                    pending.clear();
                    active_tx = 0;
                    ++commits;
                }
            } else if (fr.kind == codec::K_BASE) {
                pending.clear();
                active_tx = 0;
            } else if (fr.kind == codec::K_PUT || fr.kind == codec::K_DEL ||
                       fr.kind == codec::K_SCHEMA || fr.kind == codec::K_DROP ||
                       fr.kind == codec::K_RESCH) {
                if (fr.tx != active_tx) {
                    // a new tx started: the previous one was abandoned by a
                    // crash mid-transaction — its pending frames are discarded
                    pending.clear();
                    active_tx = fr.tx;
                }
                pending.emplace_back(fr.kind, payload_of(fr));
            }
        } catch (const Error&) {  // undecodable frame → treat as bad tail
            bad_tail = true;
            break;
        }
        good_bytes += 12 + len;
    }
    if (bad_tail) {  // repair so future appends start from a clean frame
        const auto size = static_cast<std::uint64_t>(std::filesystem::file_size(wal_path));
        bad_tail_bytes = size > good_bytes ? size - good_bytes : 0;
        f.close();
        std::filesystem::resize_file(wal_path, good_bytes);
    }
    tx_counter = max_tx;  // never reuse ids of leftover uncommitted frames
}

void DbEngine::recover() {
#ifdef GINA_TRACE
    const auto trv0 = std::chrono::steady_clock::now();
#endif
    base_lsn = load_snapshot();
    // resolver de defs del base: la capa delta deriva la PK de cada fila con
    // un pk-scan del frame (sin decodificarlo) usando primary_key de su tabla
    const auto tdo0 = std::chrono::steady_clock::now();
    dstore = delta::Store::open(
        delta_path, [this](std::uint32_t tid) -> const TableDef* {
            auto it = by_id.find(tid);
            return it == by_id.end() ? nullptr : &it->second->def;
        });
#ifdef GINA_TRACE
    {
        const auto t = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[RECOVER] %-22s %7.1f ms  (base_lsn=%llu)\n",
            "load_snapshot",
            std::chrono::duration<double, std::milli>(t - trv0).count(),
            (unsigned long long)base_lsn);
        std::fprintf(stderr, "[RECOVER] %-22s %7.1f ms  (%zu registros delta)\n",
            "delta::Store::open",
            std::chrono::duration<double, std::milli>(t - tdo0).count(),
            dstore.records().size());
    }
#else
    (void)tdo0;
#endif
    const bool delta_active =
        dstore.active() && dstore.watermark() > base_lsn;
    if (dstore.active() && !delta_active)
        dstore.drop_file();  // delta obsoleto (base más nuevo tras un FC)
#ifdef GINA_TRACE
    const auto tdel0 = std::chrono::steady_clock::now();
#endif
    if (delta_active) apply_delta();
#ifdef GINA_TRACE
    const auto tdel1 = std::chrono::steady_clock::now();
#endif
    const std::uint64_t from = delta_active ? dstore.watermark() : base_lsn;
    replay_wal(from);
#ifdef GINA_TRACE
    {
        const auto t = std::chrono::steady_clock::now();
        std::fprintf(stderr,
            "[RECOVER] delta_apply %7.1f ms  wal_replay %7.1f ms  (delta_active=%d)\n",
            std::chrono::duration<double, std::milli>(tdel1 - tdel0).count(),
            std::chrono::duration<double, std::milli>(t - tdel1).count(),
            delta_active ? 1 : 0);
    }
#endif
#ifdef GINA_TRACE
    {
        const auto t = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[RECOVER] %-22s %7.1f ms\n", "replay_wal+delta",
            std::chrono::duration<double, std::milli>(t - trv0).count());
    }
#endif
    // A torn-tail repair may leave the WAL with no frames; the in-memory lsn
    // must never regress below the watermark, or new frames would be
    // mistaken for "already in the snapshot" and silently skipped on recovery.
    lsn = std::max(lsn, from);
    commits_since_ckpt = 0;
}

// ---------------------------------------------------------------------------
// checkpoints
// ---------------------------------------------------------------------------

[[nodiscard]] bool DbEngine::should_compact() const {
    if (!dstore.active()) return false;  // sin delta todavía → IC primero
    const std::uint64_t thr = std::max<std::uint64_t>(
        snap_pages / std::max<std::uint32_t>(opt.compact_divisor, 1),
        opt.compact_min_pages);
    return dstore.live_pages() >= thr;
}

// Checkpoint incremental: escribe solo lo dirty desde el último checkpoint.
void DbEngine::incremental_locked() {
    const std::uint64_t W = lsn;
#ifdef GINA_TRACE
    const auto t0 = std::chrono::steady_clock::now();
    auto lap = [&](const char* what) {
        const auto t = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[IC] %-22s %7.1f ms\n", what,
            std::chrono::duration<double, std::milli>(t - t0).count());
    };
#endif
    std::vector<delta::Store::WriteRec> batch;

    for (const auto& [name, dd] : catalog_dirty) {
        delta::Store::WriteRec r;
        if (dd.first) {  // tabla borrada desde el último checkpoint
            r.layout = delta::L_DROP;
            r.tid = dd.second;
            r.name = name;
            codec::put_str(r.payload, name);
        } else {
            auto* t = find_table(name);
            if (!t) continue;  // defensivo
            r.layout = delta::L_SCHEMA;
            r.tid = t->id;
            r.schema = t->def;
        }
        batch.push_back(std::move(r));
    }
    {
        std::shared_lock cat(catalog_mu);
        for (const auto& [name, t] : tables) {  // name-sorted map
            std::shared_lock lock(t->mu);
            if (t->dirty.empty()) continue;
            batch.reserve(batch.size() + t->dirty.size());
            for (const auto& key : t->dirty) {
                delta::Store::WriteRec r;
                r.tid = t->id;
                r.key = key;
                const SRow* row = t->rows.find(key);
                if (row) {
                    r.layout = delta::L_ROW;
                    // 0.7: la fila se codifica AQUÍ (bajo lock, con el def
                    // vivo) — el lote es bytes puro, sin copias de filas
                    codec::encode_srow(r.payload, t->def, *row);
                } else {
                    r.layout = delta::L_TOMB;
                    codec::put_str(r.payload, key);
                }
                batch.push_back(std::move(r));
            }
        }
    }

    std::uint64_t freed = 0, copied = 0;
#ifdef GINA_TRACE
    lap("lote listo");
    std::fprintf(stderr, "[IC] lote=%zu registros\n", batch.size());
#endif
    dstore.checkpoint(W, batch, freed, copied);
#ifdef GINA_TRACE
    lap("store.checkpoint");
    std::fprintf(stderr, "[IC] freed=%llu copied=%llu\n",
                 (unsigned long long)freed, (unsigned long long)copied);
#endif

    {  // limpiar dirty (writer_mu held; unique por tabla)
        std::shared_lock cat(catalog_mu);
        for (const auto& [name, t] : tables) {
            std::unique_lock lock(t->mu);
            t->dirty.clear();
        }
    }
    catalog_dirty.clear();
    rebase_wal(W);
#ifdef GINA_TRACE
    lap("rebase WAL");
#endif
    ++checkpoints;
    commits_since_ckpt = 0;
}

// Checkpoint completo (compactación): reescribe data.pages y reinicia delta.
void DbEngine::full_checkpoint_locked() {
    const std::uint64_t W = lsn;

    std::vector<TableDef> schemas;
    {
        std::shared_lock cat(catalog_mu);
        schemas.reserve(tables.size());
        for (const auto& [name, t] : tables) schemas.push_back(t->def);
    }
    std::sort(schemas.begin(), schemas.end(),
              [](const TableDef& a, const TableDef& b) { return a.name < b.name; });
    std::map<std::string, std::uint32_t> ids;
    for (std::uint32_t i = 0; i < schemas.size(); i++)
        ids[schemas[i].name] = i + 1;

    const auto st = pager::save(
        data_path, W, schemas,
        [&](const pager::YieldSRow& yield) {
            std::shared_lock cat(catalog_mu);
            for (const auto& [name, t] : tables) {  // name-sorted map
                std::shared_lock lock(t->mu);
                t->rows.for_each(
                    [&](const std::string&, const SRow& row) {
                        yield(t->def, row);  // ya ordenadas por pk (B+Tree)
                        return btree::Visit::Next;
                    });
            }
        });

    {  // ids estables del base compacto
        std::unique_lock cat(catalog_mu);
        by_id.clear();
        for (auto& [name, t] : tables) {
            auto it = ids.find(name);
            t->id = it != ids.end() ? it->second : 0;
            if (t->id) by_id[t->id] = t.get();
            std::unique_lock lock(t->mu);
            t->dirty.clear();
        }
        next_table_id =
            tables.empty() ? 1 : static_cast<std::uint32_t>(tables.size()) + 1;
    }
    catalog_dirty.clear();
    dstore.reset(W);
    rebase_wal(W);
    ++checkpoints;
    ++compactions;
    commits_since_ckpt = 0;
    snap_pages = st.pages;
    snap_ovf = st.overflow_pages;
    snap_bytes = st.bytes;
    base_lsn = W;
}

void DbEngine::checkpoint_locked() {
    if (should_compact()) full_checkpoint_locked();
    else incremental_locked();
}

void DbEngine::checkpoint() {
    std::lock_guard<std::mutex> writer(writer_mu);
    checkpoint_locked();
}

} // namespace gina
