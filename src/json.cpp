// GinaDB Lite 0.7 — json.cpp (ver json.hpp)
#include "json.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <optional>

namespace gina::json {
namespace {

// ---- base64 ---------------------------------------------------------------
const char* B64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] std::string b64_encode(const Bytes& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::uint32_t buf = 0;
    int bits = 0;
    for (std::uint8_t c : in) {
        buf = (buf << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(B64[(buf >> bits) & 63]);
        }
    }
    if (bits > 0) out.push_back(B64[(buf << (6 - bits)) & 63]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

[[nodiscard]] int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

[[nodiscard]] Bytes b64_decode(const std::string& in) {
    Bytes out;
    out.reserve(in.size() / 4 * 3);
    std::uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const int v = b64_val(c);
        if (v < 0) throw Error(Error::Code::BadArgument, "bad base64");
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buf >> bits) & 255));
        }
    }
    return out;
}

// ---- writer ----------------------------------------------------------------
void escape(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void encode_into(std::string& out, const Value& v) {
    switch (v.type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.as_bool() ? "true" : "false"; break;
        case Type::Int: out += std::to_string(v.as_int()); break;
        case Type::Float: {
            const double d = v.as_double();
            if (std::isnan(d) || std::isinf(d)) {
                out += "null";  // JSON has no NaN/Inf
            } else {
                char b[40];
                std::snprintf(b, sizeof b, "%.17g", d);
                out += b;
            }
            break;
        }
        case Type::Text: escape(out, v.as_text()); break;
        case Type::Bytes:
            out += "{\"$bytes\":\"";
            out += b64_encode(v.as_bytes());
            out += "\"}";
            break;
        case Type::Vector: {
            out += "{\"$vec\":[";
            bool first = true;
            for (float f : v.as_vector()) {
                if (!first) out.push_back(',');
                first = false;
                char b[40];
                std::snprintf(b, sizeof b, "%.9g", static_cast<double>(f));
                out += b;
            }
            out += "]}";
            break;
        }
        case Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto& z : v.as_array()) {
                if (!first) out.push_back(',');
                first = false;
                encode_into(out, z);
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [k, z] : v.as_object()) {
                if (!first) out.push_back(',');
                first = false;
                escape(out, k);
                out.push_back(':');
                encode_into(out, z);
            }
            out.push_back('}');
            break;
        }
    }
}

