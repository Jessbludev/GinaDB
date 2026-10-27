// GinaDB Lite 0.6 — json.hpp: puente JSON (C ABI, CLI, ejemplos, select).
// Round-trip rules:
//   Bytes  <-> {"$bytes":"<base64>"}
//   Vector <-> {"$vec":[f32,...]}
//   Int/Float/Bool/Text/Null/Array/Object <-> plain JSON
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gina/db.hpp"

namespace gina::json {

// Value -> JSON
[[nodiscard]] std::string encode(const Value& v);
[[nodiscard]] std::string encode_row(const Row& r);
[[nodiscard]] std::string encode_rows(const std::vector<Row>& rows);
[[nodiscard]] std::string encode_event(const Event& e);
[[nodiscard]] std::string encode_changes(const ChangeList& cl);
[[nodiscard]] std::string encode_schema(const TableDef& def);
[[nodiscard]] std::string encode_stats(const Stats& s);

// JSON -> Value (throws gina::Error(BadArgument) on invalid input)
[[nodiscard]] Value decode(const std::string& json);
[[nodiscard]] Row decode_row(const std::string& json);  // must be an object
[[nodiscard]] TableDef decode_schema(const std::string& json);

// knn result: [{"key":"...","score":0.123},...]
[[nodiscard]] std::string encode_knn(
    const std::vector<std::pair<std::string, double>>& hits);

// ---- select (0.6) ------------------------------------------------------------
// Consulta JSON: {"where":{...},"order":["age"|{"col":"age","desc":true}],
//                 "limit":10,"fields":["id","name"]}
// where: {"op":"eq|ne|gt|ge|lt|le|between|in","col":..,"val":..,"lo":..,"hi":..,"vals":[..]}
//        | {"op":"and|or","args":[...]}
struct SelectQuery {
    std::optional<Cond> where;
    std::vector<std::pair<std::string, bool>> order;
    std::size_t limit = static_cast<std::size_t>(-1);
    std::vector<std::string> fields;  // proyección (vacío = fila completa)
};
[[nodiscard]] SelectQuery decode_select(const std::string& json);

} // namespace gina::json
