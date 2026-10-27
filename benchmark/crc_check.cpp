// verificación del CRC slice-by-8 contra la versión bit a bit
#include "../src/codec.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

static std::uint32_t crc_old(const std::uint8_t* p, std::size_t n) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0u);
    }
    return ~c;
}

int main() {
    std::vector<std::uint8_t> buf(70000);
    for (std::size_t i = 0; i < buf.size(); i++)
        buf[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 24);
    int bad = 0;
    for (std::size_t len : {0u, 1u, 2u, 7u, 8u, 9u, 15u, 16u, 31u, 63u, 64u,
                            100u, 1000u, 4096u, 5000u, 65536u, 69999u, 70000u}) {
        const auto a = crc_old(buf.data(), len);
        const auto b = gina::codec::crc32(buf.data(), len);
        if (a != b) { printf("MISMATCH len=%zu old=%08x new=%08x\n", len, a, b); bad++; }
    }
    // y con todos-cero / todos-FF
    std::fill(buf.begin(), buf.begin() + 100, 0);
    std::fill(buf.begin() + 100, buf.begin() + 200, 0xFF);
    if (crc_old(buf.data(), 200) != gina::codec::crc32(buf.data(), 200)) {
        printf("MISMATCH patrón\n"); bad++;
    }
    printf(bad ? "CRC FAIL\n" : "CRC OK: idéntico en todos los casos\n");
    return bad;
}
