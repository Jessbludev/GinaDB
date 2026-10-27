// GinaDB Lite 0.7 — db.hpp
// New-style fluent API:
//
//   gina::Database db{"mydata"};
//   db["users"].create({.primary_key="id", .columns{{"id",gina::T::Text},
//                                                   {"age",gina::T::Int}}});
//   db["users"].emplace("id","u1", "name","Gina", "age",33);
//   db["users"].patch("u1", "age",34);            // partial update
//   auto u    = db["users"].get("u1");
//   auto hits = db["users"].where([](auto& r){ return r.at("age").as_int()>=18; })
//                          .order_by("age").limit(10).collect();
//   auto m    = db["users"].where(gina::eq("email","gina@db.io"))   // índice 0.6
//                          .collect();
//   auto r    = db["users"].where(gina::between("age",18,65))       // índice 0.6
//                          .order_by("id").limit(20).collect();
//   auto sims = db["media"].knn("vec", q, 5);           // multimodal KNN
//   auto docs = db["users"].search("bio", "base datos"); // full-text
//   auto sub  = db["users"].watch([](const gina::Event& ev){ ... });  // real-time
//   auto log  = db["users"].changes(cursor);             // changelog cursor
//
// Engine 0.6: B+Tree primario agrupado (hojas enlazadas → orden y rangos por
// PK) + índices secundarios ordenados + planner con pushdown + checkpoint
// incremental base+delta con free-list persistente derivada + WAL con rebase.
#pragma once
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "value.hpp"

