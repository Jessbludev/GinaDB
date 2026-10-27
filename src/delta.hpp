// GinaDB Lite 0.7 — delta.hpp: almacén de páginas del checkpoint incremental.
//
// El motor 0.6 persiste en dos niveles:
//   * data.pages  — snapshot BASE compacto (reescripciones completas, FC).
//   * delta.pages — ESTE archivo: versiones de registros cambiados desde la
//                   base, en páginas slotted de 4 KiB con lsn de página.
//
// Protocolo de checkpoint incremental (IC):
//   1. El motor entrega el lote de versiones nuevas (filas dirty, tombstones,
//      schemas dirty).
//   2. La Store decide copy-forward por página: si ≥50% de sus miembros está
//      muerto (superado por el lote, o su tabla fue borrada), reemite los
//      vivos en el lote nuevo y libera la página; si está 100% muerta, solo
//      la libera.
//   3. Asigna páginas: primero reutiliza la FREE-LIST (huecos de páginas
//      liberadas), después extiende al final del archivo.
//   4. pwrite de páginas nuevas + fsync + "flip" atómico del encabezado de
//      doble slot (seq creciente): el flip ES el punto de commit.
//
// Crash-safety: páginas con page_lsn > watermark se ignoran al abrir (IC no
// comprometido); siempre queda un slot válido; nunca se sobrescribe el único
// slot válido. La free-list NO se persiste como estado redundante: se DERIVA
// del contenido (páginas sin registros vivos + cadenas overflow sin
// referencias). Es persistente porque sobrevive a reinicios y sus huecos se
// reutilizan, y es infalsificable porque no hay segundo estado que corromper.
//
// Formato (little endian) — ver docs/FORMATO_ARCHIVOS.md:
//   página 0/1  : slots de encabezado A/B (magic "GNDL", ver, slot, seq,
//                 watermark, base_lsn, page_count, live/free, crc32)
//   página ≥2   : slotted page flags=6  [tid u32][layout u8][payload]
//                 layout 0 fila inline | 1 fila overflow | 2 tombstone
//                        3 schema | 4 drop de tabla
//   overflow    : idéntico al base ("GNOV", cadena por enlaces next)
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "gina/db.hpp"
#include "srow.hpp"

namespace gina::delta {

inline constexpr std::uint32_t HDR_MAGIC = 0x4C444E47u;  // "GNDL" LE
inline constexpr std::uint8_t HDR_VERSION = 6;
inline constexpr std::uint16_t PG_DELTA = 6;             // flags de página
inline constexpr std::uint8_t L_ROW = 0, L_OVF = 1, L_TOMB = 2,
                              L_SCHEMA = 3, L_DROP = 4;

// Registro versionado tal como vive en el delta (materializado al abrir).
// 0.7: la fila NO se decodifica aquí — el registro guarda su PAYLOAD crudo
// (bytes v6, schema-agnóstico). Ventajas: el copy-forward del IC re-emite
// los mismos bytes sin decode+encode, la reapertura no paga decode por
// registros muertos (el motor decodifica solo los vivos, con su schema), y
// la capa delta queda como almacén versionado de bytes puro.
struct Rec {
    std::uint64_t lsn = 0;      // watermark del IC que lo escribió
    std::uint64_t page = 0;
    std::uint32_t tid = 0;
    std::uint8_t layout = 0;
    std::string key;            // L_ROW/L_OVF/L_TOMB (pk-scan al abrir)
    std::vector<std::uint8_t> payload;  // bytes crudos del registro (sin REC_HDR)
                                        // L_OVF: cadena ensamblada completa
    TableDef def;               // L_SCHEMA (decodificado)
    std::string name;           // L_SCHEMA/L_DROP
    std::vector<std::uint64_t> ovf_chain;  // páginas de su cadena (L_OVF)
    bool dead = false;          // runtime: supersedido (sticky)
};

// Resuelve el def vivo de una tabla del BASE por tid (para derivar la PK de
// las filas del delta con un pk-scan del frame). Solo disponible tras
// load_snapshot — los defs del propio delta se aplican antes, internamente.
using DefResolver = std::function<const TableDef*(std::uint32_t tid)>;

class Store {
public:
    // Abre (y deriva todo el estado) desde el archivo. No existe → estado
    // vacío inactivo. Sin slot válido → inactivo (nada comprometido). Con
    // páginas truncadas respecto al slot → Corruption.
    [[nodiscard]] static Store open(const std::filesystem::path& p,
                                    const DefResolver& defs_of = nullptr);

    [[nodiscard]] bool active() const;
    [[nodiscard]] std::uint64_t watermark() const;
    [[nodiscard]] std::uint64_t live_pages() const;
    [[nodiscard]] std::uint64_t free_pages() const;
    [[nodiscard]] std::uint64_t page_count() const;
    [[nodiscard]] std::uint64_t file_bytes() const;
    [[nodiscard]] std::uint64_t suspect_pages() const;
    // 0.7: páginas dentro del_extent comprometido_ con crc/magic rotos y
    // page_lsn ≤ watermark — corrupción de MEDIO con pérdida de registros
    // (se reporta; a diferencia de suspect, esto NO es un torn normal).
    [[nodiscard]] std::uint64_t corrupt_pages() const;
    // 0.7: registros L_OVF cuya cadena overflow estaba rota/corrupta — se
    // pierden esos registros pero la base abre y el hecho queda contado.
    [[nodiscard]] std::uint64_t broken_ovf_records() const;
    [[nodiscard]] const std::vector<Rec>& records() const;  // orden lsn

    // ---- escritura -----------------------------------------------------------
    struct WriteRec {
        std::uint8_t layout = 0;   // L_ROW (auto-overflow si no cabe), L_TOMB,
                                   // L_SCHEMA, L_DROP
        std::uint32_t tid = 0;
        std::string key, name;
        // L_ROW: fila ya codificada en v6 (encode_srow con el def vivo bajo
        // lock) — la capa delta la trata como bytes opacos. L_TOMB: la clave.
        std::vector<std::uint8_t> payload;
        TableDef schema;           // L_SCHEMA: def serializable
    };
    // Checkpoint incremental completo (protocolo arriba). Actualiza el estado
    // en memoria al volver. Lanza IoError/Corruption.
    void checkpoint(std::uint64_t new_watermark,
                    const std::vector<WriteRec>& batch,
                    std::uint64_t& freed_out, std::uint64_t& copied_out);

    // Recrea el archivo vacío con watermark=base_lsn (tras un FC). seq sigue
    // creciendo para que ningún slot viejo gane a uno nuevo.
    void reset(std::uint64_t watermark);
    // Borra el archivo por completo (delta obsoleto: watermark ≤ base).
    void drop_file();

    ~Store();
    Store() noexcept;             // estado vacío inactivo (miembro de DbEngine)
    Store(Store&& o) noexcept;
    Store& operator=(Store&& o) noexcept;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    struct Impl;                // interno (PageOut del .cpp lo usa)
    Impl* impl_ = nullptr;
};

} // namespace gina::delta
