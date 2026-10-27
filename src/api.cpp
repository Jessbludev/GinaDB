// GinaDB Lite 0.7 — api.cpp: public fluent API (TableRef, Transaction, Query)
// + planner de consultas con pushdown a índices.
#include "impl.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace gina {

namespace {

// ---------------------------------------------------------------------------
// evaluación de condiciones tipadas (semántica: columna ausente ≈ null;
// eq(null)=false, ne=!eq; orden cruzado de tipos por tag, como Value::operator<)
// 0.7: evalúa contra la FILA PLANA por ordinal — sin materializar el mapa.
// ---------------------------------------------------------------------------

[[nodiscard]] bool eval_cond(const Cond& c, const ColMap& cm, const SRow& r) {
    switch (c.op) {
        case Cond::Op::Eq: {
            const auto* v = srow_get(cm, r, c.col);
            return v && *v == c.val;
        }
        case Cond::Op::Ne: {
            const auto* v = srow_get(cm, r, c.col);
            return !(v && *v == c.val);
        }
        case Cond::Op::Gt: {
            const auto* v = srow_get(cm, r, c.col);
            return v && c.val < *v;
        }
        case Cond::Op::Ge: {
            const auto* v = srow_get(cm, r, c.col);
            return v && !(*v < c.val);
        }
        case Cond::Op::Lt: {
            const auto* v = srow_get(cm, r, c.col);
            return v && *v < c.val;
        }
        case Cond::Op::Le: {
            const auto* v = srow_get(cm, r, c.col);
            return v && !(c.val < *v);
        }
        case Cond::Op::Between: {  // val <= v <= val2 (ambos inclusivos)
            const auto* v = srow_get(cm, r, c.col);
            return v && !(*v < c.val) && !(c.val2 < *v);
        }
        case Cond::Op::In: {
            const auto* v = srow_get(cm, r, c.col);
            if (!v) return false;
            for (const auto& x : c.vals)
                if (*v == x) return true;
            return false;
        }
        case Cond::Op::And:
            for (const auto& a : c.args)
                if (!eval_cond(a, cm, r)) return false;
            return true;
        case Cond::Op::Or:
            for (const auto& a : c.args)
                if (eval_cond(a, cm, r)) return true;
            return false;
    }
    return false;
}

void flatten_and(const Cond& c, std::vector<const Cond*>& out) {
    if (c.op == Cond::Op::And) {
        for (const auto& a : c.args) flatten_and(a, out);
    } else {
        out.push_back(&c);
    }
}
// ---------------------------------------------------------------------------
// planner: elige la vía de acceso
//   PkPoint > PkRange > SecPoint > SecRange > Scan
// ---------------------------------------------------------------------------
struct Plan {
    enum Kind { Scan, PkPoint, PkRange, SecPoint, SecRange };
    Kind kind = Scan;
    std::string lo, hi;         // PkRange/PkPoint: claves PK (hi exclusivo)
    std::string sec_col;        // columna del índice secundario
    std::string sec_lo, sec_hi; // claves codificadas idx (hi exclusivo)
    std::string sec_eq;
};

[[nodiscard]] Plan make_plan(const DbEngine::Table* t, const Cond* cond) {
    Plan p;
    if (!cond) return p;
    std::vector<const Cond*> leaves;
    flatten_and(*cond, leaves);
    const auto& pk = t->def.primary_key;

    const Cond* pk_eq = nullptr;
    const Cond* pk_lo = nullptr;
    const Cond* pk_hi = nullptr;
    const Cond* sec_eq = nullptr;
    const Cond* sec_rng = nullptr;
    for (const auto* l : leaves) {
        if (l->op == Cond::Op::And || l->op == Cond::Op::Or) continue;
        if (l->col == pk) {
            if (l->op == Cond::Op::Eq && l->val.is_text() &&
                !l->val.as_text().empty() && !pk_eq)
                pk_eq = l;
            else if (l->op == Cond::Op::Between) {
                if (!pk_lo) pk_lo = l;
                if (!pk_hi) pk_hi = l;
            } else if (l->op == Cond::Op::Ge || l->op == Cond::Op::Gt) {
                if (!pk_lo) pk_lo = l;
            } else if (l->op == Cond::Op::Le || l->op == Cond::Op::Lt) {
                if (!pk_hi) pk_hi = l;
            }
        } else if (t->sec.count(l->col)) {
            // utilizable si el valor se codifica (null/array/object/vector no)
            const bool usable = l->op == Cond::Op::In
                                    ? false
                                    : !idx::encode(l->val).empty();
            if (!usable) continue;
            if (l->op == Cond::Op::Eq) {
                if (!sec_eq) sec_eq = l;
            } else if (!sec_rng) {
                sec_rng = l;
            }
        }
    }
    if (pk_eq) {
        p.kind = Plan::PkPoint;
        p.lo = pk_eq->val.as_text();
        p.hi = p.lo + std::string(1, '\0');  // sucesor: solo esa clave
    } else if (pk_lo || pk_hi) {
        p.kind = Plan::PkRange;
        if (pk_lo) {  // Ge/Between-lo: inclusivo; Gt: sucesor
            p.lo = pk_lo->val.as_text();
            if (pk_lo->op == Cond::Op::Gt)
                p.lo = p.lo + std::string(1, '\0');
        }
        if (pk_hi) {
            if (pk_hi->op == Cond::Op::Between) {
                p.hi = pk_hi->val2.as_text() + std::string(1, '\0');
            } else if (pk_hi->op == Cond::Op::Le) {
                p.hi = pk_hi->val.as_text() + std::string(1, '\0');
            } else {  // Lt: exclusivo
                p.hi = pk_hi->val.as_text();
            }
        }
    } else if (sec_eq) {
        const auto enc = idx::encode(sec_eq->val);
        if (!enc.empty()) {
            p.kind = Plan::SecPoint;
            p.sec_col = sec_eq->col;
            p.sec_eq = enc;
        }
    } else if (sec_rng) {
        const auto& v = sec_rng->val;
        const auto& v2 = sec_rng->val2;
        std::string lo, hi;
        bool ok = true;
        switch (sec_rng->op) {
            case Cond::Op::Ge: lo = idx::encode(v); break;
            case Cond::Op::Gt: lo = idx::lower_exclusive(idx::encode(v)); break;
            case Cond::Op::Le: hi = idx::upper_inclusive(idx::encode(v)); break;
            case Cond::Op::Lt: hi = idx::encode(v); break;
            case Cond::Op::Between: {
                const auto a = idx::encode(v), b = idx::encode(v2);
                if (a.empty() || b.empty()) ok = false;
                else { lo = a; hi = idx::upper_inclusive(b); }
                break;
            }
            default: ok = false;
        }
        if (ok && !(lo.empty() && hi.empty())) {
            p.kind = Plan::SecRange;
            p.sec_col = sec_rng->col;
            p.sec_lo = lo;
            p.sec_hi = hi;
        }
    }
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------

Database::Database(std::filesystem::path dir, Options options)
    : p_(new DbEngine(std::move(dir), options)) {}

Database::~Database() = default;

Transaction Database::begin() { return Transaction(*this); }

void Database::checkpoint() { p_->checkpoint(); }

void Database::compact() {
    std::lock_guard<std::mutex> w(p_->writer_mu);
    p_->full_checkpoint_locked();
}

Stats Database::stats() const {
    Stats s;
    s.last_lsn = p_->lsn;
    s.commits = p_->commits;
    s.checkpoints = p_->checkpoints;
    s.compactions = p_->compactions;
    s.pages = p_->snap_pages;
    s.overflow_pages = p_->snap_ovf;
    s.data_bytes = p_->snap_bytes;
    s.recovered_bad_tail = p_->bad_tail_bytes;
    s.delta_bytes = p_->dstore.file_bytes();
    s.delta_pages = p_->dstore.page_count();
    s.delta_live_pages = p_->dstore.live_pages();
    s.delta_free_pages = p_->dstore.free_pages();
    s.delta_suspect_pages = p_->dstore.suspect_pages();
    s.delta_corrupt_pages = p_->dstore.corrupt_pages();          // 0.7
    s.delta_broken_ovf_records = p_->dstore.broken_ovf_records(); // 0.7
    std::error_code ec;
    if (std::filesystem::exists(p_->wal_path, ec))
        s.wal_bytes = static_cast<std::uint64_t>(
            std::filesystem::file_size(p_->wal_path, ec));
    std::shared_lock cat(p_->catalog_mu);
    s.tables = p_->tables.size();
    for (const auto& [name, t] : p_->tables) {
        std::shared_lock l(t->mu);
        s.rows += t->rows.size();
    }
    return s;
}

std::vector<std::string> Database::tables() const {
    std::shared_lock cat(p_->catalog_mu);
    std::vector<std::string> out;
    out.reserve(p_->tables.size());
    for (const auto& [name, t] : p_->tables) out.push_back(name);
    return out;
}

// ---------------------------------------------------------------------------
// TableRef — DDL
// ---------------------------------------------------------------------------

bool TableRef::exists() const { return db_->p_->find_table(name_) != nullptr; }

void TableRef::create(TableDef def) const {
    auto* p = db_->p_.get();
    if (name_.empty())
        throw Error(Error::Code::BadArgument, "table name must not be empty");
    if (def.primary_key.empty())
        throw Error(Error::Code::BadArgument, "primary key must not be empty");
    for (std::size_t i = 0; i < def.columns.size(); i++)
        for (std::size_t j = i + 1; j < def.columns.size(); j++)
            if (def.columns[i].name == def.columns[j].name)
                throw Error(Error::Code::BadArgument,
                            "duplicate column '" + def.columns[i].name + "'");
    // índices declarados en el create: validar columnas
    for (const auto& ix : def.indexes) {
        const Column* col = nullptr;
        for (const auto& c : def.columns)
            if (c.name == ix) { col = &c; break; }
        if (!col && !def.schemaless)
            throw Error(Error::Code::BadArgument,
                        "index on unknown column '" + ix + "'");
        if (col && (col->type == Type::Array || col->type == Type::Object ||
                    col->type == Type::Vector))
            throw Error(Error::Code::BadArgument,
                        "column '" + ix + "' is not indexable");
    }
    std::lock_guard<std::mutex> w(p->writer_mu);
    {
        std::shared_lock cat(p->catalog_mu);
        if (p->tables.count(def.name))
            throw Error(Error::Code::TableExists,
                        "table '" + def.name + "' already exists");
    }
    def.name = name_;
    std::vector<std::uint8_t> payload;
    codec::encode_schema(payload, def);
    const auto events = p->commit_ops_locked({{codec::K_SCHEMA, std::move(payload)}});
    p->dispatch(events);
}

void TableRef::drop() const {
    auto* p = db_->p_.get();
    std::lock_guard<std::mutex> w(p->writer_mu);
    (void)p->must_table(name_);  // throws NoSuchTable
    std::vector<std::uint8_t> payload;
    codec::put_str(payload, name_);
    const auto events = p->commit_ops_locked({{codec::K_DROP, std::move(payload)}});
    p->dispatch(events);
}

TableDef TableRef::schema() const {
    auto* t = db_->p_->must_table(name_);
    std::shared_lock l(t->mu);
    return t->def;
}

// ---------------------------------------------------------------------------
// TableRef — índices secundarios
// ---------------------------------------------------------------------------

void TableRef::create_index(const std::string& column) const {
    auto* p = db_->p_.get();
    std::lock_guard<std::mutex> w(p->writer_mu);
    auto* t = p->must_table(name_);
    TableDef nd;
    {
        std::shared_lock l(t->mu);
        const Column* col = nullptr;
        for (const auto& c : t->def.columns)
            if (c.name == column) { col = &c; break; }
        if (!col && !t->def.schemaless)
            throw Error(Error::Code::BadArgument,
                        "unknown column '" + column + "'");
        if (col && (col->type == Type::Array || col->type == Type::Object ||
                    col->type == Type::Vector))
            throw Error(Error::Code::BadArgument,
                        "column '" + column + "' is not indexable");
        for (const auto& ix : t->def.indexes)
            if (ix == column)
                throw Error(Error::Code::BadArgument,
                            "index on '" + column + "' already exists");
        nd = t->def;
    }
    nd.indexes.push_back(column);
    std::vector<std::uint8_t> payload;
    codec::encode_schema(payload, nd);
    const auto events =
        p->commit_ops_locked({{codec::K_RESCH, std::move(payload)}});
    p->dispatch(events);
}

void TableRef::drop_index(const std::string& column) const {
    auto* p = db_->p_.get();
    std::lock_guard<std::mutex> w(p->writer_mu);
    auto* t = p->must_table(name_);
    TableDef nd;
    {
        std::shared_lock l(t->mu);
        bool found = false;
        for (const auto& ix : t->def.indexes)
            if (ix == column) found = true;
        if (!found)
            throw Error(Error::Code::BadArgument,
                        "no index on '" + column + "'");
        nd = t->def;
    }
    nd.indexes.erase(std::remove(nd.indexes.begin(), nd.indexes.end(), column),
                     nd.indexes.end());
    std::vector<std::uint8_t> payload;
    codec::encode_schema(payload, nd);
    const auto events =
        p->commit_ops_locked({{codec::K_RESCH, std::move(payload)}});
    p->dispatch(events);
}

// ---------------------------------------------------------------------------
// TableRef — CRUD
// ---------------------------------------------------------------------------

void TableRef::insert(Row r) const {
    auto* p = db_->p_.get();
    std::lock_guard<std::mutex> w(p->writer_mu);
    auto* t = p->must_table(name_);  // never a silent loss (0.4 bug fixed)
    r = validate_row(t->def, std::move(r), p->opt.max_blob_size, false);
    std::vector<std::uint8_t> payload;
    codec::put_str(payload, name_);
    codec::encode_row(payload, r);
    const auto events = p->commit_ops_locked({{codec::K_PUT, std::move(payload)}});
    p->dispatch(events);
}

std::optional<Row> TableRef::get(const std::string& key) const {
    auto* p = db_->p_.get();
    auto* t = p->find_table(name_);
    if (!t) return std::nullopt;
    std::shared_lock l(t->mu);
    const SRow* r = t->rows.find(key);
    if (!r) return std::nullopt;
    return row_from_srow(t->def, *r);  // materializa solo en el borde
}

bool TableRef::update(const std::string& key, Row patch) const {
    auto* p = db_->p_.get();
    std::lock_guard<std::mutex> w(p->writer_mu);
    auto* t = p->must_table(name_);
    patch = validate_row(t->def, std::move(patch), p->opt.max_blob_size, true);
    SRow merged;
    {
        std::shared_lock l(t->mu);
        const SRow* it = t->rows.find(key);
        if (!it) return false;
        merged = *it;  // copia plana (mucho más barata que el mapa de 0.6)
    }
    srow_merge_patch(t->def, t->cmap, merged, std::move(patch));
    std::vector<std::uint8_t> payload;
    codec::put_str(payload, name_);
    codec::encode_srow(payload, t->def, merged);
    const auto events = p->commit_ops_locked({{codec::K_PUT, std::move(payload)}});
    p->dispatch(events);
    return true;
}

bool TableRef::erase(const std::string& key) const {
    auto* p = db_->p_.get();
    std::lock_guard<std::mutex> w(p->writer_mu);
    auto* t = p->must_table(name_);
    {
        std::shared_lock l(t->mu);
        if (!t->rows.find(key)) return false;
    }
    std::vector<std::uint8_t> payload;
    codec::put_str(payload, name_);
    codec::put_str(payload, key);
    const auto events = p->commit_ops_locked({{codec::K_DEL, std::move(payload)}});
    p->dispatch(events);
    return true;
}

std::vector<Row> TableRef::all() const {
    auto* p = db_->p_.get();
    auto* t = p->find_table(name_);
    if (!t)
        throw Error(Error::Code::NoSuchTable,
                    "table '" + name_ + "' does not exist");
    std::shared_lock l(t->mu);
    std::vector<Row> out;
    out.reserve(t->rows.size());
    t->rows.for_each([&](const std::string&, const SRow& r) {
        out.push_back(row_from_srow(t->def, r));
        return btree::Visit::Next;
    });
    return out;  // ordenadas por PK (B+Tree)
}

std::size_t TableRef::count() const {
    auto* p = db_->p_.get();
    auto* t = p->find_table(name_);
    if (!t)
        throw Error(Error::Code::NoSuchTable,
                    "table '" + name_ + "' does not exist");
    std::shared_lock l(t->mu);
    return t->rows.size();
}

// ---------------------------------------------------------------------------
// TableRef — queries
// ---------------------------------------------------------------------------

Query TableRef::where(std::function<bool(const Row&)> pred) const {
    return Query(db_, name_, std::move(pred));
}

Query TableRef::where(Cond cond) const {
    return Query(db_, name_, std::move(cond));
}

std::optional<Row> TableRef::find(
    std::function<bool(const Row&)> pred) const {
    auto* p = db_->p_.get();
    auto* t = p->find_table(name_);
    if (!t) return std::nullopt;
    std::shared_lock l(t->mu);
    std::optional<Row> out;
    t->rows.for_each([&](const std::string&, const SRow& r) {
        Row m = row_from_srow(t->def, r);
        if (pred(m)) {
            out = std::move(m);
            return btree::Visit::Stop;
        }
        return btree::Visit::Next;
    });
    return out;
}

std::vector<Row> Query::collect() const {
    auto* p = db_->p_.get();
    auto* t = p->find_table(table_);
    if (!t)
        throw Error(Error::Code::NoSuchTable,
                    "table '" + table_ + "' does not exist");
    std::shared_lock l(t->mu);
    const Plan plan = make_plan(t, cond_ ? &*cond_ : nullptr);
    const auto& pk = t->def.primary_key;
    // 0.7: la condición tipada se evalúa contra la fila PLANA (sin
    // materializar). El lambda libre (pred_) exige el mapa → se materializa
    // SOLO esa fila (y solo cuando la condición ya pasó, si ambas hay).
    auto pass_s = [&](const SRow& r) {
        if (pred_) {
            Row m = row_from_srow(t->def, r);
            if (!pred_(m)) return false;
        }
        if (cond_ && !eval_cond(*cond_, t->cmap, r)) return false;
        return true;
    };
    auto materialize = [&](const SRow& r) { return row_from_srow(t->def, r); };

    // vía rápida: recorrido PK-ordenado (sin order o order por PK) con tope
    const bool pk_order =
        order_.empty() || (order_.size() == 1 && order_[0].first == pk);
    if (pk_order &&
        (plan.kind == Plan::Scan || plan.kind == Plan::PkRange ||
         plan.kind == Plan::PkPoint)) {
        std::vector<Row> out;
        const bool desc = !order_.empty() && order_[0].second;
        auto walk = [&](const std::string&, const SRow& row) {
            if (out.size() >= limit_) return btree::Visit::Stop;
            if (pass_s(row)) out.push_back(materialize(row));
            return btree::Visit::Next;
        };
        if (desc) t->rows.visit_range_reverse(plan.lo, plan.hi, walk);
        else t->rows.visit_range(plan.lo, plan.hi, walk);
        return out;
    }

    // candidatos por vía de acceso (punteros: el árbol está lockeado shared)
    std::vector<const SRow*> cands;
    if (plan.kind == Plan::SecPoint) {
        auto it = t->sec.find(plan.sec_col);
        if (it != t->sec.end())
            if (const auto* pks = it->second.find(plan.sec_eq))
                for (const auto& k : *pks) {
                    const SRow* r = t->rows.find(k);
                    if (r) cands.push_back(r);
                }
    } else if (plan.kind == Plan::SecRange) {
        // ORDEN por la MISMA columna indexada (asc/desc): recorrer el índice
        // ya ordenado con parada temprana en limit — O(límite) en vez de
        // O(rango) + ordenar.
        const bool order_by_idx =
            order_.size() == 1 && order_[0].first == plan.sec_col;
        auto it = t->sec.find(plan.sec_col);
        if (it != t->sec.end() && order_by_idx) {
            const bool desc = order_[0].second;
            std::vector<Row> out;
            out.reserve(std::min<std::size_t>(limit_, 64));
            auto emit = [&](const std::vector<std::string>& pks) {
                for (const auto& k : pks) {
                    if (out.size() >= limit_) return btree::Visit::Stop;
                    const SRow* r = t->rows.find(k);
                    if (r && pass_s(*r)) out.push_back(materialize(*r));
                }
                return out.size() >= limit_ ? btree::Visit::Stop
                                            : btree::Visit::Next;
            };
            if (desc)
                it->second.visit_range_reverse(plan.sec_lo, plan.sec_hi,
                    [&](const std::string&, const std::vector<std::string>& pks) {
                        return emit(pks);
                    });
            else
                it->second.visit_range(plan.sec_lo, plan.sec_hi,
                    [&](const std::string&, const std::vector<std::string>& pks) {
                        return emit(pks);
                    });
            return out;  // ya ordenados por la columna indexada
        }
        if (it != t->sec.end())
            it->second.visit_range(
                plan.sec_lo, plan.sec_hi,
                [&](const std::string&, const std::vector<std::string>& pks) {
                    for (const auto& k : pks) {
                        const SRow* r = t->rows.find(k);
                        if (r) cands.push_back(r);
                    }
                    return btree::Visit::Next;
                });
    } else if (plan.kind == Plan::PkPoint || plan.kind == Plan::PkRange) {
        t->rows.visit_range(plan.lo, plan.hi,
                            [&](const std::string&, const SRow& r) {
                                cands.push_back(&r);
                                return btree::Visit::Next;
                            });
    } else {
        cands.reserve(t->rows.size());
        t->rows.for_each([&](const std::string&, const SRow& r) {
            cands.push_back(&r);
            return btree::Visit::Next;
        });
    }

    // filtrar (plano) → materializar SOLO los supervivientes
    std::vector<const SRow*> hits;
    hits.reserve(std::min(cands.size(), limit_));
    for (const auto* r : cands)
        if (pass_s(*r)) hits.push_back(r);

    if (order_.empty()) {
        std::vector<Row> out;
        out.reserve(std::min(hits.size(), limit_));
        for (const auto* r : hits) {
            out.push_back(materialize(*r));
            if (out.size() >= limit_) break;
        }
        return out;
    }
    if (pk_order) {  // ordena por PK (los candidatos no vienen ordenados)
        const bool desc = order_[0].second;
        std::sort(hits.begin(), hits.end(),
                  [&](const SRow* a, const SRow* b) {
                      const std::string ka = srow_keyof(t->def, t->cmap, *a),
                                         kb = srow_keyof(t->def, t->cmap, *b);
                      return desc ? kb < ka : ka < kb;
                  });
    } else {
        std::stable_sort(hits.begin(), hits.end(),
                         [this, t](const SRow* a, const SRow* b) {
                             for (const auto& [col, desc] : order_) {
                                 const Value* pa = srow_get(t->cmap, *a, col);
                                 const Value* pb = srow_get(t->cmap, *b, col);
                                 const Value va = pa ? *pa : Value{};
                                 const Value vb = pb ? *pb : Value{};
                                 if (va < vb) return !desc;
                                 if (vb < va) return desc;
                             }
                             return false;
                         });
    }
    if (hits.size() > limit_) hits.resize(limit_);
    std::vector<Row> out;
    out.reserve(hits.size());
    for (const auto* r : hits) out.push_back(materialize(*r));
    return out;
}

std::size_t Query::count() const {
    auto* p = db_->p_.get();
    auto* t = p->find_table(table_);
    if (!t)
        throw Error(Error::Code::NoSuchTable,
                    "table '" + table_ + "' does not exist");
    std::shared_lock l(t->mu);
    const Plan plan = make_plan(t, cond_ ? &*cond_ : nullptr);
    auto pass_s = [&](const SRow& r) {
        if (pred_) {
            Row m = row_from_srow(t->def, r);
            if (!pred_(m)) return false;
        }
        if (cond_ && !eval_cond(*cond_, t->cmap, r)) return false;
        return true;
    };
    std::size_t n = 0;
    auto bump = [&](const SRow& r) {
        if (pass_s(r)) ++n;
        return n >= limit_ ? btree::Visit::Stop : btree::Visit::Next;
    };
    if (plan.kind == Plan::SecPoint) {
        auto it = t->sec.find(plan.sec_col);
        if (it != t->sec.end())
            if (const auto* pks = it->second.find(plan.sec_eq))
                for (const auto& k : *pks) {
                    const SRow* r = t->rows.find(k);
                    if (r && bump(*r) == btree::Visit::Stop) break;
                }
        return n;
    }
    if (plan.kind == Plan::SecRange) {
        auto it = t->sec.find(plan.sec_col);
        if (it != t->sec.end())
            it->second.visit_range(
                plan.sec_lo, plan.sec_hi,
                [&](const std::string&, const std::vector<std::string>& pks) {
                    for (const auto& k : pks) {
                        const SRow* r = t->rows.find(k);
                        if (r && bump(*r) == btree::Visit::Stop)
                            return btree::Visit::Stop;
                    }
                    return btree::Visit::Next;
                });
        return n;
    }
    t->rows.visit_range(plan.lo, plan.hi,
                        [&](const std::string&, const SRow& r) {
                            return bump(r);
                        });
    return n;
}

// ---------------------------------------------------------------------------
// TableRef — multimodal KNN + full-text search
// ---------------------------------------------------------------------------

std::vector<std::pair<std::string, double>> TableRef::knn(
    const std::string& column, const Vec& query, std::size_t k,
    Metric metric) const {
    auto* p = db_->p_.get();
    auto* t = p->must_table(name_);
    if (query.empty())
        throw Error(Error::Code::BadArgument, "query vector is empty");
    if (k == 0) return {};
    std::shared_lock l(t->mu);
    double qnorm = 0;
    for (float f : query) qnorm += static_cast<double>(f) * f;
    qnorm = std::sqrt(qnorm);
    // 0.7: heap top-k en vez de puntuar TODO y ordenar — memoria O(k),
    // sin copia de claves salvo para los que entran al heap, y abandono
    // temprano por dimensión en L2 (d2 solo crece). Desempate por clave
    // para un orden determinista (0.6 dependía del orden de std::sort).
    struct Hit { double score; std::string key; };
    // orden de prioridad: mejor = menor. El heap (con `better` como "menor")
    // deja al FRENTE el peor de los k guardados — el candidato a expulsar.
    const auto better = [metric](const Hit& a, const Hit& b) {
        if (a.score != b.score) return metric == Metric::Cosine ? a.score > b.score
                                                                : a.score < b.score;
        return a.key < b.key;
    };
    std::vector<Hit> heap;
    heap.reserve(k + 1);
    const std::int32_t vord = t->cmap.find(column);
    t->rows.for_each([&](const std::string& key, const SRow& r) {
        const Value* pv = vord >= 0 && r.has(static_cast<std::uint32_t>(vord))
                              ? &r.flat[vord]
                              : srow_get(t->cmap, r, column);
        if (!pv || !pv->is_vector()) return btree::Visit::Next;
        const Vec& v = pv->as_vector();
        if (v.size() != query.size())
            return btree::Visit::Next;  // dimension mismatch → skip
        double score;
        if (metric == Metric::Cosine) {
            double dot = 0, norm = 0;
            for (std::size_t i = 0; i < v.size(); i++) {
                dot += static_cast<double>(query[i]) * v[i];
                norm += static_cast<double>(v[i]) * v[i];
            }
            const double denom = qnorm * std::sqrt(norm);
            if (denom == 0)
                return btree::Visit::Next;  // undefined direction
            score = dot / denom;
        } else {
            const double bound =  // peor distancia guardada (abandono temprano)
                heap.size() >= k ? heap.front().score
                                 : std::numeric_limits<double>::infinity();
            double d2 = 0;
            bool abandoned = false;
            for (std::size_t i = 0; i < v.size(); i++) {
                const double d = static_cast<double>(query[i]) - v[i];
                d2 += d * d;
                if (d2 > bound) { abandoned = true; break; }
            }
            if (abandoned) return btree::Visit::Next;
            score = std::sqrt(d2);
        }
        if (heap.size() < k) {
            heap.push_back({score, key});
            std::push_heap(heap.begin(), heap.end(), better);
        } else if (better({score, key}, heap.front())) {
            std::pop_heap(heap.begin(), heap.end(), better);
            heap.back() = {score, key};
            std::push_heap(heap.begin(), heap.end(), better);
        }
        return btree::Visit::Next;
    });
    std::sort(heap.begin(), heap.end(), better);
    std::vector<std::pair<std::string, double>> out;
    out.reserve(heap.size());
    for (auto& h : heap) out.emplace_back(std::move(h.key), h.score);
    return out;
}

std::vector<Row> TableRef::search(const std::string& column,
                                  const std::string& query,
                                  std::size_t limit) const {
    auto* p = db_->p_.get();
    auto* t = p->must_table(name_);
    std::vector<std::string> tokens;
    fts::tokenize(query, tokens);
    if (tokens.empty()) return {};
    std::vector<Row> out;
    // 0.7: FTS perezoso — la primera búsqueda construye el índice invertido
    // (una sola vez, bajo lock exclusivo con doble chequeo)
    if (!t->fts_built) {
        std::unique_lock bl(t->mu);
        if (!t->fts_built) DbEngine::fts_build(*t);
    }
    std::shared_lock l(t->mu);
    std::vector<std::string> keys;
    auto idx = t->fts.find(column);
    if (idx != t->fts.end()) {
        keys = idx->second.query_and(tokens);
    } else {  // brute-force fallback for non-indexed text columns
        t->rows.for_each([&](const std::string& fkey, const SRow& r) {
            const Value* v = srow_get(t->cmap, r, column);
            if (!v || !v->is_text()) return btree::Visit::Next;
            std::vector<std::string> toks;
            fts::tokenize(v->as_text(), toks);
            std::set<std::string> present(toks.begin(), toks.end());
            bool all = true;
            for (const auto& q : tokens)
                if (!present.count(q)) { all = false; break; }
            if (all) keys.push_back(fkey);
            return btree::Visit::Next;
        });
        std::sort(keys.begin(), keys.end());
    }
    for (const auto& k : keys) {
        if (out.size() >= limit) break;
        const SRow* r = t->rows.find(k);
        if (r) out.push_back(row_from_srow(t->def, *r));
    }
    return out;
}

// ---------------------------------------------------------------------------
// TableRef — real-time (watch + changelog cursor)
// ---------------------------------------------------------------------------

WatchId TableRef::watch(WatchFn fn) const {
    auto* p = db_->p_.get();
    std::unique_lock w(p->watch_mu);
    const WatchId id = p->next_watch_id++;
    p->watchers.emplace(id, std::make_pair(name_, std::move(fn)));
    return id;
}

void TableRef::unwatch(WatchId id) const {
    auto* p = db_->p_.get();
    std::unique_lock w(p->watch_mu);
    p->watchers.erase(id);
}

ChangeList TableRef::changes(std::uint64_t since_cursor) const {
    auto* p = db_->p_.get();
    auto* t = p->must_table(name_);
    ChangeList out;
    std::lock_guard<std::mutex> l(t->log_mu);
    out.next_cursor = since_cursor;
    if (t->log.empty()) return out;
    const std::uint64_t oldest = t->log.front().seq;
    out.truncated = since_cursor + 1 < oldest;
    for (const auto& c : t->log) {
        if (c.seq <= since_cursor) continue;
        out.events.push_back(c);
        out.next_cursor = c.seq;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------

Transaction::Transaction(Database& db) : db_(&db) {}

Transaction::Transaction(Transaction&& o) noexcept
    : db_(o.db_), ops_(std::move(o.ops_)), done_(o.done_) {
    o.done_ = true;
}

Transaction::~Transaction() {
    if (!done_) rollback();
}

void Transaction::rollback() noexcept {
    ops_.clear();
    done_ = true;
}

void Transaction::commit() {
    if (done_)
        throw Error(Error::Code::TxDiscarded, "transaction already finished");
    auto* p = db_->p_.get();
    std::lock_guard<std::mutex> w(p->writer_mu);
    // validate every op against the CURRENT catalog before writing anything:
    // a failure aborts the whole transaction with no WAL bytes appended.
    std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> encoded;
    encoded.reserve(ops_.size());
    for (auto& op : ops_) {
        auto* t = p->must_table(op.table);
        std::vector<std::uint8_t> payload;
        codec::put_str(payload, op.table);
        if (op.kind == codec::K_PUT) {
            Row r = validate_row(t->def, op.row, p->opt.max_blob_size, false);
            codec::encode_row(payload, r);
        } else {
            codec::put_str(payload, op.key);
        }
        encoded.emplace_back(op.kind, std::move(payload));
    }
    const auto events = p->commit_ops_locked(std::move(encoded));
    ops_.clear();
    done_ = true;
    p->dispatch(events);
}

void TxTable::insert(Row r) const {
    if (tx_->done_)
        throw Error(Error::Code::TxDiscarded, "transaction already finished");
    tx_->ops_.push_back({codec::K_PUT, name_, std::move(r), {}});
}

void TxTable::erase(const std::string& key) const {
    if (tx_->done_)
        throw Error(Error::Code::TxDiscarded, "transaction already finished");
    tx_->ops_.push_back({codec::K_DEL, name_, {}, key});
}

} // namespace gina
