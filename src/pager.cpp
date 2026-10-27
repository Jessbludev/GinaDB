// GinaDB Lite 0.7 — pager.cpp (see pager.hpp) — BASE compacto.
#include "pager.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>

#include <fcntl.h>
#include <unistd.h>

namespace gina::pager {
namespace {

using codec::PAGE_SIZE;
using codec::PAGE_MAGIC_DATA;
using codec::PAGE_MAGIC_OVF;
using codec::PAGE_MAGIC_HDR;

constexpr std::uint16_t PG_DATA = 0, PG_CATALOG = 2;
constexpr std::size_t DATA_HDR = 32;          // data area start
constexpr std::size_t INLINE_MAX = 4050;      // max inline record payload
constexpr std::size_t REC_HDR = 5;            // u32 table_id + u8 layout
constexpr std::size_t SLOT_SZ = 4;

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

struct DataPageHeader {
    std::uint32_t magic = 0, page_id = 0;
    std::uint64_t page_lsn = 0;
    std::uint16_t slot_count = 0, data_end = 0, slots_end = 0, flags = 0;
    std::uint32_t crc = 0;
};

[[nodiscard]] DataPageHeader parse_data_header(const std::uint8_t* p) {
    DataPageHeader h;
    h.magic = rd32_at(p, 0);
    h.page_id = rd32_at(p, 4);
    h.page_lsn = rd64_at(p, 8);
    h.slot_count = rd16_at(p, 16);
    h.data_end = rd16_at(p, 18);
    h.slots_end = rd16_at(p, 20);
    h.flags = rd16_at(p, 22);
    h.crc = rd32_at(p, 24);
    return h;
}

// ---------------------------------------------------------------------------
// Streams slotted pages to an ofstream. Physical file order == page id order:
// overflow pages are written as soon as they are full (their ids are assigned
// at write time), and a slotted page receives its id when it is finally
// flushed — after any overflow pages its records reference.
class PageWriter {
public:
    explicit PageWriter(std::ofstream& out) : out_(out) {}

    void set_page_lsn(std::uint64_t lsn) { page_lsn_ = lsn; }

    void open_page(std::uint16_t flags) {
        if (open_) finish_page();
        open_ = true;
        current_flags_ = flags;
        cur_.assign(PAGE_SIZE, 0);
        slot_count_ = data_end_ = slots_end_ = 0;
    }

    // Appends [table_id][layout][payload] to pages of `page_kind`
    // (PG_CATALOG or PG_DATA); payloads larger than INLINE_MAX spill into a
    // chain of overflow pages and are referenced indirectly.
    void add_record(std::uint32_t table_id,
                    const std::vector<std::uint8_t>& payload,
                    std::uint16_t page_kind) {
        if (payload.size() + REC_HDR <= INLINE_MAX) {
            write_inline(table_id, 0, payload.data(), payload.size(), page_kind);
            return;
        }
        const auto total = static_cast<std::uint64_t>(payload.size());
        std::uint64_t first_ovf = 0, prev = 0;
        std::size_t off = 0;
        while (off < payload.size()) {
            const auto chunk = static_cast<std::uint32_t>(
                std::min<std::size_t>(codec::OVF_PAYLOAD, payload.size() - off));
            const auto id = static_cast<std::uint64_t>(next_page_++);
            if (first_ovf == 0) first_ovf = id;
            if (prev != 0) patch_next_(prev, id);
            std::vector<std::uint8_t> pg(PAGE_SIZE, 0);
            wr32_at(pg.data(), 0, PAGE_MAGIC_OVF);
            wr32_at(pg.data(), 4, static_cast<std::uint32_t>(id));
            wr32_at(pg.data(), 8, chunk);
            wr64_at(pg.data(), 16, 0);  // next (patched when the chain grows)
            std::memcpy(pg.data() + 24, payload.data() + off, chunk);
            out_.write(reinterpret_cast<const char*>(pg.data()), PAGE_SIZE);
            ovf_pages_++;
            prev = id;
            off += chunk;
        }
        std::vector<std::uint8_t> desc(16);
        wr64_at(desc.data(), 0, total);
        wr64_at(desc.data(), 8, first_ovf);
        write_inline(table_id, 1, desc.data(), desc.size(), page_kind);
    }

    void flush_open() {
        if (open_) finish_page();
    }

