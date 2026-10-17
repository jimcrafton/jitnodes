#include "jitnodes/core/arena.h"

#include <new>

#include "jitnodes/core/alignedArray.h"

namespace jitnodes {

Arena::Arena(size_t capacityBytes) : capacity_(capacityBytes) {
    base_ = static_cast<uint8_t*>(alignedAlloc(capacityBytes, 64));
}

Arena::~Arena() { alignedFree(base_); }

void* Arena::allocate(size_t bytes) {
    size_t aligned = (bytes + 63) & ~size_t(63);
    if (offset_ + aligned > capacity_) throw std::bad_alloc();
    void* p = base_ + offset_;
    offset_ += aligned;
    return p;
}

}  // namespace jitnodes
