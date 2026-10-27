// GinaDB Lite 0.7 — btree.hpp: B+Tree en memoria (índice primario agrupado).
//
// Rol en el motor:
//   * Almacén de filas por clave primaria (Table::rows): hojas enlazadas →
//     recorrido ordenado por PK sin ordenar (order_by(pk), checkpoints),
//     rangos PK (between) y búsqueda puntual O(log N).
//   * Estructura de los índices secundarios: Map<IndexKey, std::vector<PK>>.
//
// Diseño:
//   * Nodos interior: hasta INNER_MAX claves separadoras / INNER_MAX+1 hijos.
//     Invariante: keys[i] == primera clave del subárbol children[i+1].
//   * Hojas: hasta LEAF_MAX entradas (clave,valor), enlazadas prev/next.
//   * Inserción/borrado recursivos con reparto y fusión (borrow/merge);
//     tras cada fix se refrescan los separadores afectados desde el hecho
//     (first_key_of), de modo que el invariante se restaura siempre.
//   * bulk_load(): construcción bottom-up desde pares ya ordenados —
//     vía rápida de arranque desde un snapshot (que ya viene ordenado).
//   * visit_range / visit_range_reverse: recorridos de rango con parada
//     temprana, usados por el planner de consultas (order_by + limit).
//
// Concurrencia: ninguna interna; la protección la da el shared_mutex de la
// tabla (lectores con shared_lock, escritores con unique_lock).
#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace gina::btree {

inline constexpr std::size_t LEAF_MAX = 64;   // entradas por hoja
inline constexpr std::size_t INNER_MAX = 64;  // claves por nodo interior
inline constexpr std::size_t LEAF_MIN = LEAF_MAX / 2;
inline constexpr std::size_t INNER_MIN = INNER_MAX / 2;

namespace detail {

struct NodeBase {
    bool leaf;
    explicit NodeBase(bool l) : leaf(l) {}
};

struct Leaf : NodeBase {
    std::vector<std::string> keys;
    std::vector<std::size_t> vals;   // índice al pool de valores
    Leaf* prev = nullptr;
    Leaf* next = nullptr;
    Leaf() : NodeBase(true) {}
};

struct Inner : NodeBase {
    std::vector<std::string> keys;      // keys[i] = 1ª clave de children[i+1]
    std::vector<NodeBase*> children;    // size == keys.size()+1
    Inner() : NodeBase(false) {}
};

} // namespace detail

enum class Visit : std::uint8_t { Next = 0, Stop = 1 };

