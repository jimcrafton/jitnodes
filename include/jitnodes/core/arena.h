#pragma once

#include <cstddef>
#include <cstdint>

namespace jitnodes {

// Linear scratch allocator for per-cook temporaries. Every allocation is 64-byte aligned
// (safe for AVX-512) and reset() releases everything in O(1).
class Arena {
public:
    explicit Arena(size_t capacityBytes);
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    ~Arena();

    void* allocate(size_t bytes);  // throws std::bad_alloc when exhausted

    template <class T>
    T* allocateArray(size_t count) {
        return static_cast<T*>(allocate(count * sizeof(T)));
    }

    void reset() { offset_ = 0; }
    size_t used() const { return offset_; }
    size_t capacity() const { return capacity_; }

private:
    uint8_t* base_ = nullptr;
    size_t offset_ = 0;
    size_t capacity_ = 0;
};

}  // namespace jitnodes
