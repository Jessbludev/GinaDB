// GinaDB Lite 0.7 — internal engine definition (NOT part of the public API).
// Locking discipline (acquire in this order, never inverted):
//   writer_mu -> catalog_mu -> table->mu   (+ log_mu / watch_mu standalone)
// Writers are serialized by writer_mu (SQLite-style single writer);
// readers run concurrently on shared locks.
#pragma once
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "gina/db.hpp"
#include "btree.hpp"
#include "codec.hpp"
#include "delta.hpp"
#include "fts.hpp"
#include "idxkey.hpp"
#include "pager.hpp"
#include "srow.hpp"

namespace gina {

[[nodiscard]] std::string keyof(const TableDef& def, const Row& r,
                                bool legacy_fallback = false);
// Validates (and possibly normalizes: int->float widening) a row against the
// schema. partial=true → update patch (PK change rejected, may omit columns).
[[nodiscard]] Row validate_row(const TableDef& def, Row row, std::size_t max_blob,
                               bool partial);

// Secondary index tree: idxkey codificado → PKs con ese valor.
using SecTree = btree::Map<std::vector<std::string>>;

struct DbEngine {
    struct Table {
        TableDef def;
        ColMap cmap;                          // nombre → ordinal (0.7)
        std::uint32_t id = 0;                   // id estable (base catalog / delta)
        btree::Map<SRow> rows;                   // índice primario agrupado (PK)
        std::unordered_map<std::string, SecTree> sec;  // índices secundarios
        std::unordered_map<std::string, fts::InvertedIndex> fts;  // por columna
        // 0.7: FTS perezoso — no se construye al abrir; lo hace la primera
        // búsqueda que lo necesite (bajo lock exclusivo, una sola vez). El
        // mantenimiento incremental solo corre cuando ya está construido.
        mutable bool fts_built = false;
        std::unordered_set<std::string> dirty;  // claves tocadas desde el último IC
        mutable std::shared_mutex mu;
        std::deque<Change> log;                                   // ring
        std::uint64_t last_seq = 0;
        mutable std::mutex log_mu;

        // reconstruye cmap desde def (llamar tras cualquier cambio de def)
        void rebuild_cmap() { cmap = cmap_build(def); }
    };

    std::filesystem::path dir, data_path, wal_path, delta_path;
    Options opt;
    int wal_fd = -1;

    std::uint64_t lsn = 0, commits = 0, checkpoints = 0, compactions = 0,
                  tx_counter = 0, commits_since_ckpt = 0, bad_tail_bytes = 0;
    std::uint64_t snap_pages = 0, snap_ovf = 0, snap_bytes = 0, base_lsn = 0;

    mutable std::shared_mutex catalog_mu;
    std::map<std::string, std::unique_ptr<Table>> tables;  // name-sorted
    std::unordered_map<std::uint32_t, Table*> by_id;       // tid → tabla viva
    std::uint32_t next_table_id = 1;

    // dirty de catálogo: nombre → (¿borrada?, tid en el momento del cambio)
    std::map<std::string, std::pair<bool, std::uint32_t>> catalog_dirty;

    delta::Store dstore;  // checkpoint incremental (delta.pages)

    std::mutex writer_mu;

    mutable std::shared_mutex watch_mu;
    std::map<WatchId, std::pair<std::string, WatchFn>> watchers;
    WatchId next_watch_id = 1;
    std::mutex dispatch_mu;  // orders watcher dispatch across committers

    explicit DbEngine(std::filesystem::path d, Options o);
    ~DbEngine();
    DbEngine(const DbEngine&) = delete;
    DbEngine& operator=(const DbEngine&) = delete;

    // hilos usados para decodificar el base al abrir (0/1 = serial)
    std::uint32_t open_threads = 2;

    // ---- table helpers (catalog_mu must be held, or writer_mu implies it) --
    [[nodiscard]] Table* find_table(const std::string& name) const {
        std::shared_lock cat(catalog_mu);
        auto it = tables.find(name);
        return it == tables.end() ? nullptr : it->second.get();
    }
    [[nodiscard]] Table* must_table(const std::string& name) const;

    // registra una tabla nueva (def ya con nombre; construye cmap+fts)
    Table* adopt_table(const TableDef& def, std::uint32_t id);

    // ---- secondary index maintenance (t->mu exclusive held) -----------------
    // nthreads: sort+build por índice en paralelo (0.7); 1 = serial
    static void sec_build(Table& t, unsigned nthreads = 1);
    static void sec_add(Table& t, const std::string& key, const SRow& r);
    static void sec_remove(Table& t, const std::string& key, const SRow& r);
    static void fts_build(Table& t);                       // índice FTS completo

    // ---- WAL ----------------------------------------------------------------
    void append_frame(std::uint8_t kind, std::uint64_t tx,
                      const std::vector<std::uint8_t>& payload);
    void sync_wal();
    void rebase_wal(std::uint64_t snapshot_lsn);

    // ---- apply (memory + fts + sec + events); caller holds writer_mu or
    //      recovery) ------------------------------------------------------------
    void apply_op(std::uint8_t kind, const std::vector<std::uint8_t>& payload,
                  std::vector<Event>* events, bool legacy);
    void apply_row(Table& t, const std::string& key, SRow row,
                   std::vector<Event>* events, const std::string& tname);
    bool apply_erase(Table& t, const std::string& key,
                     std::vector<Event>* events, const std::string& tname);
    void index_row(Table& t, const std::string& key, const SRow* old_row,
                   const SRow* new_row);  // FTS (sec se mantiene en apply_*)
    void log_changes(Table& t, const std::vector<Event>& events);

    // ---- commit pipeline ------------------------------------------------------
    // ops: (kind, payload) already validated. writer_mu MUST be held by the
    // caller. Returns the events to dispatch AFTER releasing writer_mu.
    [[nodiscard]] std::vector<Event> commit_ops_locked(
        std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> ops);
    void dispatch(const std::vector<Event>& events);

    // ---- recovery ---------------------------------------------------------------
    void recover();
    [[nodiscard]] std::uint64_t load_snapshot();   // returns checkpoint_lsn
    void decode_base_records(                     // 0.7: crudo → filas planas
        const pager::LoadResult& res,
        std::unordered_map<std::uint32_t,
                           std::vector<std::pair<std::string, SRow>>>& out);
    void apply_delta();                            // base + delta + WAL
    void replay_wal(std::uint64_t ckpt_lsn);

    // ---- checkpoint ---------------------------------------------------------------
    void checkpoint_locked();                      // IC o FC según umbral
    void incremental_locked();                      // checkpoint incremental
    void full_checkpoint_locked();                  // compactación (FC)
    [[nodiscard]] bool should_compact() const;
    void checkpoint();                              // takes writer_mu
};

} // namespace gina
