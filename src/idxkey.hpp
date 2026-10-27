// GinaDB Lite 0.7 — idxkey.hpp: codificación canónica ORDENABLE de valores
// para los índices secundarios (B+Tree de claves binarias).
//
// La codificación produce un byte-string cuyo orden lexicográfico coincide
// EXACTAMENTE con el orden lógico de Value::operator< (rango de tipos por
// tag: null < bool < int < float < text < bytes), lo que permite:
//   * búsqueda por igualdad:  find(encode(v))
//   * búsqueda por rango:     visit_range(encode(lo), enc_upper(hi))
//
// Reglas:
//   * null / array / object / vector NO se indexan (devuelven "" y el planner
//     degrada a recorrido; semántica SQL de NULL fuera de índice).
//   * int  → big-endian de (u64)i con bit de signo invertido (orden con signo).
//   * float→ big-endian de los bits IEEE754 con el truco estándar de orden
//             total (IEEE totalOrder): negativos invertidos, bit de signo
//             puesto al resto.
//   * text/bytes → contenido cruto (el byte de rango precede y separa tipos).
#pragma once
#include <cstring>
#include <string>

#include "gina/value.hpp"

namespace gina::idx {

// Devuelve "" si el valor no es indexable (null/array/object/vector).
[[nodiscard]] inline std::string encode(const Value& v) {
    std::string out;
    switch (v.type()) {
        case Type::Null: case Type::Array: case Type::Object: case Type::Vector:
            return out;  // no indexable
        case Type::Bool:
            out.push_back(static_cast<char>(1));
            out.push_back(v.as_bool() ? 1 : 0);
            return out;
        case Type::Int: {
            out.push_back(static_cast<char>(2));
            std::uint64_t u =
                static_cast<std::uint64_t>(v.as_int()) ^ 0x8000000000000000ull;
            for (int i = 7; i >= 0; i--)
                out.push_back(static_cast<char>((u >> (i * 8)) & 255));
            return out;
        }
        case Type::Float: {
            out.push_back(static_cast<char>(3));
            double d = v.as_double();
            std::uint64_t bits;
            std::memcpy(&bits, &d, 8);
            // IEEE totalOrder: para negativos invertir todo; para el resto
            // poner a 1 el bit de signo.
            bits = (bits & 0x8000000000000000ull)
                       ? ~bits
                       : (bits | 0x8000000000000000ull);
            for (int i = 7; i >= 0; i--)
                out.push_back(static_cast<char>((bits >> (i * 8)) & 255));
            return out;
        }
        case Type::Text:
            out.push_back(static_cast<char>(4));
            out += v.as_text();
            return out;
        case Type::Bytes:
            out.push_back(static_cast<char>(5));
            const Bytes& b = v.as_bytes();
            out.append(reinterpret_cast<const char*>(b.data()), b.size());
            return out;
    }
    return out;
}

// Cota superior EXCLUSIVA que cubre `enc` y todo lo menor o igual:
// enc + '\0' (sucesor lexicográfico exacto).
[[nodiscard]] inline std::string upper_inclusive(const std::string& enc) {
    return enc + std::string(1, '\0');
}

// Cota inferior inclusiva para ">" (excluye enc y todo lo menor):
// enc + '\0' (sucesor).
[[nodiscard]] inline std::string lower_exclusive(const std::string& enc) {
    return enc + std::string(1, '\0');
}

} // namespace gina::idx
