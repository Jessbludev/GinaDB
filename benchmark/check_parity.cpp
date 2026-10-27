// verificación de paridad motor-vs-Zig sobre una BD con delta corrupto
#include "gina/db.hpp"
#include <cstdio>
using namespace gina;
int main(int argc, char** argv) {
    Database db{argc > 1 ? argv[1] : "/tmp/zig-corrupt"};
    const auto st = db.stats();
    printf("motor:   tablas=%zu users=%zu corrupt=%llu broken_ovf=%llu\n",
           db.tables().size(), db["users"].count(),
           (unsigned long long)st.delta_corrupt_pages,
           (unsigned long long)st.delta_broken_ovf_records);
    return 0;
}