    [[nodiscard]] std::uint64_t page_count() const { return next_page_; }
    [[nodiscard]] std::uint64_t ovf_pages() const { return ovf_pages_; }

private:
    [[nodiscard]] bool fits_(std::size_t n) const {
        // free = PAGE_SIZE - slots_end_ - (DATA_HDR + data_end_) - SLOT_SZ
        return DATA_HDR + data_end_ + n + slots_end_ + SLOT_SZ <= PAGE_SIZE;
    }

    void write_inline(std::uint32_t table_id, std::uint8_t layout,
                      const std::uint8_t* p, std::size_t n,
                      std::uint16_t page_kind) {
        if (!open_ || current_flags_ != page_kind || !fits_(REC_HDR + n))
            open_page(page_kind);
        std::uint8_t* rec = cur_.data() + DATA_HDR + data_end_;
        wr32_at(rec, 0, table_id);
        rec[4] = layout;
        std::memcpy(rec + REC_HDR, p, n);
        const auto off16 = static_cast<std::uint16_t>(data_end_);
        const auto len16 = static_cast<std::uint16_t>(REC_HDR + n);
        data_end_ += REC_HDR + n;
        wr16_at(cur_.data(), PAGE_SIZE - slots_end_ - SLOT_SZ, off16);
        wr16_at(cur_.data(), PAGE_SIZE - slots_end_ - SLOT_SZ + 2, len16);
        slots_end_ += SLOT_SZ;
        slot_count_++;
    }

    void patch_next_(std::uint64_t ovf_page, std::uint64_t next) {
        out_.flush();
        const auto off = static_cast<std::int64_t>(ovf_page * PAGE_SIZE + 16);
        out_.seekp(off, std::ios::beg);
        std::uint8_t b[8];
        wr64_at(b, 0, next);
        out_.write(reinterpret_cast<const char*>(b), 8);
        out_.flush();
        out_.seekp(0, std::ios::end);
    }

    void finish_page() {
        open_id_ = next_page_++;
        wr32_at(cur_.data(), 0, PAGE_MAGIC_DATA);
        wr32_at(cur_.data(), 4, static_cast<std::uint32_t>(open_id_));
        wr64_at(cur_.data(), 8, page_lsn_);
        wr16_at(cur_.data(), 16, slot_count_);
        wr16_at(cur_.data(), 18, data_end_);
        wr16_at(cur_.data(), 20, slots_end_);
        wr16_at(cur_.data(), 22, current_flags_);
        wr32_at(cur_.data(), 24, codec::crc32(cur_.data() + DATA_HDR,
                                              PAGE_SIZE - DATA_HDR));
        out_.write(reinterpret_cast<const char*>(cur_.data()), PAGE_SIZE);
        open_ = false;
    }