namespace gina {

using T = Type;  // short alias for schema definitions

struct Column {
    std::string name;
    Type type = Type::Text;
    bool nullable = true;
    bool fts = false;              // maintain inverted index for full-text search
};

struct TableDef {
    std::string name;
    std::string primary_key = "id";
    std::vector<Column> columns;
    bool schemaless = false;       // hybrid mode: accept any fields
    std::vector<std::string> indexes;  // columnas con índice secundario (0.6)
};

enum class Metric : std::uint8_t { Cosine = 0, L2 = 1 };

struct Event {
    enum class Kind : std::uint8_t { Put = 1, Del = 2 };
    Kind kind = Kind::Put;
    std::string table, key;
    Row row;                       // full row for Put, empty for Del
};

struct Change {
    std::uint64_t seq = 0;         // monotonic per table, per process
    Event ev;
};

struct ChangeList {
    std::vector<Change> events;
    std::uint64_t next_cursor = 0;
    bool truncated = false;        // cursor older than retained history
};

using WatchId = std::int64_t;
using WatchFn = std::function<void(const Event&)>;

// ---------------------------------------------------------------------------
// Condición tipada (0.6): el planner puede empujarla a los índices (PK
// B+Tree o índice secundario de la columna). where(lambda) sigue disponible
// y se resuelve con recorrido completo.
struct Cond {
    enum class Op : std::uint8_t {
        Eq = 1, Ne = 2, Gt = 3, Ge = 4, Lt = 5, Le = 6,
        Between = 7, In = 8, And = 9, Or = 10,
    };
    Op op = Op::And;
    std::string col;               // hoja: columna; And/Or: vacío
    Value val, val2;               // Eq..Le: val; Between: [val, val2]
    std::vector<Value> vals;       // In
    std::vector<Cond> args;        // And / Or
};

[[nodiscard]] inline Cond eq(std::string col, Value v) {
    Cond c; c.op = Cond::Op::Eq; c.col = std::move(col); c.val = std::move(v); return c;
}
[[nodiscard]] inline Cond ne(std::string col, Value v) {
    Cond c; c.op = Cond::Op::Ne; c.col = std::move(col); c.val = std::move(v); return c;
}
[[nodiscard]] inline Cond gt(std::string col, Value v) {
    Cond c; c.op = Cond::Op::Gt; c.col = std::move(col); c.val = std::move(v); return c;
}
[[nodiscard]] inline Cond ge(std::string col, Value v) {
    Cond c; c.op = Cond::Op::Ge; c.col = std::move(col); c.val = std::move(v); return c;
}
[[nodiscard]] inline Cond lt(std::string col, Value v) {
    Cond c; c.op = Cond::Op::Lt; c.col = std::move(col); c.val = std::move(v); return c;
}
[[nodiscard]] inline Cond le(std::string col, Value v) {
    Cond c; c.op = Cond::Op::Le; c.col = std::move(col); c.val = std::move(v); return c;
}
[[nodiscard]] inline Cond between(std::string col, Value lo, Value hi) {
    Cond c; c.op = Cond::Op::Between; c.col = std::move(col);
    c.val = std::move(lo); c.val2 = std::move(hi); return c;
}
[[nodiscard]] inline Cond in_(std::string col, std::vector<Value> vs) {
    Cond c; c.op = Cond::Op::In; c.col = std::move(col); c.vals = std::move(vs); return c;
}
[[nodiscard]] inline Cond all_of(std::vector<Cond> cs) {
    Cond c; c.op = Cond::Op::And; c.args = std::move(cs); return c;
}
[[nodiscard]] inline Cond any_of(std::vector<Cond> cs) {
    Cond c; c.op = Cond::Op::Or; c.args = std::move(cs); return c;
}

struct Stats {
    std::uint64_t tables = 0, rows = 0, last_lsn = 0, commits = 0,
                  checkpoints = 0, wal_bytes = 0, data_bytes = 0,
                  pages = 0, overflow_pages = 0, recovered_bad_tail = 0;
    // 0.6 — checkpoint incremental (delta.pages)
    std::uint64_t delta_bytes = 0, delta_pages = 0, delta_live_pages = 0,
                  delta_free_pages = 0, compactions = 0;
    std::uint64_t delta_suspect_pages = 0;  // ilegibles (torn/corruptas) vistas al abrir
    // 0.7 — clasificación de páginas ilegibles del delta: suspect son torn
    // normales de IC sin flip; corrupt son roturas DENTRO del extent
    // comprometido (pérdida real de registros, detectada y reportada);
    // broken_ovf_records cuenta cadenas overflow rotas (registro perdido).
    std::uint64_t delta_corrupt_pages = 0, delta_broken_ovf_records = 0;
};

struct Options {
    bool durable = false;                    // fsync WAL on every commit
    std::uint32_t checkpoint_every = 1000;   // commits between checkpoints (0 = manual only)
    std::size_t changelog_capacity = 4096;   // retained events per table (0 = disable)
    std::size_t max_blob_size = 16u << 20;   // per-Bytes-value limit (16 MiB)
    bool checkpoint_on_close = true;         // false = simulate crash on exit (tests)
    // 0.6 — compactación (checkpoint completo) cuando las páginas vivas del
    // delta superan max(base_pages / compact_divisor, compact_min_pages)
    std::uint32_t compact_divisor = 4;
    std::uint32_t compact_min_pages = 64;
};

namespace detail {
[[nodiscard]] inline std::string to_key(const std::string& k) { return k; }
[[nodiscard]] inline std::string to_key(const char* k) { return k; }

inline void fill_row(Row&) {}
template <class K, class V, class... Rest>
void fill_row(Row& r, K&& k, V&& v, Rest&&... rest) {
    r.emplace(to_key(std::forward<K>(k)), Value(std::forward<V>(v)));
    fill_row(r, std::forward<Rest>(rest)...);
}

template <class... A>
[[nodiscard]] Row row_from_pairs(A&&... kv) {
    static_assert(sizeof...(A) % 2 == 0,
                  "emplace/patch expects alternating key,value pairs");
    Row r;
    fill_row(r, std::forward<A>(kv)...);
    return r;
}
} // namespace detail

class Transaction;
class TableRef;
struct DbEngine;   // internal engine, defined in src/impl.hpp

// ---------------------------------------------------------------------------
class Query {
public:
    Query& limit(std::size_t n) & { limit_ = n; return *this; }
    Query&& limit(std::size_t n) && { limit_ = n; return std::move(*this); }
    Query& order_by(std::string column, bool desc = false) & {
        order_.emplace_back(std::move(column), desc); return *this;
    }
    Query&& order_by(std::string column, bool desc = false) && {
        order_.emplace_back(std::move(column), desc); return std::move(*this);
    }
    [[nodiscard]] std::vector<Row> collect() const;
    [[nodiscard]] std::size_t count() const;
private:
    friend class TableRef;
    Query(const class Database* db, std::string table,
          std::function<bool(const Row&)> pred)
        : db_(db), table_(std::move(table)), pred_(std::move(pred)) {}
    Query(const class Database* db, std::string table, Cond cond)
        : db_(db), table_(std::move(table)), cond_(std::move(cond)) {}
    const Database* db_;
    std::string table_;
    std::function<bool(const Row&)> pred_;   // filtro libre (recorrido total)
    std::optional<Cond> cond_;               // condición tipada (planneable)
    std::size_t limit_ = static_cast<std::size_t>(-1);
    std::vector<std::pair<std::string, bool>> order_;
};

// ---------------------------------------------------------------------------
// Lightweight table handle: db["users"] — all operations validated by name.
class TableRef {
public:
    [[nodiscard]] bool exists() const;
    void create(TableDef def) const;
    void drop() const;
    [[nodiscard]] TableDef schema() const;

