#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace jitnodes {

// SIMD friendly constants shared by the data layer and the JIT.
inline constexpr size_t simdAlignment = 64;  // AVX-512 register width in bytes (also covers SSE2/AVX2)
inline constexpr size_t simdLanes = 16;      // floats per AVX-512 register; arrays are padded to this

inline constexpr size_t padToLanes(size_t n) { return (n + simdLanes - 1) & ~(simdLanes - 1); }

inline void* alignedAlloc(size_t bytes, size_t alignment) {
    if (bytes == 0) bytes = alignment;
#if defined(_MSC_VER)
    void* p = _aligned_malloc(bytes, alignment);
#else
    void* p = std::aligned_alloc(alignment, (bytes + alignment - 1) & ~(alignment - 1));
#endif
    if (!p) throw std::bad_alloc();
    return p;
}

inline void alignedFree(void* p) {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

// Contiguous, 64-byte aligned array whose storage is always padded to a multiple of
// simdLanes elements and zero-initialised, so JIT kernels never need a tail loop.
template <class T>
class AlignedArray {
    static_assert(std::is_trivially_copyable_v<T>);

public:
    AlignedArray() = default;
    explicit AlignedArray(size_t n) { resize(n); }
    AlignedArray(const AlignedArray& o) { assign(o); }
    AlignedArray(AlignedArray&& o) noexcept { swap(o); }
    AlignedArray& operator=(const AlignedArray& o) {
        if (this != &o) assign(o);
        return *this;
    }
    AlignedArray& operator=(AlignedArray&& o) noexcept {
        swap(o);
        return *this;
    }
    ~AlignedArray() { alignedFree(data_); }

    void swap(AlignedArray& o) noexcept {
        std::swap(data_, o.data_);
        std::swap(size_, o.size_);
        std::swap(capacity_, o.capacity_);
    }

    // Grows or shrinks to n elements; new elements and padding are zero.
    void resize(size_t n) {
        size_t needed = padToLanes(n);
        if (needed > capacity_) {
            T* p = static_cast<T*>(alignedAlloc(needed * sizeof(T), simdAlignment));
            if (size_) std::memcpy(p, data_, size_ * sizeof(T));
            std::memset(p + size_, 0, (needed - size_) * sizeof(T));
            alignedFree(data_);
            data_ = p;
            capacity_ = needed;
        } else if (n < size_) {
            std::memset(data_ + n, 0, (size_ - n) * sizeof(T));
        } else if (n > size_) {
            std::memset(data_ + size_, 0, (n - size_) * sizeof(T));
        }
        size_ = n;
    }

    T* data() { return data_; }
    const T* data() const { return data_; }
    size_t size() const { return size_; }
    size_t paddedSize() const { return padToLanes(size_); }
    bool empty() const { return size_ == 0; }
    T& operator[](size_t i) { return data_[i]; }
    const T& operator[](size_t i) const { return data_[i]; }
    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }

private:
    void assign(const AlignedArray& o) {
        const size_t n = o.size_;
        const size_t needed = padToLanes(n);
        if (needed > capacity_) {
            alignedFree(data_);
            data_ = static_cast<T*>(alignedAlloc(needed * sizeof(T), simdAlignment));
            capacity_ = needed;
            std::memcpy(data_, o.data_, n * sizeof(T));
            std::memset(data_ + n, 0, (needed - n) * sizeof(T));
        } else {
            if (n) std::memcpy(data_, o.data_, n * sizeof(T));
            if (size_ > n) std::memset(data_ + n, 0, (size_ - n) * sizeof(T));
        }
        size_ = n;
    }

    T* data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
};

}  // namespace jitnodes