template <class T>
class Map {
public:
    Map() = default;
    ~Map() { destroy(root_); }
    Map(const Map&) = delete;
    Map& operator=(const Map&) = delete;
    Map(Map&& o) noexcept
        : root_(o.root_), count_(o.count_), first_(o.first_), last_(o.last_),
          pool_(std::move(o.pool_)), free_(std::move(o.free_)) {
        o.root_ = nullptr; o.first_ = o.last_ = nullptr; o.count_ = 0;
    }
    Map& operator=(Map&& o) noexcept {
        if (this != &o) {
            destroy(root_);
            root_ = o.root_; count_ = o.count_; first_ = o.first_;
            last_ = o.last_; pool_ = std::move(o.pool_); free_ = std::move(o.free_);
            o.root_ = nullptr; o.first_ = o.last_ = nullptr; o.count_ = 0;
        }
        return *this;
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

    // ---- búsqueda -----------------------------------------------------------
    [[nodiscard]] const T* find(const std::string& key) const {
        const detail::NodeBase* n = root_;
        while (n) {
            if (n->leaf) {
                const auto* lf = static_cast<const detail::Leaf*>(n);
                const auto it =
                    std::lower_bound(lf->keys.begin(), lf->keys.end(), key);
                if (it == lf->keys.end() || *it != key) return nullptr;
                return &pool_[lf->vals[static_cast<std::size_t>(
                    it - lf->keys.begin())]];
            }
            const auto* in = static_cast<const detail::Inner*>(n);
            const auto it =
                std::upper_bound(in->keys.begin(), in->keys.end(), key);
            n = in->children[static_cast<std::size_t>(it - in->keys.begin())];
        }
        return nullptr;
    }
    [[nodiscard]] T* find(const std::string& key) {
        return const_cast<T*>(static_cast<const Map*>(this)->find(key));
    }

    // ---- inserción ----------------------------------------------------------
    // insert_or_assign: devuelve (puntero estable al valor, true si nueva).
    std::pair<T*, bool> insert_or_assign(const std::string& key, T value) {
        if (!root_) {
            auto* lf = new detail::Leaf();
            lf->keys.push_back(key);
            lf->vals.push_back(pool_alloc(std::move(value)));
            root_ = first_ = last_ = lf;
            count_ = 1;
            return {&pool_[lf->vals[0]], true};
        }
        std::string up;
        detail::NodeBase* right = nullptr;
        const bool fresh = insert_rec(root_, key, std::move(value), up, right);
        if (right) {
            auto* nr = new detail::Inner();
            nr->keys.push_back(std::move(up));
            nr->children.push_back(root_);
            nr->children.push_back(right);
            root_ = nr;
        }
        return {find(key), fresh};
    }

    // ---- borrado ------------------------------------------------------------
    bool erase(const std::string& key) {
        if (!root_) return false;
        bool removed = false;
        if (erase_rec(root_, key, removed) && root_ && !root_->leaf) {
            // la raíz solo puede quedar "bajo mínimo" teniendo 1 hijo
            auto* old = static_cast<detail::Inner*>(root_);
            if (old->children.size() == 1) {
                root_ = old->children[0];
                old->children.clear();
                delete old;
            }
        }
        if (root_ && root_->leaf) {
            auto* lf = static_cast<detail::Leaf*>(root_);
            if (lf->keys.empty()) {
                delete root_;
                root_ = first_ = last_ = nullptr;
            }
        }
        if (removed) count_--;
        return removed;
    }

    // ---- recorridos ----------------------------------------------------------
    void for_each(
        const std::function<Visit(const std::string&, const T&)>& fn) const {
        for (const detail::Leaf* lf = first_; lf; lf = lf->next)
            for (std::size_t i = 0; i < lf->keys.size(); i++)
                if (fn(lf->keys[i], pool_[lf->vals[i]]) == Visit::Stop) return;
    }

    // 0.7: recorrido MUTABLE (re-schema in situ de filas planas). Los valores
    // viven en el pool (deque) con índices estables: mutarlos es seguro.
    void for_each_mut(
        const std::function<Visit(const std::string&, T&)>& fn) {
        for (detail::Leaf* lf = first_; lf; lf = lf->next)
            for (std::size_t i = 0; i < lf->keys.size(); i++)
                if (fn(lf->keys[i], pool_[lf->vals[i]]) == Visit::Stop) return;
    }

    // Entradas con (lower vacía → -inf) <= clave < (upper vacía → +inf).
    // Devuelve cuántas visitó (incluida la que detuvo el recorrido).
    std::size_t visit_range(
        const std::string& lower, const std::string& upper,
        const std::function<Visit(const std::string&, const T&)>& fn) const {
        std::size_t seen = 0;
        const detail::Leaf* lf = seek_leaf(lower);
        std::size_t i = 0;
        if (lf && !lower.empty())
            i = static_cast<std::size_t>(
                std::lower_bound(lf->keys.begin(), lf->keys.end(), lower) -
                lf->keys.begin());
        for (; lf; lf = lf->next, i = 0)
            for (; i < lf->keys.size(); i++) {
                if (!upper.empty() && !(lf->keys[i] < upper)) return seen;
                ++seen;
                if (fn(lf->keys[i], pool_[lf->vals[i]]) == Visit::Stop)
                    return seen;
            }
        return seen;
    }

    // Igual que visit_range pero en orden de clave descendente.
    std::size_t visit_range_reverse(
        const std::string& lower, const std::string& upper,
        const std::function<Visit(const std::string&, const T&)>& fn) const {
        std::size_t seen = 0;
        const detail::Leaf* lf;
        std::size_t i;
        if (upper.empty()) {
            lf = last_;
            i = lf ? lf->keys.size() : 0;
        } else {
            lf = seek_leaf(upper);
            i = lf ? static_cast<std::size_t>(
                         std::lower_bound(lf->keys.begin(), lf->keys.end(),
                                          upper) - lf->keys.begin())
                   : 0;
        }
        while (lf) {
            while (i-- > 0) {
                if (!lower.empty() && lf->keys[i] < lower) return seen;
                ++seen;
                if (fn(lf->keys[i], pool_[lf->vals[i]]) == Visit::Stop)
                    return seen;
            }
            lf = lf->prev;
            i = lf ? lf->keys.size() : 0;
        }
        return seen;
    }

    // Primer par con clave >= key. Solo pruebas/herramientas.
    [[nodiscard]] bool lower_bound_copy(const std::string& key, std::string& k_out,
                                        T& v_out) const {
        bool found = false;
        visit_range(key, "", [&](const std::string& k, const T& v) {
            k_out = k;
            v_out = v;
            found = true;
            return Visit::Stop;
        });
        return found;
    }

    // ---- carga masiva ---------------------------------------------------------
    // `sorted` DEBE venir ordenado por clave y sin duplicados (el snapshot se
    // escribe así). Reemplaza cualquier contenido previo.
    void bulk_load(std::vector<std::pair<std::string, T>> sorted) {
        destroy(root_);
        root_ = nullptr; first_ = last_ = nullptr; count_ = sorted.size();
        pool_.clear(); free_.clear();
        if (sorted.empty()) return;
        std::vector<detail::Leaf*> leaves;
        for (std::size_t i = 0; i < sorted.size();) {
            auto* lf = new detail::Leaf();
            const std::size_t n =
                std::min<std::size_t>(LEAF_MAX, sorted.size() - i);
            lf->keys.reserve(n);
            lf->vals.reserve(n);
            for (std::size_t j = 0; j < n; j++, i++) {
                lf->keys.push_back(std::move(sorted[i].first));
                lf->vals.push_back(pool_alloc(std::move(sorted[i].second)));
            }
            if (!leaves.empty()) {
                leaves.back()->next = lf;
                lf->prev = leaves.back();
            }
            leaves.push_back(lf);
        }
        first_ = leaves.front();
        last_ = leaves.back();
        std::vector<detail::NodeBase*> level(leaves.begin(), leaves.end());
        while (level.size() > 1) {
            std::vector<detail::NodeBase*> up;
            for (std::size_t i = 0; i < level.size();) {
                auto* in = new detail::Inner();
                const std::size_t n =
                    std::min<std::size_t>(INNER_MAX + 1, level.size() - i);
                for (std::size_t j = 0; j < n; j++, i++) {
                    in->children.push_back(level[i]);
                    if (j > 0) in->keys.push_back(first_key_of(level[i]));
                }
                up.push_back(in);
            }
            level = std::move(up);
        }
        root_ = level[0];
    }

    // Validación estructural (solo pruebas): orden estricto, ocupación mínima
    // (salvo raíz), separadores == primera clave del hijo derecho, conteo.
    [[nodiscard]] bool validate() const {
        if (!root_) return count_ == 0 && first_ == nullptr && last_ == nullptr;
        std::size_t total = 0;
        if (!validate_node(root_, nullptr, nullptr, total)) return false;
        return total == count_;
    }

    void clear() {
        destroy(root_);
        root_ = first_ = last_ = nullptr;
        count_ = 0;
        pool_.clear();
        free_.clear();
    }

private:
    detail::NodeBase* root_ = nullptr;
    std::size_t count_ = 0;
    detail::Leaf* first_ = nullptr, * last_ = nullptr;

    // Pool de valores en deque: referencias ESTABLES aunque crezca (los
    // nodos guardan índices; el borrado recicla huecos en LIFO).
    std::deque<T> pool_;
    std::vector<std::size_t> free_;

    [[nodiscard]] std::size_t pool_alloc(T&& v) {
        if (!free_.empty()) {
            const std::size_t idx = free_.back();
            free_.pop_back();
            pool_[idx] = std::move(v);
            return idx;
        }
        pool_.push_back(std::move(v));
        return pool_.size() - 1;
    }

    [[nodiscard]] const detail::Leaf* seek_leaf(const std::string& key) const {
        if (!root_) return nullptr;
        if (key.empty()) return first_;
        const detail::NodeBase* n = root_;
        while (!n->leaf) {
            const auto* in = static_cast<const detail::Inner*>(n);
            const auto it =
                std::upper_bound(in->keys.begin(), in->keys.end(), key);
            n = in->children[static_cast<std::size_t>(it - in->keys.begin())];
        }
        return static_cast<const detail::Leaf*>(n);
    }

    [[nodiscard]] static const std::string& first_key_of(
        const detail::NodeBase* n) {
        while (!n->leaf) n = static_cast<const detail::Inner*>(n)->children[0];
        return static_cast<const detail::Leaf*>(n)->keys[0];
    }

    static void destroy(detail::NodeBase* n) {
        if (!n) return;
        if (!n->leaf)
            for (auto* c : static_cast<detail::Inner*>(n)->children) destroy(c);
        delete n;
    }

    // ---- inserción recursiva --------------------------------------------------
    bool insert_rec(detail::NodeBase* n, const std::string& key, T value,
                    std::string& up, detail::NodeBase*& right) {
        if (n->leaf) {
            auto* lf = static_cast<detail::Leaf*>(n);
            auto it = std::lower_bound(lf->keys.begin(), lf->keys.end(), key);
            const auto idx = static_cast<std::size_t>(it - lf->keys.begin());
            if (it != lf->keys.end() && *it == key) {
                pool_[lf->vals[idx]] = std::move(value);
                return false;
            }
            lf->keys.insert(it, key);
            lf->vals.insert(lf->vals.begin() + static_cast<std::ptrdiff_t>(idx),
                            pool_alloc(std::move(value)));
            count_++;
            if (lf->keys.size() > LEAF_MAX) split_leaf(lf, up, right);
            return true;
        }
        auto* in = static_cast<detail::Inner*>(n);
        const auto it = std::upper_bound(in->keys.begin(), in->keys.end(), key);
        const auto ci = static_cast<std::size_t>(it - in->keys.begin());
        const bool fresh =
            insert_rec(in->children[ci], key, std::move(value), up, right);
        if (right) {
            in->keys.insert(in->keys.begin() + static_cast<std::ptrdiff_t>(ci),
                            std::move(up));
            in->children.insert(in->children.begin() +
                                    static_cast<std::ptrdiff_t>(ci + 1),
                                right);
            right = nullptr;
            if (in->keys.size() > INNER_MAX) split_inner(in, up, right);
        }
        return fresh;
    }

    void split_leaf(detail::Leaf* lf, std::string& up, detail::NodeBase*& right) {
        auto* rn = new detail::Leaf();
        const std::size_t mid = lf->keys.size() / 2;
        rn->keys.assign(lf->keys.begin() + static_cast<std::ptrdiff_t>(mid),
                        lf->keys.end());
        rn->vals.assign(lf->vals.begin() + static_cast<std::ptrdiff_t>(mid),
                        lf->vals.end());
        lf->keys.resize(mid);
        lf->vals.resize(mid);
        rn->next = lf->next;
        rn->prev = lf;
        if (lf->next) lf->next->prev = rn;
        lf->next = rn;
        if (last_ == lf) last_ = rn;
        up = rn->keys.front();
        right = rn;
    }

    void split_inner(detail::Inner* in, std::string& up, detail::NodeBase*& right) {
        auto* rn = new detail::Inner();
        const std::size_t mid = in->keys.size() / 2;  // clave promovida
        up = in->keys[mid];
        rn->keys.assign(in->keys.begin() + static_cast<std::ptrdiff_t>(mid + 1),
                        in->keys.end());
        rn->children.assign(in->children.begin() +
                                static_cast<std::ptrdiff_t>(mid + 1),
                            in->children.end());
        in->keys.resize(mid);
        in->children.resize(mid + 1);
        right = rn;
    }

    // ---- borrado recursivo -----------------------------------------------------
    // Devuelve true si `n` quedó por debajo del mínimo (el padre debe arreglar).
    bool erase_rec(detail::NodeBase* n, const std::string& key, bool& removed) {
        if (n->leaf) {
            auto* lf = static_cast<detail::Leaf*>(n);
            const auto it =
                std::lower_bound(lf->keys.begin(), lf->keys.end(), key);
            if (it == lf->keys.end() || *it != key) return false;
            const auto idx = static_cast<std::size_t>(it - lf->keys.begin());
            const std::size_t vidx = lf->vals[idx];
            free_.push_back(vidx);
            pool_[vidx] = T{};  // libera el valor retenido
            lf->keys.erase(it);
            lf->vals.erase(lf->vals.begin() + static_cast<std::ptrdiff_t>(idx));
            removed = true;
            return n != root_ && lf->keys.size() < LEAF_MIN;
        }
        auto* in = static_cast<detail::Inner*>(n);
        const auto it = std::upper_bound(in->keys.begin(), in->keys.end(), key);
        const std::size_t ci = static_cast<std::size_t>(it - in->keys.begin());
        if (erase_rec(in->children[ci], key, removed)) fix_underflow(in, ci);
        refresh_separators(in, ci);
        return n != root_ && in->keys.size() < INNER_MIN;
    }

    void fix_underflow(detail::Inner* in, std::size_t ci) {
        detail::NodeBase* child = in->children[ci];
        if (child->leaf) {
            auto* lf = static_cast<detail::Leaf*>(child);
            auto* left = ci > 0 ? static_cast<detail::Leaf*>(in->children[ci - 1])
                                : nullptr;
            auto* right = ci + 1 < in->children.size()
                              ? static_cast<detail::Leaf*>(in->children[ci + 1])
                              : nullptr;
            if (left && left->keys.size() > LEAF_MIN) {
                lf->keys.insert(lf->keys.begin(), left->keys.back());
                lf->vals.insert(lf->vals.begin(), left->vals.back());
                left->keys.pop_back();
                left->vals.pop_back();
            } else if (right && right->keys.size() > LEAF_MIN) {
                lf->keys.push_back(right->keys.front());
                lf->vals.push_back(right->vals.front());
                right->keys.erase(right->keys.begin());
                right->vals.erase(right->vals.begin());
            } else if (left) {
                left->keys.insert(left->keys.end(), lf->keys.begin(),
                                  lf->keys.end());
                left->vals.insert(left->vals.end(), lf->vals.begin(),
                                  lf->vals.end());
                left->next = lf->next;
                if (lf->next) lf->next->prev = left;
                if (last_ == lf) last_ = left;
                detach_child(in, ci);
            } else if (right) {
                lf->keys.insert(lf->keys.end(), right->keys.begin(),
                                right->keys.end());
                lf->vals.insert(lf->vals.end(), right->vals.begin(),
                                right->vals.end());
                lf->next = right->next;
                if (right->next) right->next->prev = lf;
                if (last_ == right) last_ = lf;
                detach_child(in, ci + 1);
            }
            refresh_separators(in, ci);
            return;
        }
        auto* child_in = static_cast<detail::Inner*>(child);
        auto* left = ci > 0 ? static_cast<detail::Inner*>(in->children[ci - 1])
                            : nullptr;
        auto* right = ci + 1 < in->children.size()
                          ? static_cast<detail::Inner*>(in->children[ci + 1])
                          : nullptr;
        if (left && left->keys.size() > INNER_MIN) {
            child_in->keys.insert(child_in->keys.begin(), in->keys[ci - 1]);
            child_in->children.insert(child_in->children.begin(),
                                      left->children.back());
            left->keys.pop_back();
            left->children.pop_back();
        } else if (right && right->keys.size() > INNER_MIN) {
            child_in->keys.push_back(in->keys[ci]);
            child_in->children.push_back(right->children.front());
            right->keys.erase(right->keys.begin());
            right->children.erase(right->children.begin());
        } else if (left) {
            left->keys.push_back(in->keys[ci - 1]);
            left->keys.insert(left->keys.end(), child_in->keys.begin(),
                              child_in->keys.end());
            left->children.insert(left->children.end(), child_in->children.begin(),
                                  child_in->children.end());
            detach_child(in, ci);
        } else if (right) {
            child_in->keys.push_back(in->keys[ci]);
            child_in->keys.insert(child_in->keys.end(), right->keys.begin(),
                                  right->keys.end());
            child_in->children.insert(child_in->children.end(),
                                      right->children.begin(),
                                      right->children.end());
            detach_child(in, ci + 1);
        }
        refresh_separators(in, ci);
    }

    // Elimina al hijo en `pos` del padre `in` (su contenido ya fue absorbido
    // por un hermano) y libera el nodo.
    void detach_child(detail::Inner* in, std::size_t pos) {
        detail::NodeBase* child = in->children[pos];
        in->children.erase(in->children.begin() + static_cast<std::ptrdiff_t>(pos));
        in->keys.erase(in->keys.begin() +
                       static_cast<std::ptrdiff_t>(pos == 0 ? 0 : pos - 1));
        delete child;
    }

    // Recalcula los separadores alrededor del hijo ci desde el hecho.
    void refresh_separators(detail::Inner* in, std::size_t ci) {
        const std::size_t from = ci == 0 ? 0 : ci - 1;
        const std::size_t to = std::min(ci + 1, in->keys.size());
        for (std::size_t i = from; i <= to && i < in->keys.size(); i++)
            if (i + 1 < in->children.size())
                in->keys[i] = first_key_of(in->children[i + 1]);
    }

    [[nodiscard]] bool validate_node(const detail::NodeBase* n,
                                     const std::string* lo, const std::string* hi,
                                     std::size_t& total) const {
        if (n->leaf) {
            const auto* lf = static_cast<const detail::Leaf*>(n);
            if (lf->keys.size() != lf->vals.size()) return false;
            for (std::size_t i = 0; i + 1 < lf->keys.size(); i++)
                if (!(lf->keys[i] < lf->keys[i + 1])) return false;
            if (!lf->keys.empty()) {
                // lo == primera clave de la hoja cuando la hoja no es la
                // primera de su subtree (el separador ES esa clave): igualdad
                // válida; solo está mal que la primera clave sea menor.
                if (lo && lf->keys.front() < *lo) return false;
                if (hi && !(lf->keys.back() < *hi)) return false;
            }
            if (n != root_ && lf->keys.size() < LEAF_MIN) return false;
            total += lf->keys.size();
            return true;
        }
        const auto* in = static_cast<const detail::Inner*>(n);
        if (in->children.size() != in->keys.size() + 1) return false;
        if (n != root_ && in->keys.size() < INNER_MIN) return false;
        for (std::size_t i = 0; i + 1 < in->keys.size(); i++)
            if (!(in->keys[i] < in->keys[i + 1])) return false;
        for (std::size_t i = 0; i < in->keys.size(); i++)
            if (in->keys[i] != first_key_of(in->children[i + 1])) return false;
        for (std::size_t i = 0; i < in->children.size(); i++) {
            const std::string* clo = i == 0 ? lo : &in->keys[i - 1];
            const std::string* chi =
                i == in->children.size() - 1 ? hi : &in->keys[i];
            if (!validate_node(in->children[i], clo, chi, total)) return false;
        }
        return true;
    }
};

} // namespace gina::btree
