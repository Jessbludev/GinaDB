// GinaDB Lite 0.7 — srow.cpp: implementación de la fila plana interna.
#include "srow.hpp"

namespace gina {

std::string srow_keyof(const TableDef& def, const ColMap& cm, const SRow& r) {
    const Value* pk = nullptr;
    if (cm.pk_ord >= 0) {
        if (r.has(static_cast<std::uint32_t>(cm.pk_ord)))
            pk = &r.flat[static_cast<std::uint32_t>(cm.pk_ord)];
    } else {
        const auto it = r.obj.find(def.primary_key);
        if (it != r.obj.end()) pk = &it->second;
    }
    if (pk && pk->is_text() && !pk->as_text().empty()) return pk->as_text();
    if (!pk)
        throw Error(Error::Code::BadKey,
                    "row missing primary key '" + def.primary_key + "'");
    if (!pk->is_text())
        throw Error(Error::Code::BadKey,
                    "primary key '" + def.primary_key + "' must be text");
    throw Error(Error::Code::BadKey, "primary key must not be empty");
}

SRow srow_from_row(const TableDef& def, const ColMap& cm, Row&& r) {
    SRow out;
    out.resize(def);
    for (auto& [k, v] : r) {
        const auto ord = cm.find(k);
        if (ord >= 0) {
            const auto o = static_cast<std::uint32_t>(ord);
            out.flat[o] = std::move(v);
            SRow::set_bit(out.present, o, true);
        } else {
            out.obj.emplace(std::move(k), std::move(v));
        }
    }
    return out;
}

Row row_from_srow(const TableDef& def, const SRow& r) {
    Row out;
    for (std::uint32_t i = 0; i < def.columns.size() && i < r.flat.size();
         i++)
        if (r.has(i)) out.emplace(def.columns[i].name, r.flat[i]);
    for (const auto& [k, v] : r.obj) out.emplace(k, v);
    return out;
}

void srow_merge_patch(const TableDef& def, const ColMap& cm, SRow& base,
                      Row&& patch) {
    for (auto& [k, v] : patch) {
        const auto ord = cm.find(k);
        if (ord >= 0) {
            const auto o = static_cast<std::uint32_t>(ord);
            if (o >= base.flat.size()) base.resize(def);  // defensivo
            base.flat[o] = std::move(v);
            SRow::set_bit(base.present, o, true);
        } else {
            base.obj[k] = std::move(v);  // schemaless / desconocida
        }
    }
}

void srow_rewidth(const TableDef& od, const TableDef& nd, const ColMap& ncm,
                  SRow& r) {
    // vía rápida: mismas columnas en el mismo orden (re-schema de índices,
    // el único K_RESCH que emite el motor hoy) → nada que mover
    if (od.columns.size() == nd.columns.size()) {
        bool same = true;
        for (std::size_t i = 0; i < od.columns.size() && same; i++)
            same = od.columns[i].name == nd.columns[i].name;
        if (same) return;
    }
    // materializar todo por nombre (viejo) y redistribuir contra el nuevo
    Row loose = std::move(r.obj);
    for (std::uint32_t i = 0; i < od.columns.size() && i < r.flat.size();
         i++) {
        if (!r.has(i)) continue;
        auto it = loose.find(od.columns[i].name);
        if (it == loose.end())
            loose.emplace(od.columns[i].name, std::move(r.flat[i]));
        else
            it->second = std::move(r.flat[i]);  // no debería ocurrir: únicos
    }
    r.resize(nd);
    for (auto it = loose.begin(); it != loose.end();) {
        const auto ord = ncm.find(it->first);
        if (ord >= 0) {
            const auto o = static_cast<std::uint32_t>(ord);
            r.flat[o] = std::move(it->second);
            SRow::set_bit(r.present, o, true);
            it = loose.erase(it);
        } else {
            ++it;
        }
    }
    r.obj = std::move(loose);
}

} // namespace gina