    std::ofstream& out_;
    std::vector<std::uint8_t> cur_;
    bool open_ = false;
    std::uint16_t current_flags_ = PG_DATA;
    std::uint64_t open_id_ = 0, next_page_ = 1 /* page 0 = header */,
                  ovf_pages_ = 0, page_lsn_ = 0;
    std::uint16_t slot_count_ = 0, data_end_ = 0, slots_end_ = 0;
};

void fsync_path(const std::filesystem::path& p, bool dir) {
    const int fd = ::open(p.c_str(),
                          dir ? (O_RDONLY | O_DIRECTORY | O_CLOEXEC)
                              : (O_RDONLY | O_CLOEXEC));
    if (fd >= 0) { ::fsync(fd); ::close(fd); }
}

// 0.7: lectura masiva — istreambuf_iterator copia byte a byte y costaba
// cientos de ms en bases de decenas de MB; esto es una sola read().
[[nodiscard]] std::vector<std::uint8_t> read_file(
    const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) throw Error(Error::Code::IoError, "cannot open " + p.string());
    const auto sz = f.tellg();
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
    f.seekg(0, std::ios::beg);
    if (!buf.empty()) f.read(reinterpret_cast<char*>(buf.data()), buf.size());
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
LoadResult load(const std::filesystem::path& file, const SchemaFn& on_schema,
                 const RowFn& on_row) {
    LoadResult res;
    res.buf = read_file(file);
    const std::vector<std::uint8_t>& buf = res.buf;
    if (buf.size() < PAGE_SIZE)
        throw Error(Error::Code::Corruption, "snapshot too small");

    const std::uint8_t* h = buf.data();
    const auto hmagic = rd32_at(h, 0);
    if ((hmagic != codec::PAGE_MAGIC_HDR && hmagic != codec::PAGE_MAGIC_HDR5) ||
        (h[4] != codec::FORMAT_VERSION && h[4] != 5))
        throw Error(Error::Code::Corruption, "bad snapshot header");
    res.checkpoint_lsn = rd64_at(h, 8);
    res.pages = rd64_at(h, 16);
    const auto table_count = rd32_at(h, 24);
    if (res.pages == 0 || buf.size() < res.pages * PAGE_SIZE)
        throw Error(Error::Code::Corruption, "snapshot truncated");

    // pass 1: catalog pages (flags=2) → table_id -> TableDef
    std::map<std::uint32_t, TableDef> catalog;
    for (std::uint64_t pid = 1; pid < res.pages; pid++) {
        const std::uint8_t* p = buf.data() + pid * PAGE_SIZE;
        const auto magic = rd32_at(p, 0);
        if (magic == PAGE_MAGIC_OVF) continue;
        if (magic != PAGE_MAGIC_DATA)
            throw Error(Error::Code::Corruption, "bad page magic in snapshot");
        const auto hd = parse_data_header(p);
        if (hd.flags != PG_CATALOG) continue;
        if (hd.crc != codec::crc32(p + DATA_HDR, PAGE_SIZE - DATA_HDR))
            throw Error(Error::Code::Corruption, "catalog page crc mismatch");
        for (std::uint16_t s = 0; s < hd.slot_count; s++) {
            const std::size_t so = PAGE_SIZE - (s + 1) * SLOT_SZ;
            const auto off = static_cast<std::size_t>(rd16_at(p, so));
            const auto len = static_cast<std::size_t>(rd16_at(p, so + 2));
            if (DATA_HDR + off + len > PAGE_SIZE || len < REC_HDR)
                throw Error(Error::Code::Corruption, "catalog slot out of range");
            const std::uint8_t* rec = p + DATA_HDR + off;
            const auto tid = rd32_at(rec, 0);
            std::size_t pos = REC_HDR;
            const std::size_t end = len;
            TableDef def = codec::decode_schema(rec, end, pos, false);
            catalog.emplace(tid, std::move(def));
        }
    }
    if (catalog.size() != table_count)
        throw Error(Error::Code::Corruption, "catalog count mismatch");
    // schemas are complete before any row is delivered (loader relies on it)
    for (auto& [tid, def] : catalog) {
        res.schemas.push_back(def);
        if (on_schema) on_schema(def, tid);
    }

    // overflow chain reader (los cuerpos montados viven en res.spilled)
    auto read_overflow = [&](std::uint64_t first, std::uint64_t total,
                             std::vector<std::uint8_t>& out) {
        out.reserve(std::min<std::size_t>(total, 1u << 30));
        std::uint64_t page = first, hops = 0;
        while (page != 0) {
            if (page >= res.pages || ++hops > res.pages)
                throw Error(Error::Code::Corruption, "overflow chain broken");
            const std::uint8_t* p = buf.data() + page * PAGE_SIZE;
            if (rd32_at(p, 0) != PAGE_MAGIC_OVF)
                throw Error(Error::Code::Corruption, "bad overflow magic");
            const auto chunk = rd32_at(p, 8);
            if (24 + chunk > PAGE_SIZE)
                throw Error(Error::Code::Corruption, "overflow chunk too big");
            out.insert(out.end(), p + 24, p + 24 + chunk);
            page = rd64_at(p, 16);
        }
        if (out.size() != total)
            throw Error(Error::Code::Corruption, "overflow length mismatch");
    };

    // pass 2: data pages (flags=0) → registros crudos (+ count overflow pages)
    res.rows.reserve(static_cast<std::size_t>(
        (res.pages > 2 ? res.pages - 2 : 0) * 8));  // estimación holgada
    for (std::uint64_t pid = 1; pid < res.pages; pid++) {
        const std::uint8_t* p = buf.data() + pid * PAGE_SIZE;
        if (rd32_at(p, 0) == PAGE_MAGIC_OVF) { res.overflow_pages++; continue; }
        const auto hd = parse_data_header(p);
        if (hd.flags != PG_DATA) continue;  // catalog page
        if (hd.crc != codec::crc32(p + DATA_HDR, PAGE_SIZE - DATA_HDR))
            throw Error(Error::Code::Corruption, "data page crc mismatch");
        for (std::uint16_t s = 0; s < hd.slot_count; s++) {
            const std::size_t so = PAGE_SIZE - (s + 1) * SLOT_SZ;
            const auto off = static_cast<std::size_t>(rd16_at(p, so));
            const auto len = static_cast<std::size_t>(rd16_at(p, so + 2));
            if (DATA_HDR + off + len > PAGE_SIZE || len < REC_HDR)
                throw Error(Error::Code::Corruption, "data slot out of range");
            const std::uint8_t* rec = p + DATA_HDR + off;
            const auto tid = rd32_at(rec, 0);
            const std::uint8_t layout = rec[4];
            auto it = catalog.find(tid);
            if (it == catalog.end())
                throw Error(Error::Code::Corruption, "record with unknown table id");
            RowView rv;
            rv.tid = tid;
            if (layout == 0) {
                rv.body = rec + REC_HDR;
                rv.body_len = len - REC_HDR;
            } else if (layout == 1) {
                if (len != REC_HDR + 16)
                    throw Error(Error::Code::Corruption, "bad indirect record");
                const auto total = rd64_at(rec, REC_HDR);
                const auto first = rd64_at(rec, REC_HDR + 8);
                res.spilled.emplace_back();
                auto& body = res.spilled.back();
                read_overflow(first, total, body);
                rv.body = body.data();
                rv.body_len = body.size();
                rv.spilled = &body;
            } else {
                throw Error(Error::Code::Corruption, "bad record layout");
            }
            res.rows.push_back(rv);
            if (on_row) on_row(it->second, tid, rv);
        }
    }

    return res;
}

// ---------------------------------------------------------------------------
WriteStats save(const std::filesystem::path& file, std::uint64_t checkpoint_lsn,
                const std::vector<TableDef>& schemas,
                const std::function<void(const YieldSRow&)>& sorted_rows) {
    const auto tmp = file.string() + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) throw Error(Error::Code::IoError, "cannot write snapshot tmp");
    WriteStats st;

