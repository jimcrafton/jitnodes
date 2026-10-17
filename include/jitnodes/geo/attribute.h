#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>

#include "jitnodes/core/alignedArray.h"

namespace jitnodes {

enum class AttribClass : uint8_t { point, primitive };
enum class AttribType : uint8_t { float32, int32 };

// Copying an AttribArray shares its storage; the first non-const access of a shared array clones it.
class AttribArray {
public:
    explicit AttribArray(AttribType type = AttribType::float32) : type_(type) {
        if (type_ == AttribType::float32) floats_ = std::make_shared<AlignedArray<float>>();
        else ints_ = std::make_shared<AlignedArray<int32_t>>();
    }

    AttribType type() const { return type_; }
    size_t size() const { return size_; }
    size_t paddedSize() const { return padToLanes(size_); }

    void resize(size_t n) {
        if (n == size_) return;
        if (type_ == AttribType::float32) resizeStorage(floats_, n);
        else resizeStorage(ints_, n);
        size_ = n;
    }

    AlignedArray<float>& floats() {
        requireType(AttribType::float32);
        detach(floats_);
        return *floats_;
    }
    const AlignedArray<float>& floats() const {
        requireType(AttribType::float32);
        return *floats_;
    }
    AlignedArray<int32_t>& ints() {
        requireType(AttribType::int32);
        detach(ints_);
        return *ints_;
    }
    const AlignedArray<int32_t>& ints() const {
        requireType(AttribType::int32);
        return *ints_;
    }

    void* data() { return type_ == AttribType::float32 ? static_cast<void*>(floats().data()) : ints().data(); }
    const void* data() const { return type_ == AttribType::float32 ? static_cast<const void*>(floats_->data()) : ints_->data(); }

    float at(size_t i) const { return type_ == AttribType::float32 ? (*floats_)[i] : static_cast<float>((*ints_)[i]); }

    bool sharesStorageWith(const AttribArray& o) const {
        return type_ == o.type_ && (type_ == AttribType::float32 ? floats_ == o.floats_ : ints_ == o.ints_);
    }

    // Copies src's elements into this array starting at dstOffset; both must have the same type and fit.
    void copyFrom(size_t dstOffset, const AttribArray& src) {
        requireType(src.type_);
        if (src.size_ == 0) return;
        if (type_ == AttribType::float32) std::memcpy(floats().data() + dstOffset, src.floats_->data(), src.size_ * sizeof(float));
        else std::memcpy(ints().data() + dstOffset, src.ints_->data(), src.size_ * sizeof(int32_t));
    }

private:
    void requireType(AttribType t) const {
        if (t != type_) throw std::logic_error("attribute type mismatch");
    }

    template <class T>
    static void detach(std::shared_ptr<AlignedArray<T>>& storage) {
        if (storage.use_count() > 1) storage = std::make_shared<AlignedArray<T>>(*storage);
    }

    template <class T>
    void resizeStorage(std::shared_ptr<AlignedArray<T>>& storage, size_t n) {
        if (storage.use_count() <= 1) {
            storage->resize(n);
            return;
        }
        auto fresh = std::make_shared<AlignedArray<T>>(n);
        const size_t keep = n < size_ ? n : size_;
        if (keep) std::memcpy(fresh->data(), storage->data(), keep * sizeof(T));
        storage = std::move(fresh);
    }

    AttribType type_;
    size_t size_ = 0;
    std::shared_ptr<AlignedArray<float>> floats_;
    std::shared_ptr<AlignedArray<int32_t>> ints_;
};

}  // namespace jitnodes
