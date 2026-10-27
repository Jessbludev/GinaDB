// GinaDB Lite 0.7 — btree_test: diferencial contra std::map + invariantes.
// Estrategia: un PRNG con semilla fija genera secuencias aleatorias de
// inserción/actualización/borrado/rango; el B+Tree y un std::map de referencia
// deben quedar SIEMPRE idénticos, y validate() debe pasar en cada punto de
// control (orden estricto, ocupación mínima, separadores exactos, conteo).
#include "btree.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

using gina::btree::Map;
using gina::btree::Visit;

static int failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

static void compare(const Map<std::string>& bt, const std::map<std::string, std::string>& ref) {
    CHECK(bt.size() == ref.size());
    CHECK(bt.validate());
    std::vector<std::pair<std::string, std::string>> got;
    bt.for_each([&](const std::string& k, const std::string& v) {
        got.emplace_back(k, v);
        return Visit::Next;
    });
    CHECK(got.size() == ref.size());
    std::size_t i = 0;
    for (const auto& [k, v] : ref) {
        if (i >= got.size()) break;
        CHECK(got[i].first == k);
        CHECK(got[i].second == v);
        i++;
    }
}

static std::string key_of(int i) {
    // mezcla de longitudes y prefijos comunes (peor caso para comparadores)
    char buf[64];
    if (i % 3 == 0) std::snprintf(buf, sizeof buf, "user-%09d", i);
    else if (i % 3 == 1) std::snprintf(buf, sizeof buf, "u%09d", i);
    else std::snprintf(buf, sizeof buf, "clave-larga-con-prefixo-comun-%09d", i);
    return buf;
}

static void test_sequential() {
    Map<std::string> bt;
    std::map<std::string, std::string> ref;
    for (int i = 0; i < 50000; i++) {
        const std::string k = key_of(i);
        bt.insert_or_assign(k, "v" + std::to_string(i));
        ref.emplace(k, "v" + std::to_string(i));
    }
    compare(bt, ref);
    // búsqueda puntual sobre todo el conjunto
    for (int i = 0; i < 50000; i += 7) {
        const std::string k = key_of(i);
        const std::string* v = bt.find(k);
        CHECK(v != nullptr);
        CHECK(*v == "v" + std::to_string(i));
    }
    CHECK(bt.find("no-existe") == nullptr);
    // borrado secuencial de la mitad
    for (int i = 0; i < 50000; i += 2) {
        CHECK(bt.erase(key_of(i)) == true);
        ref.erase(key_of(i));
    }
    compare(bt, ref);
    CHECK(bt.erase(key_of(0)) == false);  // ya borrado
    std::printf("ok sequential (size=%zu)\n", bt.size());
}

static void test_random(std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    Map<std::string> bt;
    std::map<std::string, std::string> ref;
    const int space = 4000;
    for (int op = 0; op < 60000; op++) {
        const int action = static_cast<int>(rng() % 100);
        const std::string k = key_of(static_cast<int>(rng() % space));
        if (action < 55) {  // insert/overwrite
            const std::string v = "op" + std::to_string(op);
            const auto [it, fresh] = ref.emplace(k, v);
            if (!fresh) it->second = v;
            bt.insert_or_assign(k, v);
        } else if (action < 90) {  // erase
            CHECK(bt.erase(k) == ref.erase(k));
        } else {  // find
            const std::string* v = bt.find(k);
            auto it = ref.find(k);
            const bool has_btree = v != nullptr, has_ref = it != ref.end();
            CHECK(has_btree == has_ref);
            if (v && it != ref.end()) CHECK(*v == it->second);
        }
        if (op % 5000 == 4999) compare(bt, ref);
    }
    compare(bt, ref);
    std::printf("ok random seed=%llu (size=%zu)\n",
                static_cast<unsigned long long>(seed), bt.size());
}

static void test_bulk_and_range() {
    std::mt19937_64 rng(99);
    std::map<std::string, std::string> ref;
    for (int i = 0; i < 20000; i++)
        ref.emplace(key_of(i), "bulk" + std::to_string(i));
    std::vector<std::pair<std::string, std::string>> sorted(ref.begin(), ref.end());
    Map<std::string> bt;
    bt.bulk_load(sorted);
    compare(bt, ref);

    // rangos con y sin cotas, directos e inversos
    for (int t = 0; t < 200; t++) {
        std::string lo = key_of(static_cast<int>(rng() % 20000));
        std::string hi = key_of(static_cast<int>(rng() % 20000));
        if (hi < lo) std::swap(lo, hi);
        const bool empty_lo = (t % 4 == 0), empty_hi = (t % 5 == 0);
        const std::string L = empty_lo ? "" : lo, H = empty_hi ? "" : hi;
        std::vector<std::string> fwd;
        bt.visit_range(L, H, [&](const std::string& k, const std::string& v) {
            fwd.push_back(k + "=" + v);
            return Visit::Next;
        });
        std::vector<std::string> want;
        for (auto it = ref.lower_bound(empty_lo ? "\x01" : lo); it != ref.end(); ++it) {
            if (!empty_hi && !(it->first < hi)) break;
            want.push_back(it->first + "=" + it->second);
        }
        CHECK(fwd == want);
        std::vector<std::string> rev;
        bt.visit_range_reverse(L, H, [&](const std::string& k, const std::string& v) {
            rev.push_back(k + "=" + v);
            return Visit::Next;
        });
        std::vector<std::string> want_rev(want.rbegin(), want.rend());
        CHECK(rev == want_rev);
    }
    // bulk + mutación posterior (splits tras carga masiva)
    for (int i = 20000; i < 30000; i++) {
        bt.insert_or_assign(key_of(i), "post" + std::to_string(i));
        ref.emplace(key_of(i), "post" + std::to_string(i));
    }
    for (int i = 0; i < 20000; i += 3)
        if ((i % 2) == 0) {
            bt.erase(key_of(i));
            ref.erase(key_of(i));
        }
    compare(bt, ref);
    std::printf("ok bulk+range (size=%zu)\n", bt.size());
}

static void test_lifecycle() {
    Map<std::string> a;
    a.insert_or_assign("x", "1");
    a.insert_or_assign("y", "2");
    Map<std::string> b(std::move(a));
    CHECK(b.size() == 2 && b.validate());
    CHECK(b.find("x") && *b.find("x") == "1");
    // overwrite in situ: el puntero debe seguir válido y actualizado
    std::string* px = b.find("x");
    b.insert_or_assign("x", "99");
    CHECK(px != nullptr);          // deque: referencias estables
    CHECK(*b.find("x") == "99");
    CHECK(b.erase("x") && b.size() == 1);
    CHECK(b.erase("y") && b.empty());
    CHECK(b.validate());
    b.insert_or_assign("z", "7");
    CHECK(b.size() == 1 && *b.find("z") == "7");
    b.clear();
    CHECK(b.empty() && b.validate());
    // stress de un solo elemento (raíz hoja vaciándose muchas veces)
    for (int i = 0; i < 1000; i++) {
        b.insert_or_assign("k" + std::to_string(i % 7), "v");
        b.erase("k" + std::to_string(i % 7));
    }
    CHECK(b.empty() && b.validate());
    std::printf("ok lifecycle\n");
}

int main() {
    test_sequential();
    test_random(1);
    test_random(42);
    test_random(2026);
    test_bulk_and_range();
    test_lifecycle();
    if (failures) {
        std::printf("BTREE TEST: %d fallos\n", failures);
        return 1;
    }
    std::printf("BTREE TEST: todo OK\n");
    return 0;
}