    {
        PageWriter pw(out);
        // page 0 placeholder (rewritten at the end)
        const std::vector<std::uint8_t> h0(PAGE_SIZE, 0);
        out.write(reinterpret_cast<const char*>(h0.data()), PAGE_SIZE);

        std::vector<TableDef> sorted = schemas;
        std::sort(sorted.begin(), sorted.end(),
                  [](const TableDef& a, const TableDef& b) { return a.name < b.name; });
        std::map<std::string, std::uint32_t> ids;
        for (std::uint32_t i = 0; i < sorted.size(); i++) ids[sorted[i].name] = i + 1;

        pw.set_page_lsn(checkpoint_lsn);
        pw.open_page(PG_CATALOG);
        for (const auto& def : sorted) {
            std::vector<std::uint8_t> rec;
            codec::encode_schema(rec, def);
            pw.add_record(ids[def.name], rec, PG_CATALOG);
        }
        pw.flush_open();

        pw.open_page(PG_DATA);
        sorted_rows([&](const TableDef& def, const SRow& row) {
            auto it = ids.find(def.name);
            if (it == ids.end()) return;  // dropped concurrently — skip
            std::vector<std::uint8_t> rec;
            codec::encode_srow(rec, def, row);
            pw.add_record(it->second, rec, PG_DATA);
        });
        pw.flush_open();

        // header page 0
        std::vector<std::uint8_t> h(PAGE_SIZE, 0);
        wr32_at(h.data(), 0, PAGE_MAGIC_HDR);
        h[4] = codec::FORMAT_VERSION;
        h[5] = 0;
        wr64_at(h.data(), 8, checkpoint_lsn);
        wr64_at(h.data(), 16, pw.page_count());
        wr32_at(h.data(), 24, static_cast<std::uint32_t>(sorted.size()));
        wr64_at(h.data(), 32, 0);  // free-list head: none (packed snapshot)
        wr64_at(h.data(), 40, 0);  // 0.6: base_lsn (informativo)
        out.seekp(0, std::ios::beg);
        out.write(reinterpret_cast<const char*>(h.data()), PAGE_SIZE);
        out.flush();
        st.pages = pw.page_count();
        st.overflow_pages = pw.ovf_pages();
    }

    out.close();
    fsync_path(tmp, false);
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) throw Error(Error::Code::IoError, "snapshot rename failed: " +
                                                 ec.message());
    fsync_path(file.parent_path(), true);
    std::error_code sz_ec;
    const auto bytes = std::filesystem::file_size(file, sz_ec);
    st.bytes = sz_ec ? 0 : static_cast<std::uint64_t>(bytes);
    return st;
}

} // namespace gina::pager
