#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "jitnodes/core/alignedArray.h"

namespace jitnodes {

enum class AttribClass : uint8_t { point, primitive };
enum class AttribType : uint8_t { float32, int32 };

class AttribArray {
public:
    explicit AttribArray(AttribType type = AttribType::float32) : type_(type) {}

    AttribType type() const { return type_; }
    size_t size() const { return size_; }
    size_t paddedSize() const { return padToLanes(size_); }

    void resize(size_t n) {
        if (type_ == AttribType::float32) floats_.resize(n);
        else ints_.resize(n);
        size_ = n;
    }

    AlignedArray<float>& floats() {
        requireType(AttribType::float32);
        return floats_;
    }
    const AlignedArray<float>& floats() const {
        requireType(AttribType::float32);
        return floats_;
    }
    AlignedArray<int32_t>& ints() {
        requireType(AttribType::int32);
        return ints_;
    }
    const AlignedArray<int32_t>& ints() const {
        requireType(AttribType::int32);
        return ints_;
    }

    void* data() { return type_ == AttribType::float32 ? static_cast<void*>(floats_.data()) : ints_.data(); }
    const void* data() const { return type_ == AttribType::float32 ? static_cast<const void*>(floats_.data()) : ints_.data(); }

    float at(size_t i) const { return type_ == AttribType::float32 ? floats_[i] : static_cast<float>(ints_[i]); }

    // Copies src's elements into this array starting at dstOffset; both must have the same type and fit.
    void copyFrom(size_t dstOffset, const AttribArray& src) {
        requireType(src.type_);
        if (src.size_ == 0) return;
        if (type_ == AttribType::float32) std::memcpy(floats_.data() + dstOffset, src.floats_.data(), src.size_ * sizeof(float));
        else std::memcpy(ints_.data() + dstOffset, src.ints_.data(), src.size_ * sizeof(int32_t));
    }

private:
    void requireType(AttribType t) const {
        if (t != type_) throw std::logic_error("attribute type mismatch");
    }

    AttribType type_;
    size_t size_ = 0;
    AlignedArray<float> floats_;
    AlignedArray<int32_t> ints_;
};

}  // namespace jitnodes
