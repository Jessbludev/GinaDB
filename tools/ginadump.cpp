// GinaDB Lite 0.7 — ginadump: offline inspector (storage-level view).
// Reads the database directory and prints snapshot + WAL status without
// the C ABI: it opens a Database and reports engine stats + tables.
#include "gina/db.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <db-dir>\n", argv[0]);
        return 1;
    }
    try {
        gina::Database db{argv[1]};
        const auto s = db.stats();
        std::cout << "GinaDB Lite 0.7 — " << argv[1] << "\n"
                  << "  tables=" << s.tables << " rows=" << s.rows
                  << " last_lsn=" << s.last_lsn << " commits=" << s.commits << "\n"
                  << "  snapshot: pages=" << s.pages
                  << " overflow=" << s.overflow_pages
                  << " bytes=" << s.data_bytes << "\n"
                  << "  wal: bytes=" << s.wal_bytes
                  << " (rebased at last checkpoint)\n"
                  << "  recovered_bad_tail=" << s.recovered_bad_tail << "\n";
        for (const auto& name : db.tables()) {
            const auto def = db[name].schema();
            std::cout << "  table '" << name << "' pk='" << def.primary_key
                      << "' schemaless=" << (def.schemaless ? "yes" : "no")
                      << " rows=" << db[name].count() << " cols=[";
            for (const auto& c : def.columns)
                std::cout << c.name << ":" << gina::type_name(c.type)
                          << (c.fts ? "+fts" : "") << " ";
            std::cout << "]\n";
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
    return 0;
}