// ---- parser ------------------------------------------------------------------
struct Parser {
    const char* p;
    const char* end;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
    }
    [[noreturn]] void fail(const char* why) {
        throw Error(Error::Code::BadArgument,
                    std::string("invalid JSON: ") + why);
    }
    char peek() {
        ws();
        if (p >= end) fail("unexpected end");
        return *p;
    }
    void expect(char c) {
        if (peek() != c) fail("unexpected character");
        p++;
    }
    bool lit(const char* s) {
        const auto n = std::strlen(s);
        if (static_cast<std::size_t>(end - p) >= n && std::memcmp(p, s, n) == 0) {
            p += n;
            return true;
        }
        return false;
    }

    [[nodiscard]] Value value() {
        const char c = peek();
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') return Value(string());
        if (c == 't') { if (lit("true")) return Value(true); fail("bad literal"); }
        if (c == 'f') { if (lit("false")) return Value(false); fail("bad literal"); }
        if (c == 'n') { if (lit("null")) return Value{}; fail("bad literal"); }
        return number();
    }

    [[nodiscard]] Value number() {
        const char* start = p;
        if (p < end && *p == '-') p++;
        bool is_float = false;
        while (p < end && (std::isdigit(static_cast<unsigned char>(*p)) ||
                           *p == '.' || *p == 'e' || *p == 'E' ||
                           *p == '+' || *p == '-')) {
            if (*p == '.' || *p == 'e' || *p == 'E') is_float = true;
            p++;
        }
        if (p == start) fail("bad number");
        const std::string s(start, p);
        if (!is_float) {
            try {
                std::size_t used = 0;
                const long long v = std::stoll(s, &used);
                if (used == s.size()) return Value(static_cast<std::int64_t>(v));
            } catch (...) {
            }
        }
        try {
            return Value(std::stod(s));
        } catch (...) {
            fail("bad number");
        }
    }

    [[nodiscard]] std::string string() {
        expect('"');
        std::string out;
        while (p < end && *p != '"') {
            if (static_cast<unsigned char>(*p) < 0x20) fail("control char");
            if (*p == '\\') {
                p++;
                if (p >= end) fail("bad escape");
                switch (*p) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        if (end - p < 5) fail("bad \\u");
                        unsigned cp = 0;
                        for (int i = 1; i <= 4; i++) {
                            const char h = p[i];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                            else fail("bad hex");
                        }
                        p += 4;
                        // surrogate pair
                        if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 7 &&
                            p[1] == '\\' && p[2] == 'u') {
                            unsigned lo = 0;
                            for (int i = 3; i <= 6; i++) {
                                const char h = p[i];
                                lo <<= 4;
                                if (h >= '0' && h <= '9') lo |= static_cast<unsigned>(h - '0');
                                else if (h >= 'a' && h <= 'f') lo |= static_cast<unsigned>(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') lo |= static_cast<unsigned>(h - 'A' + 10);
                            }
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                p += 6;
                            }
                        }
                        // utf-8 encode
                        if (cp < 0x80) {
                            out.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.push_back(static_cast<char>(0x80 | (cp & 63)));
                        } else if (cp < 0x10000) {
                            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
                            out.push_back(static_cast<char>(0x80 | (cp & 63)));
                        } else {
                            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
                            out.push_back(static_cast<char>(0x80 | (cp & 63)));
                        }
                        break;
                    }
                    default: fail("bad escape");
                }
                p++;
            } else {
                out.push_back(*p++);
            }
        }
        if (p >= end) fail("unterminated string");
        p++;  // closing quote
        return out;
    }

    [[nodiscard]] Value array() {
        expect('[');
        Array a;
        ws();
        if (peek() == ']') { p++; return Value(std::move(a)); }
        while (true) {
            a.push_back(value());
            if (peek() == ',') { p++; continue; }
            if (peek() == ']') { p++; break; }
            fail("expected , or ]");
        }
        return Value(std::move(a));
    }

    [[nodiscard]] Value object() {
        expect('{');
        Object o;
        ws();
        if (peek() == '}') { p++; return Value(std::move(o)); }
        while (true) {
            const std::string k = string();
            expect(':');
            o.emplace(k, value());
            if (peek() == ',') { p++; continue; }
            if (peek() == '}') { p++; break; }
            fail("expected , or }");
        }
        // decode tagged representations back into Bytes / Vector
        if (o.size() == 1) {
            if (auto it = o.find("$bytes"); it != o.end() && it->second.is_text())
                return Value(b64_decode(it->second.as_text()));
            if (auto it = o.find("$vec"); it != o.end() && it->second.is_array()) {
                Vec v;
                v.reserve(it->second.as_array().size());
                for (const auto& z : it->second.as_array()) v.push_back(
                    static_cast<float>(z.is_int()
                                           ? static_cast<double>(z.as_int())
                                           : z.as_double()));
                return Value(std::move(v));
            }
        }
        return Value(std::move(o));
    }
};

[[nodiscard]] Type type_from_name(const std::string& s) {
    if (s == "null") return Type::Null;
    if (s == "bool") return Type::Bool;
    if (s == "int") return Type::Int;
    if (s == "float") return Type::Float;
    if (s == "text") return Type::Text;
    if (s == "bytes") return Type::Bytes;
    if (s == "array") return Type::Array;
    if (s == "object") return Type::Object;
    if (s == "vector") return Type::Vector;
    throw Error(Error::Code::BadArgument, "unknown column type '" + s + "'");
}

} // namespace

// ---- public encode API --------------------------------------------------------
std::string encode(const Value& v) {
    std::string out;
    encode_into(out, v);
    return out;
}

std::string encode_row(const Row& r) {
    std::string out;
    encode_into(out, Value(r));
    return out;
}

std::string encode_rows(const std::vector<Row>& rows) {
    std::string out = "[";
    for (std::size_t i = 0; i < rows.size(); i++) {
        if (i) out.push_back(',');
        encode_into(out, Value(rows[i]));
    }
    out += "]";
    return out;
}

std::string encode_event(const Event& e) {
    std::string out = "{\"type\":\"";
    out += e.kind == Event::Kind::Put ? "put" : "del";
    out += "\",\"table\":";
    escape(out, e.table);
    out += ",\"key\":";
    escape(out, e.key);
    out += ",\"row\":";
    encode_into(out, Value(e.row));
    out += "}";
    return out;
}

std::string encode_changes(const ChangeList& cl) {
    std::string out = "{\"next_cursor\":";
    out += std::to_string(cl.next_cursor);
    out += ",\"truncated\":";
    out += cl.truncated ? "true" : "false";
    out += ",\"events\":[";
    for (std::size_t i = 0; i < cl.events.size(); i++) {
        if (i) out.push_back(',');
        out += "{\"seq\":";
        out += std::to_string(cl.events[i].seq);
        out += ",\"event\":";
        out += encode_event(cl.events[i].ev);
        out += "}";
    }
    out += "]}";
    return out;
}

