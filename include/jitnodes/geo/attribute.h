#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "jitnodes/core/alignedArray.h"

namespace jitnodes {

enum class AttribClass : uint8_t { point, primitive };
// string: the data are int32 ids into the attribute's StringTable, so everything that moves int32 values moves them unchanged.
enum class AttribType : uint8_t { float32, int32, string };

// int32 and string attributes are stored (and loaded by kernels) as int32.
inline bool isIntegerStorage(AttribType t) { return t != AttribType::float32; }

// Ids stay exact as the float a kernel converts them to.
constexpr size_t maxStrings = (size_t{1} << 24) - 1;

// Id 0 is always the empty string.
struct StringTable {
    std::vector<std::string> strings{std::string()};
    std::unordered_map<std::string, int32_t> index{{std::string(), 0}};

    int32_t find(const std::string& text) const {
        auto it = index.find(text);
        return it == index.end() ? -1 : it->second;
    }
    int32_t add(const std::string& text) {
        auto it = index.find(text);
        if (it != index.end()) return it->second;
        if (strings.size() >= maxStrings) throw std::runtime_error("string table is full");
        const int32_t id = static_cast<int32_t>(strings.size());
        strings.push_back(text);
        index.emplace(text, id);
        return id;
    }
};

// Copying an AttribArray shares its storage; the first non-const access of a shared array clones it. A string table is shared the same way
// and cloned before it changes.
class AttribArray {
public:
    explicit AttribArray(AttribType type = AttribType::float32) : type_(type) {
        if (type_ == AttribType::float32) floats_ = std::make_shared<AlignedArray<float>>();
        else ints_ = std::make_shared<AlignedArray<int32_t>>();
        if (type_ == AttribType::string) table_ = std::make_shared<const StringTable>();
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
    // For a string attribute these are the ids.
    AlignedArray<int32_t>& ints() {
        requireIntegerStorage();
        detach(ints_);
        return *ints_;
    }
    const AlignedArray<int32_t>& ints() const {
        requireIntegerStorage();
        return *ints_;
    }

    void* data() { return type_ == AttribType::float32 ? static_cast<void*>(floats().data()) : ints().data(); }
    const void* data() const { return type_ == AttribType::float32 ? static_cast<const void*>(floats_->data()) : ints_->data(); }

    float at(size_t i) const { return type_ == AttribType::float32 ? (*floats_)[i] : static_cast<float>((*ints_)[i]); }

    bool sharesStorageWith(const AttribArray& o) const {
        return type_ == o.type_ && (type_ == AttribType::float32 ? floats_ == o.floats_ : ints_ == o.ints_);
    }

    const StringTable& table() const {
        requireType(AttribType::string);
        return *table_;
    }
    const std::shared_ptr<const StringTable>& tableHandle() const {
        requireType(AttribType::string);
        return table_;
    }
    void setTable(std::shared_ptr<const StringTable> table) {
        requireType(AttribType::string);
        table_ = std::move(table);
    }
    // The text of element i; throws when its id is not in the table.
    const std::string& stringAt(size_t i) const {
        const int32_t id = ints()[i];
        const StringTable& t = table();
        if (id < 0 || static_cast<size_t>(id) >= t.strings.size()) throw std::runtime_error("string attribute id is outside its table");
        return t.strings[static_cast<size_t>(id)];
    }
    // The id of text, added to the table when missing.
    int32_t intern(const std::string& text) {
        requireType(AttribType::string);
        const int32_t found = table_->find(text);
        if (found >= 0) return found;
        return mutableTable().add(text);
    }

    // Adds the strings of src's table that this table lacks. The result maps src ids to this table's ids; it is empty when no id changes
    // (the same table, or the strings already agree).
    std::vector<int32_t> mergeTable(const AttribArray& src) {
        requireType(AttribType::string);
        src.requireType(AttribType::string);
        if (table_ == src.table_) return {};
        std::vector<int32_t> remap(src.table_->strings.size());
        bool identity = true;
        for (size_t i = 0; i < remap.size(); ++i) {
            const std::string& text = src.table_->strings[i];
            int32_t id = table_->find(text);
            if (id < 0) id = mutableTable().add(text);
            remap[i] = id;
            identity &= id == static_cast<int32_t>(i);
        }
        if (identity) return {};
        return remap;
    }

    // Copies src's elements into this array starting at dstOffset; both must have the same type and fit. String ids are translated to this
    // array's table (which grows by the strings it lacks), so after the copy every element still means the same text.
    void copyFrom(size_t dstOffset, const AttribArray& src) {
        requireType(src.type_);
        if (src.size_ == 0) return;
        if (type_ == AttribType::string) {
            copyIds(dstOffset, src, mergeTable(src));
            return;
        }
        if (type_ == AttribType::float32) std::memcpy(floats().data() + dstOffset, src.floats_->data(), src.size_ * sizeof(float));
        else std::memcpy(ints().data() + dstOffset, src.ints_->data(), src.size_ * sizeof(int32_t));
    }

    // The copy of a string attribute once the table is merged: `remap` is what mergeTable returned. Does not touch the table, so copies into
    // different ranges of one array may run in parallel.
    void copyIds(size_t dstOffset, const AttribArray& src, const std::vector<int32_t>& remap) {
        requireType(AttribType::string);
        src.requireType(AttribType::string);
        if (src.size_ == 0) return;
        int32_t* dst = ints().data() + dstOffset;
        const int32_t* from = src.ints_->data();
        if (remap.empty()) {
            std::memcpy(dst, from, src.size_ * sizeof(int32_t));
            return;
        }
        for (size_t i = 0; i < src.size_; ++i) {
            const int32_t id = from[i];
            if (id < 0 || static_cast<size_t>(id) >= remap.size()) throw std::runtime_error("string attribute id is outside its table");
            dst[i] = remap[static_cast<size_t>(id)];
        }
    }

private:
    void requireType(AttribType t) const {
        if (t != type_) throw std::logic_error("attribute type mismatch");
    }
    void requireIntegerStorage() const {
        if (!isIntegerStorage(type_)) throw std::logic_error("attribute type mismatch");
    }

    StringTable& mutableTable() {
        if (table_.use_count() > 1) table_ = std::make_shared<const StringTable>(*table_);
        return const_cast<StringTable&>(*table_);
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
    std::shared_ptr<const StringTable> table_;
};

}  // namespace jitnodes
