#include "jitnodes/geo/geometry.h"

#include <cstring>

namespace jitnodes {

Geometry::Geometry() {
    pointAttribs_["x"];
    pointAttribs_["y"];
}

void Geometry::resizePoints(size_t n) {
    for (auto& [name, array] : pointAttribs_) array.resize(n);
    pointCount_ = n;
}

AlignedArray<float>& Geometry::pointAttrib(const std::string& name) {
    auto [it, inserted] = pointAttribs_.try_emplace(name);
    if (inserted) it->second.resize(pointCount_);
    return it->second;
}

const AlignedArray<float>* Geometry::findPointAttrib(const std::string& name) const {
    auto it = pointAttribs_.find(name);
    return it == pointAttribs_.end() ? nullptr : &it->second;
}

float Geometry::detail(const std::string& name, float fallback) const {
    auto it = detail_.find(name);
    return it == detail_.end() ? fallback : it->second;
}

void Geometry::append(const Geometry& other) {
    const size_t base = pointCount_;
    for (const auto& entry : other.pointAttribs_) pointAttrib(entry.first);
    resizePoints(base + other.pointCount_);
    for (const auto& [name, array] : other.pointAttribs_) {
        if (other.pointCount_)
            std::memcpy(pointAttribs_[name].data() + base, array.data(), other.pointCount_ * sizeof(float));
    }
    for (Primitive p : other.primitives_) {
        p.start += static_cast<uint32_t>(base);
        primitives_.push_back(p);
    }
    for (const auto& [name, value] : other.detail_) detail_.try_emplace(name, value);
}

}  // namespace jitnodes
