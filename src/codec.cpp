// GinaDB Lite 0.5 — codec.cpp (see codec.hpp)
#include "codec.hpp"

#include <cstring>

namespace gina::codec {

void put8(std::vector<std::uint8_t>& b, std::uint8_t x) { b.push_back(x); }

void put32(std::vector<std::uint8_t>& b, std::uint32_t x) {
    for (int i = 0; i < 4; i++) b.push_back(static_cast<std::uint8_t>((x >> (i * 8)) & 255));
}

void put64(std::vector<std::uint8_t>& b, std::uint64_t x) {
    for (int i = 0; i < 8; i++) b.push_back(static_cast<std::uint8_t>((x >> (i * 8)) & 255));
}

void put_str(std::vector<std::uint8_t>& b, const std::string& s) {
    put32(b, static_cast<std::uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

static void check(std::size_t pos, std::size_t need, std::size_t n) {
    if (pos + need > n) throw Error(Error::Code::Corruption, "truncated record");
}

std::uint8_t get8(const std::uint8_t* p, std::size_t n, std::size_t& pos) {
    check(pos, 1, n);
    return p[pos++];
}

std::uint32_t get32(const std::uint8_t* p, std::size_t n, std::size_t& pos) {
    check(pos, 4, n);
    std::uint32_t x = 0;
    for (int i = 0; i < 4; i++) x |= static_cast<std::uint32_t>(p[pos + i]) << (i * 8);
    pos += 4;
    return x;
}

std::uint64_t get64(const std::uint8_t* p, std::size_t n, std::size_t& pos) {
    check(pos, 8, n);
    std::uint64_t x = 0;
    for (int i = 0; i < 8; i++) x |= static_cast<std::uint64_t>(p[pos + i]) << (i * 8);
    pos += 8;
    return x;
}

std::string get_str(const std::uint8_t* p, std::size_t n, std::size_t& pos) {
    const auto len = get32(p, n, pos);
    check(pos, len, n);
    std::string s(reinterpret_cast<const char*>(p + pos), len);
    pos += len;
    return s;
}

// CRC32 (IEEE) slice-by-8: 0.7 — la versión bit a bit costaba ~390 ms al
// verificar el CRC de las páginas de un base de 28 MB en cada apertura (y lo
// mismo en WAL/delta). Esta procesa 8 bytes por iteración con una tabla de
// 8×256 entradas generada en compile-time; mismo polinomio, mismo resultado.
namespace {

struct CrcTables {
    std::uint32_t t[8][256];
    constexpr CrcTables() : t{} {
        for (std::uint32_t i = 0; i < 256; i++) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0u);
            t[0][i] = c;
        }
        for (std::uint32_t i = 0; i < 256; i++)
            for (int s = 1; s < 8; s++)
                t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    }
};
constexpr CrcTables CRC_T{};

} // namespace

std::uint32_t crc32(const std::uint8_t* p, std::size_t n) {
    std::uint32_t c = 0xFFFFFFFFu;
    while (n >= 8) {  // slice-by-8 (little-endian seguro: byte a byte)
        c ^= static_cast<std::uint32_t>(p[0]) |
             (static_cast<std::uint32_t>(p[1]) << 8) |
             (static_cast<std::uint32_t>(p[2]) << 16) |
             (static_cast<std::uint32_t>(p[3]) << 24);
        const std::uint32_t hi = static_cast<std::uint32_t>(p[4]) |
                                 (static_cast<std::uint32_t>(p[5]) << 8) |
                                 (static_cast<std::uint32_t>(p[6]) << 16) |
                                 (static_cast<std::uint32_t>(p[7]) << 24);
        c = CRC_T.t[7][c & 0xFF] ^ CRC_T.t[6][(c >> 8) & 0xFF] ^
            CRC_T.t[5][(c >> 16) & 0xFF] ^ CRC_T.t[4][(c >> 24) & 0xFF] ^
            CRC_T.t[3][hi & 0xFF] ^ CRC_T.t[2][(hi >> 8) & 0xFF] ^
            CRC_T.t[1][(hi >> 16) & 0xFF] ^ CRC_T.t[0][(hi >> 24) & 0xFF];
        p += 8;
        n -= 8;
    }
    for (std::size_t i = 0; i < n; i++)
        c = CRC_T.t[0][(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

// ---- value codec -----------------------------------------------------------

void encode_value(std::vector<std::uint8_t>& b, const Value& v) {
    const auto t = static_cast<std::uint8_t>(v.data.index());
    b.push_back(t);
    switch (static_cast<Type>(t)) {
        case Type::Null: break;
        case Type::Bool: b.push_back(std::get<bool>(v.data) ? 1 : 0); break;
        case Type::Int: put64(b, static_cast<std::uint64_t>(std::get<std::int64_t>(v.data))); break;
        case Type::Float: {
            double x = std::get<double>(v.data);
            std::uint64_t u;
            std::memcpy(&u, &x, 8);
            put64(b, u);
            break;
        }
        case Type::Text: put_str(b, std::get<std::string>(v.data)); break;
        case Type::Bytes: {
            const auto& x = std::get<Bytes>(v.data);
            put32(b, static_cast<std::uint32_t>(x.size()));
            b.insert(b.end(), x.begin(), x.end());
            break;
        }
        case Type::Array: {
            const auto& x = std::get<Array>(v.data);
            put32(b, static_cast<std::uint32_t>(x.size()));
            for (const auto& z : x) encode_value(b, z);
            break;
        }
        case Type::Object: {
            const auto& x = std::get<Object>(v.data);
            put32(b, static_cast<std::uint32_t>(x.size()));
            for (const auto& [k, z] : x) { put_str(b, k); encode_value(b, z); }
            break;
        }
        case Type::Vector: {
            const auto& x = std::get<Vec>(v.data);
            put32(b, static_cast<std::uint32_t>(x.size()));
            for (float f : x) {
                std::uint32_t u;
                std::memcpy(&u, &f, 4);
                put32(b, u);
            }
            break;
        }
    }
}

Value decode_value(const std::uint8_t* p, std::size_t n, std::size_t& pos) {
    const auto t = get8(p, n, pos);
    switch (static_cast<Type>(t)) {
        case Type::Null: return Value{};
        case Type::Bool: return Value(get8(p, n, pos) != 0);
        case Type::Int: return Value(static_cast<std::int64_t>(get64(p, n, pos)));
        case Type::Float: {
            auto u = get64(p, n, pos);
            double x;
            std::memcpy(&x, &u, 8);
            return Value(x);
        }
        case Type::Text: return Value(get_str(p, n, pos));
        case Type::Bytes: {
            auto len = get32(p, n, pos);
            check(pos, len, n);
            Bytes x(p + pos, p + pos + len);
            pos += len;
            return Value(std::move(x));
        }
        case Type::Array: {
            auto len = get32(p, n, pos);
            Array a;
            a.reserve(std::min<std::size_t>(len, 1 << 20));
            for (std::uint32_t i = 0; i < len; i++) a.push_back(decode_value(p, n, pos));
            return Value(std::move(a));
        }
        case Type::Object: {
            auto len = get32(p, n, pos);
            Object o;
            for (std::uint32_t i = 0; i < len; i++) {
                auto k = get_str(p, n, pos);
                o.emplace(std::move(k), decode_value(p, n, pos));
            }
            return Value(std::move(o));
        }
        case Type::Vector: {
            auto len = get32(p, n, pos);
            check(pos, static_cast<std::size_t>(len) * 4, n);
            Vec v(len);
            for (std::uint32_t i = 0; i < len; i++) {
                std::uint32_t u = 0;
                for (int k = 0; k < 4; k++) u |= static_cast<std::uint32_t>(p[pos + i * 4 + k]) << (k * 8);
                std::memcpy(&v[i], &u, 4);
            }
            pos += static_cast<std::size_t>(len) * 4;
            return Value(std::move(v));
        }
    }
    throw Error(Error::Code::Corruption, "bad value tag " + std::to_string(t));
}

void encode_row(std::vector<std::uint8_t>& b, const Row& r) {
    put32(b, static_cast<std::uint32_t>(r.size()));
    for (const auto& [k, v] : r) { put_str(b, k); encode_value(b, v); }
}

Row decode_row(const std::uint8_t* p, std::size_t n, std::size_t& pos) {
    const auto len = get32(p, n, pos);
    Row r;
    for (std::uint32_t i = 0; i < len; i++) {
        auto k = get_str(p, n, pos);
        r.emplace(std::move(k), decode_value(p, n, pos));
    }
    return r;
}

// ---- srow codec (0.7): mismo formato v6, nombres desde el schema -----------

void encode_srow(std::vector<std::uint8_t>& b, const TableDef& def,
                 const SRow& r) {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < def.columns.size() && i < r.flat.size();
         i++)
        if (r.has(i)) n++;
    n += static_cast<std::uint32_t>(r.obj.size());
    put32(b, n);
    for (std::uint32_t i = 0; i < def.columns.size() && i < r.flat.size();
         i++)
        if (r.has(i)) {
            put_str(b, def.columns[i].name);
            encode_value(b, r.flat[i]);
        }
    for (const auto& [k, v] : r.obj) {
        put_str(b, k);
        encode_value(b, v);
    }
}

SRow decode_srow(const TableDef& def, const ColMap& cm, const std::uint8_t* p,
                 std::size_t n, std::size_t& pos) {
    const auto len = get32(p, n, pos);
    SRow r;
    r.resize(def);
    for (std::uint32_t i = 0; i < len; i++) {
        auto k = get_str(p, n, pos);
        const auto ord = cm.find(k);
        if (ord >= 0) {
            const auto o = static_cast<std::uint32_t>(ord);
            r.flat[o] = decode_value(p, n, pos);
            SRow::set_bit(r.present, o, true);
        } else {
            r.obj.emplace(std::move(k), decode_value(p, n, pos));
        }
    }
    return r;
}

// ---- WAL frame -------------------------------------------------------------

std::vector<std::uint8_t> encode_frame(std::uint8_t kind, std::uint64_t lsn,
                                       std::uint64_t tx,
                                       const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> body;
    body.reserve(payload.size() + 18);
    put8(body, FORMAT_VERSION);
    put8(body, kind);
    put64(body, lsn);
    put64(body, tx);
    body.insert(body.end(), payload.begin(), payload.end());

    std::vector<std::uint8_t> frame;
    frame.reserve(body.size() + 12);
    put32(frame, WAL_MAGIC);
    put32(frame, static_cast<std::uint32_t>(body.size()));
    put32(frame, crc32(body.data(), body.size()));
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

FrameView decode_frame_body(const std::uint8_t* p, std::size_t n) {
    if (n < 18) throw Error(Error::Code::Corruption, "short frame body");
    FrameView f;
    f.version = p[0];
    f.kind = p[1];
    std::size_t pos = 2;
    f.lsn = get64(p, n, pos);
    f.tx = get64(p, n, pos);
    if (f.version != FORMAT_VERSION && f.version != WAL_VERSION_V4 &&
        f.version != WAL_VERSION_V5)
        throw Error(Error::Code::Corruption, "unsupported WAL version " +
                                               std::to_string(f.version));
    f.payload = p + pos;
    f.payload_len = n - pos;
    return f;
}

// ---- schema codec ------------------------------------------------------------

void encode_schema(std::vector<std::uint8_t>& b, const TableDef& def) {
    put_str(b, def.name);
    put_str(b, def.primary_key);
    put8(b, def.schemaless ? 1 : 0);
    put32(b, static_cast<std::uint32_t>(def.columns.size()));
    for (const auto& c : def.columns) {
        put_str(b, c.name);
        put8(b, static_cast<std::uint8_t>(c.type));
        put8(b, c.nullable ? 1 : 0);
        put8(b, c.fts ? 1 : 0);
    }
    // 0.6: sección de índices secundarios (trailing — ausente en payloads v5,
    // lo que permite abrir WAL/snapshots de 0.5 sin migración)
    put8(b, 1);
    put32(b, static_cast<std::uint32_t>(def.indexes.size()));
    for (const auto& ix : def.indexes) put_str(b, ix);
}

TableDef decode_schema(const std::uint8_t* p, std::size_t n, std::size_t& pos, bool v4) {
    TableDef d;
    d.name = get_str(p, n, pos);
    d.primary_key = get_str(p, n, pos);
    if (!v4) d.schemaless = get8(p, n, pos) != 0;
    const auto len = get32(p, n, pos);
    d.columns.reserve(std::min<std::size_t>(len, 4096));
    for (std::uint32_t i = 0; i < len; i++) {
        Column c;
        c.name = get_str(p, n, pos);
        c.type = static_cast<Type>(get8(p, n, pos));
        c.nullable = get8(p, n, pos) != 0;
        if (!v4) c.fts = get8(p, n, pos) != 0;
        if (static_cast<std::uint8_t>(c.type) > static_cast<std::uint8_t>(Type::Vector))
            throw Error(Error::Code::Corruption, "bad column type in schema");
        d.columns.push_back(std::move(c));
    }
    // 0.6: índices si el payload los trae (v5 se queda corto y no falla)
    if (!v4 && pos < n) {
        (void)get8(p, n, pos);  // marcador de sección (reservado)
        const auto nidx = get32(p, n, pos);
        for (std::uint32_t i = 0; i < nidx && pos < n; i++)
            d.indexes.push_back(get_str(p, n, pos));
    }
    return d;
}

} // namespace gina::codec
