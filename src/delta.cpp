// GinaDB Lite 0.7 — delta.cpp: implementación del almacén incremental.
// Protocolo y formato: ver delta.hpp y docs/FORMATO_ARCHIVOS.md.
#include "delta.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <fcntl.h>
#include <unistd.h>

#include "codec.hpp"

namespace gina::delta {

// PK de una fila según el def de SU tabla; el motor pasa un resolver con los
// defs del base (los defs del propio delta se aplican antes, internamente).

namespace {

using codec::PAGE_SIZE;
constexpr std::size_t DATA_HDR = 32;
constexpr std::size_t SLOT_SZ = 4;
constexpr std::size_t REC_HDR = 5;      // [tid u32][layout u8]
constexpr std::size_t INLINE_MAX = 4050;
constexpr std::uint16_t PG_DELTA_FLAGS = 6;

[[nodiscard]] std::uint16_t rd16_at(const std::uint8_t* p, std::size_t o) {
    return static_cast<std::uint16_t>(p[o] | (p[o + 1] << 8));
}
[[nodiscard]] std::uint32_t rd32_at(const std::uint8_t* p, std::size_t o) {
    std::uint32_t x = 0;
    for (int i = 0; i < 4; i++) x |= static_cast<std::uint32_t>(p[o + i]) << (i * 8);
    return x;
}
[[nodiscard]] std::uint64_t rd64_at(const std::uint8_t* p, std::size_t o) {
    std::uint64_t x = 0;
    for (int i = 0; i < 8; i++) x |= static_cast<std::uint64_t>(p[o + i]) << (i * 8);
    return x;
}
void wr16_at(std::uint8_t* p, std::size_t o, std::uint16_t x) {
    p[o] = static_cast<std::uint8_t>(x & 255);
    p[o + 1] = static_cast<std::uint8_t>(x >> 8);
}
void wr32_at(std::uint8_t* p, std::size_t o, std::uint32_t x) {
    for (int i = 0; i < 4; i++) p[o + i] = static_cast<std::uint8_t>((x >> (i * 8)) & 255);
}
void wr64_at(std::uint8_t* p, std::size_t o, std::uint64_t x) {
    for (int i = 0; i < 8; i++) p[o + i] = static_cast<std::uint8_t>((x >> (i * 8)) & 255);
}

void write_all_at(int fd, const void* p, std::size_t n, std::uint64_t off) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    std::size_t done = 0;
    while (done < n) {
        const auto w = ::pwrite(fd, b + done, n - done,
                                static_cast<off_t>(off + done));
        if (w <= 0) throw Error(Error::Code::IoError, "delta pwrite failed");
        done += static_cast<std::size_t>(w);
    }
}

[[nodiscard]] std::uint64_t file_size_of(const std::filesystem::path& p) {
    std::error_code ec;
    const auto s = std::filesystem::file_size(p, ec);
    return ec ? 0 : static_cast<std::uint64_t>(s);
}

// ---------------------------------------------------------------------------
// pk-scan (0.7): recorre un frame de fila v6 [count u32]{ [len u32 name]
// [value] } sin decodificar los valores y devuelve el TEXT de la columna
// `pk` ("" si no está o no es text). Es un escaneo barato: comparación de
// nombres + salto de payloads. Espejo EXACTO del value-codec de codec.cpp.
// ---------------------------------------------------------------------------
namespace {

[[nodiscard]] bool scan_check(std::size_t pos, std::size_t need,
                              std::size_t n) noexcept {
    return pos + need <= n;
}

// salta el valor con tag en pos; devuelve false si el frame está truncado
[[nodiscard]] bool skip_value(const std::uint8_t* p, std::size_t n,
                              std::size_t& pos) noexcept {
    if (!scan_check(pos, 1, n)) return false;
    const auto tag = p[pos++];
    switch (tag) {
        case 0: return true;                                   // null
        case 1: if (!scan_check(pos, 1, n)) return false; pos += 1; return true;
        case 2: case 3: if (!scan_check(pos, 8, n)) return false; pos += 8; return true;
        case 4: case 5: {  // text / bytes: [len u32][bytes]
            if (!scan_check(pos, 4, n)) return false;
            std::uint32_t len = 0;
            for (int i = 0; i < 4; i++)
                len |= static_cast<std::uint32_t>(p[pos + i]) << (i * 8);
            pos += 4;
            if (!scan_check(pos, len, n)) return false;
            pos += len;
            return true;
        }
        case 6: {  // array: [len u32] values
            if (!scan_check(pos, 4, n)) return false;
            std::uint32_t len = 0;
            for (int i = 0; i < 4; i++)
                len |= static_cast<std::uint32_t>(p[pos + i]) << (i * 8);
            pos += 4;
            for (std::uint32_t i = 0; i < len; i++)
                if (!skip_value(p, n, pos)) return false;
            return true;
        }
        case 7: {  // object: [len u32]{ [str][value] }
            if (!scan_check(pos, 4, n)) return false;
            std::uint32_t len = 0;
            for (int i = 0; i < 4; i++)
                len |= static_cast<std::uint32_t>(p[pos + i]) << (i * 8);
            pos += 4;
            for (std::uint32_t i = 0; i < len; i++) {
                if (!scan_check(pos, 4, n)) return false;
                std::uint32_t sl = 0;
                for (int k = 0; k < 4; k++)
                    sl |= static_cast<std::uint32_t>(p[pos + k]) << (k * 8);
                pos += 4;
                if (!scan_check(pos, sl, n)) return false;
                pos += sl;
                if (!skip_value(p, n, pos)) return false;
            }
            return true;
        }
        case 8: {  // vector: [len u32][f32 × len]
            if (!scan_check(pos, 4, n)) return false;
            std::uint32_t len = 0;
            for (int i = 0; i < 4; i++)
                len |= static_cast<std::uint32_t>(p[pos + i]) << (i * 8);
            pos += 4;
            if (!scan_check(pos, static_cast<std::size_t>(len) * 4, n))
                return false;
            pos += static_cast<std::size_t>(len) * 4;
            return true;
        }
        default: return false;
    }
}

} // namespace

[[nodiscard]] std::string pk_scan(const std::uint8_t* p, std::size_t n,
                                  const std::string& pk) {
    std::size_t pos = 0;
    if (!scan_check(pos, 4, n)) return {};
    std::uint32_t count = 0;
    for (int i = 0; i < 4; i++)
        count |= static_cast<std::uint32_t>(p[pos + i]) << (i * 8);
    pos += 4;
    for (std::uint32_t i = 0; i < count; i++) {
        if (!scan_check(pos, 4, n)) return {};
        std::uint32_t sl = 0;
        for (int k = 0; k < 4; k++)
            sl |= static_cast<std::uint32_t>(p[pos + k]) << (k * 8);
        pos += 4;
        if (!scan_check(pos, sl, n)) return {};
        const bool is_pk = sl == pk.size() &&
                           std::memcmp(p + pos, pk.data(), sl) == 0;
        pos += sl;
        if (is_pk) {  // decodificar SOLO este valor si es text
            if (!scan_check(pos, 1, n)) return {};
            if (p[pos] != 4) return {};  // pk debe ser text
            pos++;
            if (!scan_check(pos, 4, n)) return {};
            std::uint32_t vl = 0;
            for (int k = 0; k < 4; k++)
                vl |= static_cast<std::uint32_t>(p[pos + k]) << (k * 8);
            pos += 4;
            if (!scan_check(pos, vl, n)) return {};
            return std::string(reinterpret_cast<const char*>(p + pos), vl);
        }
        if (!skip_value(p, n, pos)) return {};
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
struct Store::Impl {
    std::filesystem::path path;
    int fd = -1;
    bool active = false;
    std::uint64_t seq = 0, watermark = 0, base_lsn = 0, next_id = 2;
    std::uint64_t live_pages = 0, free_pages = 0, bytes = 0;
    std::uint64_t suspect_pages = 0;   // páginas ilegibles (torn de IC sin flip)
    std::uint64_t corrupt_pages = 0;   // 0.7: rotas DENTRO del extent comprometido (medio)
    std::uint64_t broken_ovf = 0;      // 0.7: cadenas overflow rotas (registro perdido)

    std::vector<Rec> records;          // registros en páginas vivas, orden lsn
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> page_recs;
    std::set<std::uint64_t> free_list;
    std::unordered_set<std::uint64_t> live_ovf;

    ~Impl() {
        if (fd >= 0) ::close(fd);
    }

    void rebuild_indexes() {
        page_recs.clear();
        live_ovf.clear();
        for (std::size_t i = 0; i < records.size(); i++) {
            page_recs[records[i].page].push_back(i);
            if (records[i].layout == L_OVF)
                for (auto pid : records[i].ovf_chain) live_ovf.insert(pid);
        }
        free_list.clear();
        for (std::uint64_t pid = 2; pid < next_id; pid++)
            if (!page_recs.count(pid) && !live_ovf.count(pid))
                free_list.insert(pid);
        live_pages = page_recs.size();
        free_pages = free_list.size();
    }

    [[nodiscard]] std::uint64_t alloc() {
        if (!free_list.empty()) {
            const std::uint64_t id = *free_list.begin();
            free_list.erase(free_list.begin());
            return id;
        }
        return next_id++;
    }

    void write_slot(std::uint64_t s, std::uint64_t wm, std::uint64_t base,
                    std::uint64_t pc, std::uint64_t lp, std::uint64_t fp) {
        std::vector<std::uint8_t> pg(PAGE_SIZE, 0);
        wr32_at(pg.data(), 0, HDR_MAGIC);
        pg[4] = HDR_VERSION;
        pg[5] = static_cast<std::uint8_t>(s & 1);
        wr64_at(pg.data(), 8, s);
        wr64_at(pg.data(), 16, wm);
        wr64_at(pg.data(), 24, base);
        wr64_at(pg.data(), 32, pc);
        wr64_at(pg.data(), 40, lp);
        wr64_at(pg.data(), 48, fp);
        wr32_at(pg.data(), 60, codec::crc32(pg.data(), 60));
        write_all_at(fd, pg.data(), PAGE_SIZE, (s & 1) * PAGE_SIZE);
    }

    void ensure_created(std::uint64_t base_lsn_hint) {
        if (fd < 0) {
            fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
            if (fd < 0)
                throw Error(Error::Code::IoError,
                            "cannot create delta " + path.string());
        }
        if (active) return;
        // Archivo sin estado comprometido: descartar restos y escribir AMBOS
        // slots seq=0 ANTES que cualquier página de datos. Así, "hay páginas
        // pero ningún slot válido" solo puede ser corrupción de medio.
        if (::ftruncate(fd, 0) != 0)
            throw Error(Error::Code::IoError, "delta ftruncate failed");
        write_slot(0, 0, base_lsn_hint, 2, 0, 0);
        write_slot(1, 0, base_lsn_hint, 2, 0, 0);
        if (::fsync(fd) != 0)
            throw Error(Error::Code::IoError, "delta fsync (create) failed");
        seq = 0;
        watermark = 0;
        base_lsn = base_lsn_hint;
        next_id = 2;
        bytes = 2 * PAGE_SIZE;
        active = true;
    }
};

namespace {

// Miembro emitido en el lote de un IC (identidad + página final + cadena).
struct MemberOut {
    std::uint32_t tid = 0;
    std::uint8_t kind = 0;         // 0 row/tomb, 1 schema, 2 drop
    std::string ident;
    std::uint8_t layout = 0;
    std::uint64_t page = 0;
    std::vector<std::uint64_t> ovf_chain;
};

// Páginas slotted en construcción del lote.
class PageOut {
public:
    PageOut(Store::Impl& st, std::uint64_t lsn) : st_(st), lsn_(lsn) {}

    void add(std::uint32_t tid, std::uint8_t layout,
             const std::vector<std::uint8_t>& payload, MemberOut m) {
        const std::size_t n = REC_HDR + payload.size();
        if (n + REC_HDR > PAGE_SIZE)
            throw Error(Error::Code::LimitExceeded, "registro delta imposible");
        if (open_ && !fits_(n)) close_page_();
        if (!open_) open_page_();
        std::uint8_t* rec = cur_.data() + DATA_HDR + data_end_;
        wr32_at(rec, 0, tid);
        rec[4] = layout;
        if (!payload.empty())
            std::memcpy(rec + REC_HDR, payload.data(), payload.size());
        wr16_at(cur_.data(), PAGE_SIZE - slots_end_ - SLOT_SZ,
                static_cast<std::uint16_t>(data_end_));
        wr16_at(cur_.data(), PAGE_SIZE - slots_end_ - SLOT_SZ + 2,
                static_cast<std::uint16_t>(n));
        slots_end_ += SLOT_SZ;
        data_end_ += n;
        slot_count_++;
        m.layout = layout;
        cur_members_.push_back(std::move(m));
    }

    void flush() {
        if (open_) close_page_();
    }

    // Emisión completa en ORDEN (lote del motor primero, copias después).
    std::vector<MemberOut> emitted;

private:
    [[nodiscard]] bool fits_(std::size_t n) const {
        return DATA_HDR + data_end_ + n + slots_end_ + SLOT_SZ <= PAGE_SIZE;
    }
    void open_page_() {
        cur_.assign(PAGE_SIZE, 0);
        slot_count_ = data_end_ = slots_end_ = 0;
        cur_members_.clear();
        open_ = true;
    }
    void close_page_() {
        const std::uint64_t id = st_.alloc();
        wr32_at(cur_.data(), 0, codec::PAGE_MAGIC_DATA);
        wr32_at(cur_.data(), 4, static_cast<std::uint32_t>(id));
        wr64_at(cur_.data(), 8, lsn_);
        wr16_at(cur_.data(), 16, slot_count_);
        wr16_at(cur_.data(), 18, data_end_);
        wr16_at(cur_.data(), 20, slots_end_);
        wr16_at(cur_.data(), 22, PG_DELTA_FLAGS);
        wr32_at(cur_.data(), 24,
                codec::crc32(cur_.data() + DATA_HDR, PAGE_SIZE - DATA_HDR));
        write_all_at(st_.fd, cur_.data(), PAGE_SIZE, id * PAGE_SIZE);
        for (auto& m : cur_members_) {
            m.page = id;
            emitted.push_back(std::move(m));
        }
        cur_members_.clear();
        open_ = false;
    }

    Store::Impl& st_;
    std::uint64_t lsn_;
    std::vector<std::uint8_t> cur_;
    bool open_ = false;
    std::uint16_t slot_count_ = 0, data_end_ = 0, slots_end_ = 0;
    std::vector<MemberOut> cur_members_;
};

} // namespace

// ---------------------------------------------------------------------------
Store Store::open(const std::filesystem::path& p, const DefResolver& defs_of) {
    Store s;
    std::error_code ec;
    const bool exists = std::filesystem::exists(p, ec) && file_size_of(p) > 0;
    if (!exists) {  // sin archivo: estado inactivo (se creará al primer IC)
        auto* im0 = new Impl();
        im0->path = p;
        s.impl_ = im0;
        return s;
    }

    std::ifstream fsz(p, std::ios::binary | std::ios::ate);
    const auto fsz_n = fsz.tellg();
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(fsz_n));
    fsz.seekg(0, std::ios::beg);
    if (!buf.empty())
        fsz.read(reinterpret_cast<char*>(buf.data()), buf.size());
    if (buf.size() < 2 * PAGE_SIZE) {  // slots incompletos → inactivo
        auto* im0 = new Impl();
        im0->path = p;
        s.impl_ = im0;
        return s;
    }

    auto slot_ok = [&](std::uint64_t page, std::uint64_t& seq,
                       std::uint64_t& wm, std::uint64_t& base,
                       std::uint64_t& pc) {
        const std::uint8_t* h = buf.data() + page * PAGE_SIZE;
        if (rd32_at(h, 0) != HDR_MAGIC || h[4] != HDR_VERSION) return false;
        if (rd32_at(h, 60) != codec::crc32(h, 60)) return false;
        seq = rd64_at(h, 8);
        wm = rd64_at(h, 16);
        base = rd64_at(h, 24);
        pc = rd64_at(h, 32);
        return true;
    };
    std::uint64_t sa = 0, wa = 0, ba = 0, pa = 0, sb = 0, wb = 0, bb = 0, pb = 0;
    const bool va = slot_ok(0, sa, wa, ba, pa);
    const bool vb = slot_ok(1, sb, wb, bb, pb);
    if (!va && !vb) {  // sin slot válido → nada comprometido
        auto* im0 = new Impl();
        im0->path = p;
        s.impl_ = im0;
        return s;
    }
    const bool use_b = vb && (!va || sb > sa);
    const std::uint64_t wm = use_b ? wb : wa;
    const std::uint64_t base = use_b ? bb : ba;
    const std::uint64_t pc = use_b ? pb : pa;
    const std::uint64_t seqv = use_b ? sb : sa;

    if (buf.size() < pc * PAGE_SIZE)
        throw Error(Error::Code::Corruption, "delta truncado respecto al header");
    if (buf.size() > pc * PAGE_SIZE) {  // cola de un IC sin flip
        std::filesystem::resize_file(p, pc * PAGE_SIZE, ec);
        buf.resize(static_cast<std::size_t>(pc * PAGE_SIZE));
    }

    // pase 1: páginas delta + overflow.
    // Clasificación 0.7 de páginas ilegibles dentro del extent comprometido
    // [2, pc):
    //   * page_lsn > watermark → torn de un IC sin flip (reutilización de un
    //     hueco libre o cola): NORMAL, se ignora sin alarma (suspect).
    //   * otra cosa (lsn ilegible o ≤ watermark) → la página formaba parte
    //     del estado comprometido y está ROTA: corrupción de medio con
    //     pérdida real de registros (corrupt) — se cuenta por separado para
    //     que la aplicación pueda detectarlo vía stats.
    struct OvfPage { std::uint32_t chunk; std::uint64_t next; const std::uint8_t* data; };
    std::unordered_map<std::uint64_t, OvfPage> ovf;
    struct RawRec { std::uint64_t lsn, page; std::uint32_t tid;
                    std::uint8_t layout; std::vector<std::uint8_t> payload; };
    std::vector<RawRec> raws;
    std::uint64_t suspect = 0, corrupt = 0;
    for (std::uint64_t pid = 2; pid < pc; pid++) {
        const std::uint8_t* pg = buf.data() + pid * PAGE_SIZE;
        if (rd32_at(pg, 0) == codec::PAGE_MAGIC_OVF) {
            OvfPage o{rd32_at(pg, 8), rd64_at(pg, 16), pg + 24};
            if (24 + o.chunk > PAGE_SIZE)
                throw Error(Error::Code::Corruption, "delta ovf chunk fuera de rango");
            ovf.emplace(pid, o);
            continue;
        }
        if (rd32_at(pg, 0) != codec::PAGE_MAGIC_DATA ||
            rd16_at(pg, 22) != PG_DELTA_FLAGS ||
            rd32_at(pg, 24) !=
                codec::crc32(pg + DATA_HDR, PAGE_SIZE - DATA_HDR)) {
            const std::uint64_t plsn = rd64_at(pg, 8);
            if (rd32_at(pg, 0) == codec::PAGE_MAGIC_DATA && plsn > wm) {
                // torn de un IC sin flip que reutilizó un hueco libre
                suspect++;
            } else {
                // dentro del extent comprometido: pérdida de registros
                corrupt++;
            }
            continue;
        }
        const std::uint64_t plsn = rd64_at(pg, 8);
        if (plsn > wm) continue;  // IC no comprometido
        const auto sc = rd16_at(pg, 16);
        for (std::uint16_t sl = 0; sl < sc; sl++) {
            const std::size_t so = PAGE_SIZE - (sl + 1) * SLOT_SZ;
            const auto off = static_cast<std::size_t>(rd16_at(pg, so));
            const auto len = static_cast<std::size_t>(rd16_at(pg, so + 2));
            if (DATA_HDR + off + len > PAGE_SIZE || len < REC_HDR)
                throw Error(Error::Code::Corruption, "delta slot fuera de rango");
            const std::uint8_t* rec = pg + DATA_HDR + off;
            RawRec r;
            r.lsn = plsn;
            r.page = pid;
            r.tid = rd32_at(rec, 0);
            r.layout = rec[4];
            r.payload.assign(rec + REC_HDR, rec + len);
            raws.push_back(std::move(r));
        }
    }
    std::stable_sort(raws.begin(), raws.end(),
                     [](const RawRec& a, const RawRec& b) { return a.lsn < b.lsn; });

    // pase 2: materializar SIN decodificar filas (0.7). Los defs del propio
    // delta primero (el pk es inmutable por encarnación); si la tabla vive en
    // el base, el DefResolver da el nombre del pk. La PK se obtiene con un
    // pk-scan del frame v6 (solo se decodifica el valor de la columna pk).
    std::unordered_map<std::uint32_t, std::string> tid_pk;
    for (const auto& r : raws)
        if (r.layout == L_SCHEMA) {
            std::size_t p0 = 0;
            TableDef d = codec::decode_schema(r.payload.data(), r.payload.size(),
                                              p0, false);
            tid_pk[r.tid] = d.primary_key;
        }
    std::vector<Rec> recs;
    recs.reserve(raws.size());
    std::uint64_t broken_ovf = 0;
    for (auto& r : raws) {
        Rec x;
        x.lsn = r.lsn;
        x.page = r.page;
        x.tid = r.tid;
        x.layout = r.layout;
        x.payload = std::move(r.payload);
        const std::uint8_t* p = x.payload.data();
        const std::size_t n = x.payload.size();
        std::size_t pos = 0;
        switch (r.layout) {
            case L_ROW:
            case L_OVF: {
                // 0.7: una cadena overflow rota/corrupta ya no tumba la base:
                // el registro se pierde, se cuenta y la apertura continúa.
                if (r.layout == L_OVF) {
                    if (n != 16) { ++broken_ovf; x.dead = true; x.layout = L_TOMB; }
                    else {
                        const auto total = rd64_at(p, 0);
                        const auto first = rd64_at(p, 8);
                        std::vector<std::uint8_t> body;
                        body.reserve(std::min<std::size_t>(
                            static_cast<std::size_t>(total), 1u << 30));
                        std::uint64_t cur = first, hops = 0;
                        bool ok = true;
                        while (cur != 0) {
                            auto it = ovf.find(cur);
                            if (it == ovf.end() || ++hops > pc) { ok = false; break; }
                            body.insert(body.end(), it->second.data,
                                        it->second.data + it->second.chunk);
                            x.ovf_chain.push_back(cur);
                            cur = it->second.next;
                        }
                        if (!ok || body.size() != total) {
                            ++broken_ovf;
                            x.dead = true;
                            x.layout = L_TOMB;  // neutro: no aporta estado
                            x.ovf_chain.clear();
                        } else {
                            x.payload = std::move(body);  // cadena ensamblada
                        }
                    }
                }
                if (!x.dead) {
                    // nombre del pk: def del delta propio o del base
                    std::string pk;
                    auto pk_it = tid_pk.find(r.tid);
                    if (pk_it != tid_pk.end()) pk = pk_it->second;
                    else if (defs_of) {
                        const TableDef* def = defs_of(r.tid);
                        if (def) pk = def->primary_key;
                    }
                    if (!pk.empty())
                        x.key = pk_scan(x.payload.data(), x.payload.size(), pk);
                    if (x.key.empty()) {  // sin pk válida: fila inservible
                        ++broken_ovf;
                        x.dead = true;
                        x.layout = L_TOMB;
                    }
                }
                break;
            }
            case L_TOMB:
                x.key = codec::get_str(p, n, pos);
                break;
            case L_SCHEMA: {
                std::size_t p0 = 0;
                x.def = codec::decode_schema(p, n, p0, false);
                x.name = x.def.name;
                break;
            }
            case L_DROP:
                x.name = codec::get_str(p, n, pos);
                break;
            default:
                throw Error(Error::Code::Corruption, "layout delta desconocido");
        }
        recs.push_back(std::move(x));
    }

    // pase 3: vida (latest por (tid,key) + estructurales por tid)
    std::map<std::pair<std::uint32_t, std::string>, std::size_t> latest;
    std::unordered_map<std::uint32_t, std::size_t> last_struct;
    for (std::size_t i = 0; i < recs.size(); i++) {
        const auto& r = recs[i];
        if (r.layout == L_ROW || r.layout == L_OVF || r.layout == L_TOMB)
            latest[{r.tid, r.key}] = i;
        else
            last_struct[r.tid] = i;
    }
    for (std::size_t i = 0; i < recs.size(); i++) {
        auto& r = recs[i];
        bool dead = false;
        if (r.layout == L_ROW || r.layout == L_OVF || r.layout == L_TOMB) {
            dead = r.key.empty() || latest[{r.tid, r.key}] != i;
            if (!dead) {  // un drop posterior de la misma encarnación la mata
                auto ls = last_struct.find(r.tid);
                if (ls != last_struct.end() &&
                    recs[ls->second].layout == L_DROP &&
                    recs[ls->second].lsn > r.lsn)
                    dead = true;
            }
        } else {
            dead = last_struct[r.tid] != i;
        }
        r.dead = dead;
    }
    std::unordered_map<std::uint64_t, bool> page_live;
    for (const auto& r : recs)
        if (!r.dead) page_live[r.page] = true;
    std::vector<Rec> kept;
    for (auto& r : recs)
        if (page_live[r.page]) kept.push_back(std::move(r));

    auto* im = new Impl();
    s.impl_ = im;
    im->path = p;
    im->active = true;
    im->seq = seqv;
    im->watermark = wm;
    im->base_lsn = base;
    im->next_id = pc;
    im->records = std::move(kept);
    im->suspect_pages = suspect;
    im->corrupt_pages = corrupt;
    im->broken_ovf = broken_ovf;
    im->bytes = pc * PAGE_SIZE;
    im->fd = ::open(p.c_str(), O_RDWR | O_CLOEXEC);
    if (im->fd < 0)
        throw Error(Error::Code::IoError, "cannot open delta " + p.string());
    im->rebuild_indexes();
    return s;
}

// ---------------------------------------------------------------------------
void Store::checkpoint(std::uint64_t new_watermark,
                       const std::vector<WriteRec>& batch,
                       std::uint64_t& freed_out, std::uint64_t& copied_out) {
    auto* im = impl_;
#ifdef GINA_TRACE
    const auto t0 = std::chrono::steady_clock::now();
    auto lap = [&](const char* what) {
        const auto t = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[DELTA] %-22s %7.1f ms\n", what,
            std::chrono::duration<double, std::milli>(t - t0).count());
    };
#endif
    if (!im) throw Error(Error::Code::BadArgument, "delta store no abierta");
    im->ensure_created(im->base_lsn);
    freed_out = copied_out = 0;

    // supersession por el lote: (tid,key) para filas/tombas; drops por tid
    std::unordered_map<std::uint32_t, std::unordered_set<std::string>> b_keys;
    std::unordered_set<std::uint32_t> b_struct, b_drops;
    for (const auto& w : batch) {
        if (w.layout == L_ROW || w.layout == L_TOMB) b_keys[w.tid].insert(w.key);
        else {
            b_struct.insert(w.tid);
            if (w.layout == L_DROP) b_drops.insert(w.tid);
        }
    }

    // decisión copy-forward por página viva
    std::vector<std::uint64_t> to_free;
    std::vector<std::size_t> to_copy;
    for (auto& [pid, idxs] : im->page_recs) {
        std::size_t alive = 0;
        std::vector<std::size_t> live_idx;
        for (std::size_t i : idxs) {
            Rec& r = im->records[i];
            if (!r.dead) {
                if (r.layout == L_ROW || r.layout == L_OVF ||
                    r.layout == L_TOMB) {
                    auto bk = b_keys.find(r.tid);
                    if ((bk != b_keys.end() && bk->second.count(r.key)) ||
                        b_drops.count(r.tid))
                        r.dead = true;  // sticky
                } else if (b_struct.count(r.tid)) {
                    r.dead = true;
                }
            }
            if (!r.dead) {
                alive++;
                live_idx.push_back(i);
            }
        }
        if (alive == 0) {
            to_free.push_back(pid);
        } else if (idxs.size() - alive >= alive) {  // ≥50% muertos → copiar
            for (std::size_t i : live_idx) to_copy.push_back(i);
            to_free.push_back(pid);
        }
    }

#ifdef GINA_TRACE
    lap("supersession/decisión");
#endif
    // contenido de los copiados (antes de tocar records)
    std::vector<Rec> copy_src;
    copy_src.reserve(to_copy.size());
    for (std::size_t i : to_copy) copy_src.push_back(im->records[i]);

    // ---- emitir: lote nuevo y luego copias -----------------------------------
    // 0.7: todo se emite como BYTES ya codificados (el lote llega codificado
    // por el motor bajo lock; las copias re-emiten su payload crudo tal cual
    // — sin decode+encode). Solo un registro que no cabe inline se re-trocea
    // en cadena overflow nueva. Las TOMBAS conservan su layout (payload =
    // la clave serializada).
    PageOut out(*im, new_watermark);
    auto emit_payload = [&](std::uint32_t tid, std::uint8_t layout,
                            const std::string& key,
                            const std::vector<std::uint8_t>& body) {
        if (layout == L_TOMB || REC_HDR + body.size() <= INLINE_MAX) {
            out.add(tid, layout, body,
                    MemberOut{tid, 0, key, layout, 0, {}});
            return;
        }
        std::vector<std::uint64_t> chain;
        std::uint64_t first = 0, prev = 0;
        std::size_t off = 0;
        while (off < body.size()) {
            const auto chunk = static_cast<std::uint32_t>(
                std::min<std::size_t>(codec::OVF_PAYLOAD, body.size() - off));
            const std::uint64_t id = im->alloc();
            if (first == 0) first = id;
            std::vector<std::uint8_t> pg(PAGE_SIZE, 0);
            wr32_at(pg.data(), 0, codec::PAGE_MAGIC_OVF);
            wr32_at(pg.data(), 4, static_cast<std::uint32_t>(id));
            wr32_at(pg.data(), 8, chunk);
            wr64_at(pg.data(), 16, 0);
            std::memcpy(pg.data() + 24, body.data() + off, chunk);
            write_all_at(im->fd, pg.data(), PAGE_SIZE, id * PAGE_SIZE);
            if (prev != 0) {
                std::uint8_t nb[8];
                wr64_at(nb, 0, id);
                write_all_at(im->fd, nb, 8, prev * PAGE_SIZE + 16);
            }
            chain.push_back(id);
            prev = id;
            off += chunk;
        }
        std::vector<std::uint8_t> desc(16);
        wr64_at(desc.data(), 0, body.size());
        wr64_at(desc.data(), 8, first);
        MemberOut m{tid, 0, key, L_OVF, 0, chain};
        out.add(tid, L_OVF, desc, std::move(m));
    };

    for (const auto& w : batch) {
        if (w.layout == L_ROW || w.layout == L_TOMB)
            emit_payload(w.tid, w.layout, w.key, w.payload);
        else if (w.layout == L_SCHEMA) {
            std::vector<std::uint8_t> body;
            codec::encode_schema(body, w.schema);
            out.add(w.tid, L_SCHEMA, body,
                    MemberOut{w.tid, 1, w.schema.name, L_SCHEMA, 0, {}});
        } else if (w.layout == L_DROP) {
            out.add(w.tid, L_DROP, w.payload,
                    MemberOut{w.tid, 2, w.name, L_DROP, 0, {}});
        }
    }
    for (const auto& r : copy_src) {
        switch (r.layout) {
            case L_ROW: case L_OVF: case L_TOMB:
                emit_payload(r.tid, r.layout == L_OVF ? L_ROW : r.layout,
                             r.key, r.payload);
                break;
            case L_SCHEMA: {
                std::vector<std::uint8_t> body;
                codec::encode_schema(body, r.def);
                out.add(r.tid, L_SCHEMA, body,
                        MemberOut{r.tid, 1, r.name, L_SCHEMA, 0, {}});
                break;
            }
            case L_DROP: {
                out.add(r.tid, L_DROP, r.payload,
                        MemberOut{r.tid, 2, r.name, L_DROP, 0, {}});
                break;
            }
            default: break;
        }
    }
    out.flush();
#ifdef GINA_TRACE
    lap("emisión+pwrite");
#endif

    // ---- commit: fsync + flip atómico -----------------------------------------
    if (::fsync(im->fd) != 0)
        throw Error(Error::Code::IoError, "delta fsync (pages) failed");
    const std::uint64_t new_seq = im->seq + 1;
    im->write_slot(new_seq, new_watermark, im->base_lsn, im->next_id, 0, 0);
    if (::fsync(im->fd) != 0)
        throw Error(Error::Code::IoError, "delta fsync (flip) failed");
#ifdef GINA_TRACE
    lap("fsync+flip");
#endif

    // ---- estado en memoria -------------------------------------------------------
    // la emisión va en orden: [batch...] + [copias...] — una entrada por add()
    if (out.emitted.size() != batch.size() + copy_src.size())
        throw Error(Error::Code::Corruption, "delta: emisión inconsistente");
    std::unordered_set<std::uint64_t> freed(to_free.begin(), to_free.end());
    std::vector<Rec> next;
    next.reserve(im->records.size());
    for (auto& r : im->records)
        if (!freed.count(r.page)) next.push_back(std::move(r));
    for (std::size_t i = 0; i < out.emitted.size(); i++) {
        const auto& m = out.emitted[i];
        Rec r;
        r.lsn = new_watermark;
        r.page = m.page;
        r.tid = m.tid;
        r.layout = m.layout;
        r.ovf_chain = m.ovf_chain;
        if (m.kind == 0) r.key = m.ident;
        else r.name = m.ident;
        if (i < batch.size()) {
            const auto& w = batch[i];
            if (m.kind == 0 && (w.layout == L_ROW || w.layout == L_TOMB))
                r.payload = w.payload;
            else if (m.kind == 1) r.def = w.schema;
            else if (m.kind == 2) r.payload = w.payload;
        } else {
            const auto& src = copy_src[i - batch.size()];
            if (m.kind == 0 &&
                (src.layout == L_ROW || src.layout == L_OVF ||
                 src.layout == L_TOMB))
                r.payload = src.payload;
            else if (m.kind == 1) r.def = src.def;
            else if (m.kind == 2) r.payload = src.payload;
        }
        next.push_back(std::move(r));
    }
    std::sort(next.begin(), next.end(),
              [](const Rec& a, const Rec& b) {
                  return a.lsn != b.lsn ? a.lsn < b.lsn : a.page < b.page;
              });
    im->records = std::move(next);
#ifdef GINA_TRACE
    lap("estado en memoria");
#endif
    im->seq = new_seq;
    im->watermark = new_watermark;
    im->bytes = im->next_id * PAGE_SIZE;
    im->rebuild_indexes();
    freed_out = to_free.size();
    copied_out = copy_src.size();
}

// ---------------------------------------------------------------------------
bool Store::active() const { return impl_ && impl_->active; }
std::uint64_t Store::watermark() const { return impl_ ? impl_->watermark : 0; }
std::uint64_t Store::live_pages() const { return impl_ ? impl_->live_pages : 0; }
std::uint64_t Store::free_pages() const { return impl_ ? impl_->free_pages : 0; }
std::uint64_t Store::page_count() const { return impl_ ? impl_->next_id : 2; }
std::uint64_t Store::file_bytes() const { return impl_ ? impl_->bytes : 0; }
std::uint64_t Store::suspect_pages() const {
    return impl_ ? impl_->suspect_pages : 0;
}
std::uint64_t Store::corrupt_pages() const {
    return impl_ ? impl_->corrupt_pages : 0;
}
std::uint64_t Store::broken_ovf_records() const {
    return impl_ ? impl_->broken_ovf : 0;
}
const std::vector<Rec>& Store::records() const {
    static const std::vector<Rec> empty;
    return impl_ ? impl_->records : empty;
}

Store::~Store() { delete impl_; }
Store::Store() noexcept = default;
Store::Store(Store&& o) noexcept : impl_(o.impl_) { o.impl_ = nullptr; }
Store& Store::operator=(Store&& o) noexcept {
    if (this != &o) {
        delete impl_;
        impl_ = o.impl_;
        o.impl_ = nullptr;
    }
    return *this;
}

void Store::reset(std::uint64_t watermark) {
    auto* im = impl_;
    if (!im) throw Error(Error::Code::BadArgument, "delta store no abierta");
    if (im->fd >= 0) {
        ::close(im->fd);
        im->fd = -1;
    }
    im->fd = ::open(im->path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC,
                    0644);
    if (im->fd < 0)
        throw Error(Error::Code::IoError, "cannot reset delta");
    const std::uint64_t s = im->seq + 1;
    im->write_slot(s, watermark, watermark, 2, 0, 0);
    im->write_slot(s, watermark, watermark, 2, 0, 0);
    if (::fsync(im->fd) != 0)
        throw Error(Error::Code::IoError, "delta fsync (reset) failed");
    im->seq = s;
    im->watermark = watermark;
    im->base_lsn = watermark;
    im->next_id = 2;
    im->records.clear();
    im->suspect_pages = 0;
    im->corrupt_pages = 0;
    im->broken_ovf = 0;
    im->bytes = 2 * PAGE_SIZE;
    im->active = true;
    im->rebuild_indexes();
}

void Store::drop_file() {
    auto* im = impl_;
    if (!im) return;
    if (im->fd >= 0) {
        ::close(im->fd);
        im->fd = -1;
    }
    std::error_code ec;
    std::filesystem::remove(im->path, ec);
    im->active = false;
    im->seq = im->watermark = im->base_lsn = 0;
    im->next_id = 2;
    im->records.clear();
    im->bytes = 0;
    im->live_pages = im->free_pages = 0;
    im->suspect_pages = im->corrupt_pages = im->broken_ovf = 0;
    im->page_recs.clear();
    im->free_list.clear();
    im->live_ovf.clear();
}

} // namespace gina::delta
