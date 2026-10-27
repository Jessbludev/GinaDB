// GinaDB Lite 0.7 — internal binary codec (WAL payloads + snapshot records).
// Byte-exact spec: docs/FORMATO_ARCHIVOS.md. The Zig reader (zig/ginaread.zig)
// mirrors this file — keep both in sync.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gina/db.hpp"
#include "srow.hpp"

namespace gina::codec {

// ---- constants ------------------------------------------------------------
inline constexpr std::uint32_t WAL_MAGIC = 0x414E4947u;  // "GINA" LE
inline constexpr std::uint8_t  FORMAT_VERSION = 6;
inline constexpr std::uint8_t  WAL_VERSION_V4 = 4;       // 0.4 migration
inline constexpr std::uint8_t  WAL_VERSION_V5 = 5;       // 0.5 migration
inline constexpr std::uint8_t K_PUT = 1, K_DEL = 2, K_COMMIT = 3,
                              K_SCHEMA = 4, K_DROP = 5, K_BASE = 6,
                              K_RESCH = 7;               // 0.6: re-schema (índices)
inline constexpr std::uint32_t PAGE_SIZE = 4096;
inline constexpr std::uint32_t PAGE_MAGIC_DATA = 0x47475047u;  // "GPGN"
inline constexpr std::uint32_t PAGE_MAGIC_OVF  = 0x564F4E47u;  // "GNOV"
inline constexpr std::uint32_t PAGE_MAGIC_HDR  = 0x36504E47u;  // "GNP6"
inline constexpr std::uint32_t PAGE_MAGIC_HDR5 = 0x35504E47u;  // "GNP5" (0.5)
inline constexpr std::uint32_t MAX_FRAME_BODY  = 64u << 20;    // 64 MiB
inline constexpr std::uint32_t OVF_PAYLOAD     = PAGE_SIZE - 24;

// ---- buffer primitives (little endian, bounds-checked reads) --------------
void put8(std::vector<std::uint8_t>& b, std::uint8_t x);
void put32(std::vector<std::uint8_t>& b, std::uint32_t x);
void put64(std::vector<std::uint8_t>& b, std::uint64_t x);
void put_str(std::vector<std::uint8_t>& b, const std::string& s);
[[nodiscard]] std::uint8_t  get8(const std::uint8_t* p, std::size_t n, std::size_t& pos);
[[nodiscard]] std::uint32_t get32(const std::uint8_t* p, std::size_t n, std::size_t& pos);
[[nodiscard]] std::uint64_t get64(const std::uint8_t* p, std::size_t n, std::size_t& pos);
[[nodiscard]] std::string   get_str(const std::uint8_t* p, std::size_t n, std::size_t& pos);
[[nodiscard]] std::uint32_t crc32(const std::uint8_t* p, std::size_t n);

// ---- value / row codec (type tags == gina::Type) ---------------------------
void encode_value(std::vector<std::uint8_t>& b, const Value& v);
void encode_row(std::vector<std::uint8_t>& b, const Row& r);
[[nodiscard]] Value decode_value(const std::uint8_t* p, std::size_t n, std::size_t& pos);
[[nodiscard]] Row   decode_row(const std::uint8_t* p, std::size_t n, std::size_t& pos);

// ---- srow codec (0.7) — MISMO formato de fila v6 (pares nombre-valor);
// los nombres de las columnas del schema salen de def (orden ordinal) y el
// resto del contenido de srow.obj. Decodificar por nombre hace el formato
// independiente de la versión del schema que lo escribió.
void encode_srow(std::vector<std::uint8_t>& b, const TableDef& def,
                 const SRow& r);
[[nodiscard]] SRow decode_srow(const TableDef& def, const ColMap& cm,
                               const std::uint8_t* p, std::size_t n,
                               std::size_t& pos);

// ---- WAL frame -------------------------------------------------------------
struct FrameView {
    std::uint8_t  version = 0, kind = 0;
    std::uint64_t lsn = 0, tx = 0;
    const std::uint8_t* payload = nullptr;  // points into frame body buffer
    std::size_t payload_len = 0;
};
// Encodes a full frame (magic|len|crc|body). Returns the bytes.
[[nodiscard]] std::vector<std::uint8_t> encode_frame(std::uint8_t kind,
    std::uint64_t lsn, std::uint64_t tx, const std::vector<std::uint8_t>& payload);
// Decodes a frame body (already length/crc verified). Throws Error(Corruption).
[[nodiscard]] FrameView decode_frame_body(const std::uint8_t* p, std::size_t n);

// ---- schema codec -----------------------------------------------------------
void encode_schema(std::vector<std::uint8_t>& b, const TableDef& def);
[[nodiscard]] TableDef decode_schema(const std::uint8_t* p, std::size_t n,
                                     std::size_t& pos, bool v4);

} // namespace gina::codec
