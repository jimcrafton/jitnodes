#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "jitnodes/core/threadPool.h"
#include "jitnodes/graph/simulationCache.h"

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

template <class T>
size_t vectorBytes(const std::vector<T>& v) {
    return v.capacity() * sizeof(T);
}

// Copies of a simulation's state at regular steps, so scrubbing back resumes from the nearest copy instead of simulating from the start.
// A node calls offer() after each step it advances; only multiples of the current interval are kept. When the copies outgrow the byte
// budget or the count limit the interval doubles and every other copy goes, so the replay after a jump back is at most one interval
// (about total steps / maxEntries). The caller clears the cache whenever the simulation restarts for another reason (a parameter or the
// input changed), because the copies then belong to a different run.
template <class S>
class StateCheckpoints {
public:
    struct Entry {
        int64_t step;
        size_t bytes;
        S state;
    };

    static constexpr size_t maxEntries = 64;

    void clear() {
        saved_.clear();
        bytes_ = 0;
        interval_ = std::max<int64_t>(SimulationCache::checkpointInterval, 1);
    }

    // The copy to continue from when the simulation sits at `current` and `target` is wanted: the latest one at or before target, if
    // that is behind current (scrubbing back) or ahead of it (a jump forward past copies saved earlier). Null when continuing is as good.
    const Entry* resumeFor(int64_t current, int64_t target) const {
        auto it = std::upper_bound(saved_.begin(), saved_.end(), target, [](int64_t s, const Entry& e) { return s < e.step; });
        if (it == saved_.begin()) return nullptr;
        const Entry& e = *(it - 1);
        if (target >= current && e.step <= current) return nullptr;
        ++SimulationCache::restores;
        return &e;
    }

    void offer(int64_t step, const S& state, size_t bytes) {
        const size_t budget = SimulationCache::budgetBytes;
        if (interval_ == 0) interval_ = std::max<int64_t>(SimulationCache::checkpointInterval, 1);
        if (step % interval_ != 0 || bytes > budget / 2) return;
        auto at = std::lower_bound(saved_.begin(), saved_.end(), step, [](const Entry& e, int64_t s) { return e.step < s; });
        if (at != saved_.end() && at->step == step) return;
        bytes_ += bytes;
        saved_.insert(at, Entry{step, bytes, state});
        while (bytes_ > budget || saved_.size() > maxEntries) {
            interval_ *= 2;
            auto kept = std::remove_if(saved_.begin(), saved_.end(), [&](const Entry& e) {
                const bool drop = e.step % interval_ != 0;
                if (drop) bytes_ -= e.bytes;
                return drop;
            });
            saved_.erase(kept, saved_.end());
        }
    }

private:
    std::vector<Entry> saved_;
    size_t bytes_ = 0;
    int64_t interval_ = 0;
};

// Runs fn(begin, end) over [0, count) in chunks, on the pool when one is given. chunk = 0 aims for about four chunks per thread, between
// 256 and 16384 points, so heavy per-point work on a few thousand points still spreads over the pool.
inline void parallelChunks(ThreadPool* pool, size_t count, const std::function<void(size_t, size_t)>& fn, size_t chunk = 0) {
    if (count == 0) return;
    if (chunk == 0) chunk = pool ? std::clamp<size_t>(count / (4 * pool->concurrency()), 256, 16384) : 16384;
    const size_t chunks = (count + chunk - 1) / chunk;
    if (!pool || chunks == 1) {
        fn(0, count);
        return;
    }
    pool->parallelFor(chunks, [&](size_t c) { fn(c * chunk, std::min(count, (c + 1) * chunk)); });
}

}  // namespace jitnodes