    // Índices secundarios (0.6) ------------------------------------------------
    // create_index/drop_index son atómicos: se registran en el WAL (op de
    // re-schema) y sobreviven a reinicios; el índice se construye desde las
    // filas existentes en el momento de la llamada.
    void create_index(const std::string& column) const;
    void drop_index(const std::string& column) const;

    // CRUD ------------------------------------------------------------------
    void insert(Row r) const;                            // full row
    template <class... A> void emplace(A&&... kv) const { // "k",v,"k",v,...
        insert(detail::row_from_pairs(std::forward<A>(kv)...));
    }
    [[nodiscard]] std::optional<Row> get(const std::string& key) const;
    bool update(const std::string& key, Row patch) const;  // partial merge
    template <class... A> bool patch(const std::string& key, A&&... kv) const {
        return update(key, detail::row_from_pairs(std::forward<A>(kv)...));
    }
    bool erase(const std::string& key) const;            // true if it existed
    [[nodiscard]] std::vector<Row> all() const;
    [[nodiscard]] std::size_t count() const;

    // Queries ----------------------------------------------------------------
    [[nodiscard]] Query where(std::function<bool(const Row&)> pred) const;
    [[nodiscard]] Query where(Cond cond) const;   // tipada → usa índices
    [[nodiscard]] std::optional<Row> find(
        std::function<bool(const Row&)> pred) const;

    // Multimodal ---------------------------------------------------------------
    // k nearest neighbours by vector column. Returns (key, score) sorted
    // best-first. Cosine: score in [-1,1], higher = closer. L2: euclidean
    // distance, lower = closer. Rows with a different dimension are skipped.
    [[nodiscard]] std::vector<std::pair<std::string, double>> knn(
        const std::string& column, const Vec& query,
        std::size_t k, Metric metric = Metric::Cosine) const;
    // Full-text AND search over a Text column (inverted index when the column
    // was declared with .fts=true, brute-force scan otherwise).
    [[nodiscard]] std::vector<Row> search(const std::string& column,
                                          const std::string& query,
                                          std::size_t limit = 50) const;

    // Real-time ---------------------------------------------------------------
    [[nodiscard]] WatchId watch(WatchFn fn) const;
    void unwatch(WatchId id) const;
    [[nodiscard]] ChangeList changes(std::uint64_t since_cursor) const;

private:
    friend class Database;
    friend class Transaction;
    TableRef(const class Database* db, std::string name)
        : db_(db), name_(std::move(name)) {}
    [[nodiscard]] const Database* db() const { return db_; }
    [[nodiscard]] const std::string& name() const { return name_; }

    const Database* db_;
    std::string name_;
};

// ---------------------------------------------------------------------------
// Transaction table handle: tx["users"] — staged, atomic on commit.
class TxTable {
public:
    void insert(Row r) const;
    template <class... A> void emplace(A&&... kv) const {
        insert(detail::row_from_pairs(std::forward<A>(kv)...));
    }
    void erase(const std::string& key) const;
private:
    friend class Transaction;
    TxTable(Transaction* tx, std::string name)
        : tx_(tx), name_(std::move(name)) {}
    Transaction* tx_;
    std::string name_;
};

// ---------------------------------------------------------------------------
class Transaction {
public:
    explicit Transaction(Database& db);
    void commit();
    void rollback() noexcept;
    ~Transaction();
    Transaction(Transaction&& o) noexcept;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction& operator=(Transaction&&) = delete;
    [[nodiscard]] TxTable operator[](std::string table) {
        return TxTable(this, std::move(table));
    }
private:
    friend class TxTable;
    struct Staged { std::uint8_t kind; std::string table; Row row; std::string key; };
    Database* db_;
    std::vector<Staged> ops_;
    bool done_ = false;
};

// ---------------------------------------------------------------------------
class Database {
public:
    explicit Database(std::filesystem::path dir, Options options = {});
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    [[nodiscard]] TableRef operator[](std::string table) const {
        return TableRef(this, std::move(table));
    }
    [[nodiscard]] Transaction begin();
    void checkpoint();          // incremental (o compactación si toca)
    void compact();             // checkpoint completo: reescribe data.pages
                                // y reinicia delta.pages (VACUUM)
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] std::vector<std::string> tables() const;

private:
    friend class TableRef;
    friend class Transaction;
    friend class Query;
    std::unique_ptr<DbEngine> p_;    // internal engine (src/impl.hpp)
};

} // namespace gina
