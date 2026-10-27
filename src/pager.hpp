// GinaDB Lite 0.7 — 4 KiB slotted-page snapshot manager ("pager") — BASE.
//
// File layout (data.pages), all little-endian — see docs/FORMATO_ARCHIVOS.md:
//   page 0        : file header (magic "GNP6" v6 / "GNP5" v5 legible,
//                    checkpoint_lsn, page_count, ...)
//   catalog pages : slotted pages (flags=2) holding [u32 table_id | schema]
//   data pages    : slotted pages (flags=0) holding row records
//   overflow pages: chunk chain for records larger than one page (blobs)
//
// Data page (4096 B):
//   0  u32 magic "GPGN"     4 u32 page_id        8  u64 page_lsn
//   16 u16 slot_count       18 u16 data_end      20 u16 slots_end
//   22 u16 flags (0 data, 2 catalog)            24 u32 crc32(bytes[32..4096))
//   32 .. data area (grows up) .. slot area (grows down from 4096) .. 4096
// Slot entry (4 B at tail): u16 offset_from_32, u16 length.
// Record: [u32 table_id][u8 layout]  layout 0 = row bytes follow (inline)
//                                   layout 1 = [u64 total_len][u64 first_ovf]
// Overflow page (4096 B):
//   0 u32 magic "GNOV"  4 u32 page_id  8 u32 chunk_len  12 u32 reserved
//   16 u64 next_page (0 = end)   24 .. chunk data (max 4072 B)
//
// 0.7: load entrega los registros de fila como BYTES CRUDOS (on_row recibe
// def+tid+cuerpo) para que el motor decodifique a SRow con su schema — y
// opcionalmente en paralelo — sin reconstruir mapas. save recibe (def, SRow)
// y codifica sin materializar. El formato en disco no cambia.
#pragma once
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "codec.hpp"

namespace gina::pager {

using SchemaFn = std::function<void(const TableDef&, std::uint32_t table_id)>;

// Registro de fila sin decodificar: cuerpo = bytes del frame v6 de la fila.
// `spilled` != nullptr si el cuerpo vive en un buffer montado (overflow):
// el registro SOLO es válido mientras viva el LoadResult::spilled.
struct RowView {
    std::uint32_t tid = 0;
    const std::uint8_t* body = nullptr;
    std::size_t body_len = 0;
    const std::vector<std::uint8_t>* spilled = nullptr;
};
using RowFn = std::function<void(const TableDef& def, std::uint32_t tid,
                                 const RowView& rv)>;

struct LoadResult {
    std::uint64_t checkpoint_lsn = 0;
    std::uint64_t pages = 0, overflow_pages = 0;
    std::vector<TableDef> schemas;               // for stats/inspection
    // 0.7: buffer del archivo y registros en crudo para decode diferido
    // (posiblemente paralelo) por el motor. Los RowView de registros inline
    // apuntan a `buf`; los de overflow apuntan a buffers de `spilled`.
    std::vector<std::uint8_t> buf;
    std::deque<std::vector<std::uint8_t>> spilled;
    std::vector<RowView> rows;
};

// Loads a snapshot. Throws gina::Error(Corruption) on any inconsistency.
// on_schema is invoked for every table BEFORE any on_row. Los registros de
// fila se entregan vía result.rows (on_row puede ser nullptr).
[[nodiscard]] LoadResult load(const std::filesystem::path& file,
                              const SchemaFn& on_schema, const RowFn& on_row);

// Writes an atomic snapshot to `file` (via file.tmp + rename, fsync'd).
// `sorted_rows` must invoke yield(def, row) exactly once per live row, in
// (table name asc, primary key asc) order. Throws on I/O failure.
struct WriteStats { std::uint64_t pages = 0, overflow_pages = 0, bytes = 0; };
using YieldSRow = std::function<void(const TableDef&, const SRow&)>;
[[nodiscard]] WriteStats save(const std::filesystem::path& file,
                              std::uint64_t checkpoint_lsn,
                              const std::vector<TableDef>& schemas,
                              const std::function<void(const YieldSRow&)>&
                                  sorted_rows);

} // namespace gina::pager
