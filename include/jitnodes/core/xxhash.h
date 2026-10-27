#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace jitnodes {

// XXH64 (seed 0 by default), little-endian hosts only; the output matches the reference implementation.
inline uint64_t xxh64(const void* data, size_t length, uint64_t seed = 0) {
    static_assert(std::endian::native == std::endian::little, "xxh64 reads words in host order");
    constexpr uint64_t p1 = 11400714785074694791ull, p2 = 14029467366897019727ull, p3 = 1609587929392839161ull,
                       p4 = 9650029242287828579ull, p5 = 2870177450012600261ull;
    const auto read64 = [](const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; };
    const auto read32 = [](const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; };
    const auto round = [&](uint64_t acc, uint64_t input) { return std::rotl(acc + input * p2, 31) * p1; };
    const auto merge = [&](uint64_t h, uint64_t v) { return (h ^ round(0, v)) * p1 + p4; };

    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* const end = p + length;
    uint64_t h;
    if (length >= 32) {
        uint64_t v1 = seed + p1 + p2, v2 = seed + p2, v3 = seed, v4 = seed - p1;
        for (; end - p >= 32; p += 32) {
            v1 = round(v1, read64(p));
            v2 = round(v2, read64(p + 8));
            v3 = round(v3, read64(p + 16));
            v4 = round(v4, read64(p + 24));
        }
        h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
        h = merge(h, v1);
        h = merge(h, v2);
        h = merge(h, v3);
        h = merge(h, v4);
    } else {
        h = seed + p5;
    }
    h += length;
    for (; end - p >= 8; p += 8) h = std::rotl(h ^ round(0, read64(p)), 27) * p1 + p4;
    if (end - p >= 4) {
        h = std::rotl(h ^ (static_cast<uint64_t>(read32(p)) * p1), 23) * p2 + p3;
        p += 4;
    }
    for (; p < end; ++p) h = std::rotl(h ^ (*p * p5), 11) * p1;
    h ^= h >> 33;
    h *= p2;
    h ^= h >> 29;
    h *= p3;
    h ^= h >> 32;
    return h;
}

}  // namespace jitnodes
