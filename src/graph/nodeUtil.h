#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "jitnodes/core/threadPool.h"

namespace jitnodes {

// Counter-based random numbers: every value is a pure function of (seed, index, stream), so results do not depend on the thread count.
inline uint32_t hash32(uint32_t seed, uint32_t index, uint32_t stream) {
    uint32_t h = seed * 0x9E3779B1u ^ index * 0x85EBCA6Bu ^ stream * 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

// Uniform in [0, 1).
inline float random01(uint32_t seed, uint32_t index, uint32_t stream) {
    return static_cast<float>(hash32(seed, index, stream) >> 8) * (1.0f / 16777216.0f);
}

inline uint32_t seedOf(float value) { return static_cast<uint32_t>(static_cast<int64_t>(value)); }

// Runs fn(begin, end) over [0, count) in chunks, on the pool when one is given.
inline void parallelChunks(ThreadPool* pool, size_t count, const std::function<void(size_t, size_t)>& fn, size_t chunk = 16384) {
    if (count == 0) return;
    const size_t chunks = (count + chunk - 1) / chunk;
    if (!pool || chunks == 1) {
        fn(0, count);
        return;
    }
    pool->parallelFor(chunks, [&](size_t c) { fn(c * chunk, std::min(count, (c + 1) * chunk)); });
}

}  // namespace jitnodes