std::string encode_schema(const TableDef& def) {
    std::string out = "{\"name\":";
    escape(out, def.name);
    out += ",\"primary_key\":";
    escape(out, def.primary_key);
    out += ",\"schemaless\":";
    out += def.schemaless ? "true" : "false";
    out += ",\"columns\":[";
    for (std::size_t i = 0; i < def.columns.size(); i++) {
        if (i) out.push_back(',');
        const auto& c = def.columns[i];
        out += "{\"name\":";
        escape(out, c.name);
        out += ",\"type\":\"";
        out += type_name(c.type);
        out += "\",\"nullable\":";
        out += c.nullable ? "true" : "false";
        out += ",\"fts\":";
        out += c.fts ? "true" : "false";
        out += "}";
    }
    out += "],\"indexes\":[";  // 0.6: índices secundarios de la tabla
    for (std::size_t i = 0; i < def.indexes.size(); i++) {
        if (i) out.push_back(',');
        escape(out, def.indexes[i]);
    }
    out += "]}";
    return out;
}

std::string encode_stats(const Stats& s) {
    std::string out = "{";
    auto num = [&](const char* k, std::uint64_t v, bool last = false) {
        out += "\"";
        out += k;
        out += "\":";
        out += std::to_string(v);
        if (!last) out.push_back(',');
    };
    num("tables", s.tables);
    num("rows", s.rows);
    num("last_lsn", s.last_lsn);
    num("commits", s.commits);
    num("checkpoints", s.checkpoints);
    num("pages", s.pages);
    num("overflow_pages", s.overflow_pages);
    num("wal_bytes", s.wal_bytes);
    num("data_bytes", s.data_bytes);
    num("delta_bytes", s.delta_bytes);
    num("delta_pages", s.delta_pages);
    num("delta_live_pages", s.delta_live_pages);
    num("delta_free_pages", s.delta_free_pages);
    num("delta_suspect_pages", s.delta_suspect_pages);
    num("delta_corrupt_pages", s.delta_corrupt_pages);
    num("delta_broken_ovf_records", s.delta_broken_ovf_records);
    num("compactions", s.compactions);
    num("recovered_bad_tail", s.recovered_bad_tail, true);
    out += "}";
    return out;
}

std::string encode_knn(const std::vector<std::pair<std::string, double>>& hits) {
    std::string out = "[";
    for (std::size_t i = 0; i < hits.size(); i++) {
        if (i) out.push_back(',');
        char b[64];
        std::snprintf(b, sizeof b, "%.9g", hits[i].second);
        out += "{\"key\":";
        escape(out, hits[i].first);
        out += ",\"score\":";
        out += b;
        out += "}";
    }
    out += "]";
    return out;
}

// ---- public decode API ----------------------------------------------------------
Value decode(const std::string& text) {
    Parser ps{text.data(), text.data() + text.size()};
    const Value v = ps.value();
    ps.ws();
    if (ps.p != ps.end) ps.fail("trailing characters");
    return v;
}

Row decode_row(const std::string& text) {
    const Value v = decode(text);
    if (!v.is_object())
        throw Error(Error::Code::BadArgument, "expected a JSON object");
    return v.as_object();
}

TableDef decode_schema(const std::string& text) {
    const Row r = decode_row(text);
    TableDef def;
    if (auto it = r.find("name"); it != r.end() && it->second.is_text())
        def.name = it->second.as_text();
    if (auto it = r.find("primary_key"); it != r.end() && it->second.is_text())
        def.primary_key = it->second.as_text();
    if (auto it = r.find("schemaless"); it != r.end())
        def.schemaless = it->second.as_bool();
    if (auto it = r.find("columns"); it != r.end() && it->second.is_array()) {
        for (const auto& cv : it->second.as_array()) {
            if (!cv.is_object()) continue;
            const auto& co = cv.as_object();
            Column c;
            if (auto n = co.find("name"); n != co.end() && n->second.is_text())
                c.name = n->second.as_text();
            if (auto t = co.find("type"); t != co.end() && t->second.is_text())
                c.type = type_from_name(t->second.as_text());
            if (auto n = co.find("nullable"); n != co.end())
                c.nullable = n->second.as_bool();
            if (auto f = co.find("fts"); f != co.end()) c.fts = f->second.as_bool();
            if (!c.name.empty()) def.columns.push_back(std::move(c));
        }
    }
    // 0.6: "indexes": ["email", "age", ...] (o "indexed": true en columnas)
    if (auto it = r.find("indexes"); it != r.end() && it->second.is_array()) {
        for (const auto& iv : it->second.as_array())
            if (iv.is_text() && !iv.as_text().empty())
                def.indexes.push_back(iv.as_text());
    }
    for (auto& c : def.columns) {
        // compatibilidad: columna con "indexed": true
        (void)c;
    }
    return def;
}

