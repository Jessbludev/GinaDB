// GinaDB Lite 0.7 — value.hpp
// Multimodal value model: null / bool / int / float / text / bytes / array /
// object / vector(embedding). Header-only, C++20, zero dependencies.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace gina {

using Bytes = std::vector<std::uint8_t>;
using Vec = std::vector<float>;      // dense embedding (f32)
struct Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;
using Row = Object;                  // a row is a flat string-keyed object

// Logical types (persisted as u8 tags, see docs/FORMATO_ARCHIVOS.md)
enum class Type : std::uint8_t {
    Null = 0, Bool = 1, Int = 2, Float = 3, Text = 4,
    Bytes = 5, Array = 6, Object = 7, Vector = 8,
};

[[nodiscard]] constexpr const char* type_name(Type t) noexcept {
    switch (t) {
        case Type::Null: return "null";   case Type::Bool: return "bool";
        case Type::Int: return "int";     case Type::Float: return "float";
        case Type::Text: return "text";   case Type::Bytes: return "bytes";
        case Type::Array: return "array"; case Type::Object: return "object";
        case Type::Vector: return "vector";
    }
    return "?";
}

class Error : public std::runtime_error {
public:
    enum class Code {
        NoSuchTable, TableExists, SchemaViolation, BadKey, NotFound,
        IoError, Corruption, LimitExceeded, TxDiscarded, BadArgument,
    };
    Error(Code c, const std::string& msg)
        : std::runtime_error(msg), code_(c) {}
    [[nodiscard]] Code code() const noexcept { return code_; }
private:
    Code code_;
};

struct Value {
    using Storage = std::variant<std::monostate, bool, std::int64_t, double,
                                 std::string, Bytes, Array, Object, Vec>;
    Storage data;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(const char* s) : data(std::string(s)) {}
    Value(std::string s) : data(std::move(s)) {}
    Value(bool b) : data(b) {}
    Value(std::int64_t v) : data(v) {}
    Value(int v) : data(static_cast<std::int64_t>(v)) {}
    Value(unsigned v) : data(static_cast<std::int64_t>(v)) {}
    Value(double v) : data(v) {}
    Value(float v) : data(static_cast<double>(v)) {}
    Value(Bytes b) : data(std::move(b)) {}
    Value(Vec v) : data(std::move(v)) {}
    Value(Array a) : data(std::move(a)) {}
    Value(Object o) : data(std::move(o)) {}

    [[nodiscard]] Type type() const noexcept {
        return static_cast<Type>(data.index());
    }
    [[nodiscard]] bool is_null()   const noexcept { return data.index() == 0; }
    [[nodiscard]] bool is_bool()   const noexcept { return data.index() == 1; }
    [[nodiscard]] bool is_int()    const noexcept { return data.index() == 2; }
    [[nodiscard]] bool is_float()  const noexcept { return data.index() == 3; }
    [[nodiscard]] bool is_text()   const noexcept { return data.index() == 4; }
    [[nodiscard]] bool is_bytes()  const noexcept { return data.index() == 5; }
    [[nodiscard]] bool is_array()  const noexcept { return data.index() == 6; }
    [[nodiscard]] bool is_object() const noexcept { return data.index() == 7; }
    [[nodiscard]] bool is_vector() const noexcept { return data.index() == 8; }

    // Checked accessors — type-safe reads (throw gina::Error on mismatch).
    [[nodiscard]] bool as_bool() const {
        if (!is_bool()) throw Error(Error::Code::BadKey, "value is not bool");
        return std::get<bool>(data);
    }
    [[nodiscard]] std::int64_t as_int() const {
        if (!is_int()) throw Error(Error::Code::BadKey, "value is not int");
        return std::get<std::int64_t>(data);
    }
    [[nodiscard]] double as_double() const {  // int or float
        if (is_int()) return static_cast<double>(std::get<std::int64_t>(data));
        if (is_float()) return std::get<double>(data);
        throw Error(Error::Code::BadKey, "value is not numeric");
    }
    [[nodiscard]] const std::string& as_text() const {
        if (!is_text()) throw Error(Error::Code::BadKey, "value is not text");
        return std::get<std::string>(data);
    }
    [[nodiscard]] const Bytes& as_bytes() const {
        if (!is_bytes()) throw Error(Error::Code::BadKey, "value is not bytes");
        return std::get<Bytes>(data);
    }
    [[nodiscard]] const Array& as_array() const {
        if (!is_array()) throw Error(Error::Code::BadKey, "value is not array");
        return std::get<Array>(data);
    }
    [[nodiscard]] const Object& as_object() const {
        if (!is_object()) throw Error(Error::Code::BadKey, "value is not object");
        return std::get<Object>(data);
    }
    [[nodiscard]] const Vec& as_vector() const {
        if (!is_vector()) throw Error(Error::Code::BadKey, "value is not vector");
        return std::get<Vec>(data);
    }

    // Ordered comparison used by order_by(); cross-type order: by Type tag.
    [[nodiscard]] bool operator<(const Value& o) const {
        const auto a = data.index(), b = o.data.index();
        if (a != b) return a < b;
        switch (static_cast<Type>(a)) {
            case Type::Null: return false;
            case Type::Bool: return as_bool() < o.as_bool();
            case Type::Int: return std::get<std::int64_t>(data) < std::get<std::int64_t>(o.data);
            case Type::Float: return as_double() < o.as_double();
            case Type::Text: return as_text() < o.as_text();
            case Type::Bytes: return as_bytes() < o.as_bytes();
            case Type::Vector: return as_vector() < o.as_vector();
            case Type::Array: case Type::Object: return false; // unordered
        }
        return false;
    }
    [[nodiscard]] bool operator==(const Value& o) const { return data == o.data; }
};

// Row helpers -------------------------------------------------------------
[[nodiscard]] inline const Value* row_get(const Row& r, const std::string& k) {
    auto it = r.find(k);
    return it == r.end() ? nullptr : &it->second;
}

} // namespace gina
