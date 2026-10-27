// GinaDB Lite 0.7 — srow.hpp: fila plana interna (NO es API pública).
//
// Problema 0.6 (informe): hidratar cada fila como std::map<string,Value>
// cuesta ~8,5 µs/fila — un nodo por columna con su string de clave — y
// dominaba la reapertura (850 ms @100K). Además el checkpoint copiaba el
// mapa completo de cada fila dirty al lote, y el planner consultaba las
// columnas por búsqueda en el mapa.
//
// Solución 0.7: SRow — representación interna PLANA por ordinal de columna:
//
//   * flat[i]     → valor de la columna i del schema (def.columns[i])
//   * present     → máscara de bits: columna presente vs ausente. Preserva
//                   la distinción "ausente ≠ null" que tenía el mapa
//                   (una fila puede omitir una columna nullable).
//   * obj         → pares nombre→valor que NO están en el schema: tablas
//                   schemaless (todo aquí) y columnas legado desconocidas
//                   que llegan de bases viejas. La semántica de consulta es
//                   "primero flat, luego obj", así que el caso mixto es
//                   siempre correcto.
//
// El formato EN DISCO no cambia (v6: pares nombre-valor por fila): SRow es
// solo la representación en memoria. encode_srow/decode_srow hacen el puente
// usando el schema (los nombres salen de def.columns, en orden ordinal, y el
// orden de pares en disco es irrelevante para todos los lectores).
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gina/db.hpp"  // TableDef, Row, Value

namespace gina {

// Mapa de columnas de un schema: nombre → ordinal. Se reconstruye cada vez
// que cambia el def (create / K_RESCH). pk_ord acelera keyof.
struct ColMap {
    std::unordered_map<std::string, std::uint32_t> by_name;
    std::int32_t pk_ord = -1;               // ordinal de la PK (-1: no está)

    [[nodiscard]] std::uint32_t width() const {
        return static_cast<std::uint32_t>(by_name.size());
    }
    // ordinal o -1 si la columna no pertenece al schema
    [[nodiscard]] std::int32_t find(const std::string& col) const {
        const auto it = by_name.find(col);
        return it == by_name.end() ? -1
                                   : static_cast<std::int32_t>(it->second);
    }
};

[[nodiscard]] inline ColMap cmap_build(const TableDef& def) {
    ColMap cm;
    cm.by_name.reserve(def.columns.size() * 2 + 1);
    for (std::uint32_t i = 0; i < def.columns.size(); i++) {
        cm.by_name.emplace(def.columns[i].name, i);
        if (def.columns[i].name == def.primary_key) cm.pk_ord = i;
    }
    return cm;
}

// ---------------------------------------------------------------------------
struct SRow {
    std::vector<Value> flat;        // [ordinal] = valor (si el bit está)
    std::vector<std::uint64_t> present;  // 1 bit por columna del schema
    Row obj;                        // fuera de schema (schemaless / legado)

    [[nodiscard]] static bool bit(const std::vector<std::uint64_t>& m,
                                  std::uint32_t i) noexcept {
        return (m[i >> 6] >> (i & 63)) & 1;
    }
    static void set_bit(std::vector<std::uint64_t>& m, std::uint32_t i,
                        bool on) noexcept {
        const std::uint32_t w = i >> 6, b = i & 63;
        if (on) m[w] |= 1ull << b;
        else m[w] &= ~(1ull << b);
    }

    // ajusta el ancho al schema (nuevo o tras re-schema); no mueve valores
    void resize(const TableDef& def) {
        const auto w = static_cast<std::uint32_t>(def.columns.size());
        flat.assign(w, Value{});
        present.assign((w + 63) / 64, 0);
    }
    [[nodiscard]] bool has(std::uint32_t ord) const noexcept {
        return ord < flat.size() * 64 && bit(present, ord);
    }
    void put(std::uint32_t ord, Value v) {
        flat[ord] = std::move(v);
        set_bit(present, ord, true);
    }
    void drop(std::uint32_t ord) noexcept {
        if (ord < flat.size()) {
            flat[ord] = Value{};
            set_bit(present, ord, false);
        }
    }
};

// ---- acceso ----------------------------------------------------------------

[[nodiscard]] inline const Value* srow_get(const ColMap& cm, const SRow& r,
                                           const std::string& col) {
    const auto ord = cm.find(col);
    if (ord >= 0) return r.has(static_cast<std::uint32_t>(ord)) ? &r.flat[ord]
                                                                : nullptr;
    const auto it = r.obj.find(col);
    return it == r.obj.end() ? nullptr : &it->second;
}
[[nodiscard]] inline Value* srow_get_mut(const ColMap& cm, SRow& r,
                                         const std::string& col) {
    const auto ord = cm.find(col);
    if (ord >= 0) return r.has(static_cast<std::uint32_t>(ord)) ? &r.flat[ord]
                                                                : nullptr;
    const auto it = r.obj.find(col);
    return it == r.obj.end() ? nullptr : &it->second;
}

// clave primaria (misma semántica y errores que keyof de db.cpp)
[[nodiscard]] std::string srow_keyof(const TableDef& def, const ColMap& cm,
                                     const SRow& r);

// ---- conversiones al borde de la API pública --------------------------------

// Row (mapa, ya validado) → SRow. Las columnas del schema van a flat por
// ordinal; el resto a obj (schemaless o desconocidas).
[[nodiscard]] SRow srow_from_row(const TableDef& def, const ColMap& cm,
                                 Row&& r);

// SRow → Row materializado (get/collect/eventos/JSON).
[[nodiscard]] Row row_from_srow(const TableDef& def, const SRow& r);

// aplica un patch (columnas del schema o arbitrarias) sobre base → out
void srow_merge_patch(const TableDef& def, const ColMap& cm, SRow& base,
                      Row&& patch);

// re-schema con columnas distintas: redistribuye flat/obj por nombre.
// Nunca descarta datos: lo que sale del schema va a obj. od = def VIEJO
// (necesario para nombrar los valores de flat antes de moverlos).
void srow_rewidth(const TableDef& od, const TableDef& nd, const ColMap& ncm,
                  SRow& r);

} // namespace gina