// ---- select (0.6): consulta JSON → Cond + order + limit + fields ------------
SelectQuery decode_select(const std::string& text) {
    const Row r = decode_row(text);
    SelectQuery q;
    q.limit = static_cast<std::size_t>(-1);
    std::function<Cond(const Row&)> cond_of = [&](const Row& o) -> Cond {
        auto op_it = o.find("op");
        if (op_it == o.end() || !op_it->second.is_text())
            throw Error(Error::Code::BadArgument,
                        "select: cada condición necesita \"op\"");
        const std::string op = op_it->second.as_text();
        auto col_of = [&]() -> std::string {
            auto c = o.find("col");
            if (c == o.end() || !c->second.is_text())
                throw Error(Error::Code::BadArgument,
                            "select: falta \"col\" en " + op);
            return c->second.as_text();
        };
        auto val_of = [&](const char* k) -> Value {
            auto v = o.find(k);
            if (v == o.end())
                throw Error(Error::Code::BadArgument,
                            std::string("select: falta \"") + k + "\" en " + op);
            return v->second;
        };
        Cond c;
        if (op == "and" || op == "or") {
            c.op = op == "and" ? Cond::Op::And : Cond::Op::Or;
            auto a = o.find("args");
            if (a == o.end() || !a->second.is_array())
                throw Error(Error::Code::BadArgument,
                            "select: and/or necesitan \"args\"[]");
            for (const auto& av : a->second.as_array()) {
                if (!av.is_object())
                    throw Error(Error::Code::BadArgument,
                                "select: args debe contener objetos");
                c.args.push_back(cond_of(av.as_object()));
            }
            return c;
        }
        c.col = col_of();
        if (op == "eq") { c.op = Cond::Op::Eq; c.val = val_of("val"); }
        else if (op == "ne") { c.op = Cond::Op::Ne; c.val = val_of("val"); }
        else if (op == "gt") { c.op = Cond::Op::Gt; c.val = val_of("val"); }
        else if (op == "ge") { c.op = Cond::Op::Ge; c.val = val_of("val"); }
        else if (op == "lt") { c.op = Cond::Op::Lt; c.val = val_of("val"); }
        else if (op == "le") { c.op = Cond::Op::Le; c.val = val_of("val"); }
        else if (op == "between") {
            c.op = Cond::Op::Between;
            c.val = val_of("lo");
            c.val2 = val_of("hi");
        } else if (op == "in") {
            c.op = Cond::Op::In;
            auto v = o.find("vals");
            if (v == o.end() || !v->second.is_array())
                throw Error(Error::Code::BadArgument,
                            "select: in necesita \"vals\"[]");
            for (const auto& x : v->second.as_array()) c.vals.push_back(x);
        } else {
            throw Error(Error::Code::BadArgument, "select: op desconocido " + op);
        }
        return c;
    };
    if (auto w = r.find("where"); w != r.end()) {
        if (!w->second.is_object())
            throw Error(Error::Code::BadArgument,
                        "select: \"where\" debe ser un objeto");
        q.where = cond_of(w->second.as_object());
    }
    if (auto od = r.find("order"); od != r.end() && od->second.is_array()) {
        for (const auto& ov : od->second.as_array()) {
            if (ov.is_text()) {
                q.order.emplace_back(ov.as_text(), false);
            } else if (ov.is_object()) {
                const auto& oo = ov.as_object();
                auto c = oo.find("col");
                if (c == oo.end() || !c->second.is_text())
                    throw Error(Error::Code::BadArgument,
                                "select: order necesita \"col\"");
                bool desc = false;
                if (auto d = oo.find("desc"); d != oo.end())
                    desc = d->second.as_bool();
                q.order.emplace_back(c->second.as_text(), desc);
            }
        }
    }
    if (auto li = r.find("limit"); li != r.end()) {
        if (li->second.is_int() && li->second.as_int() >= 0)
            q.limit = static_cast<std::size_t>(li->second.as_int());
    }
    if (auto fl = r.find("fields"); fl != r.end() && fl->second.is_array()) {
        for (const auto& fv : fl->second.as_array())
            if (fv.is_text()) q.fields.push_back(fv.as_text());
    }
    return q;
}

} // namespace gina::json
