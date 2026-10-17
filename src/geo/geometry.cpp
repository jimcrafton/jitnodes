#include "jitnodes/geo/geometry.h"

#include <stdexcept>

namespace jitnodes {

Geometry::Geometry() {
    pointAttribs_.try_emplace("x");
    pointAttribs_.try_emplace("y");
}

void Geometry::resizePoints(size_t n) {
    for (auto& [name, array] : pointAttribs_) array.resize(n);
    pointCount_ = n;
}

void Geometry::addPrimitive(const Primitive& p) {
    primitives_.push_back(p);
    for (auto& [name, array] : primAttribs_) array.resize(primitives_.size());
}

AttribArray& Geometry::attrib(AttribClass cls, const std::string& name, AttribType type) {
    auto& map = attribMap(cls);
    auto [it, inserted] = map.try_emplace(name, type);
    if (inserted) it->second.resize(count(cls));
    else if (it->second.type() != type) throw std::runtime_error("attribute '" + name + "' already exists with a different type");
    return it->second;
}

const AttribArray* Geometry::findAttrib(AttribClass cls, const std::string& name) const {
    const auto& map = attribMap(cls);
    auto it = map.find(name);
    return it == map.end() ? nullptr : &it->second;
}

const std::map<std::string, AttribArray>& Geometry::attribs(AttribClass cls) const { return attribMap(cls); }

const AlignedArray<float>* Geometry::findPointAttrib(const std::string& name) const {
    const AttribArray* a = findAttrib(AttribClass::point, name);
    return a ? &a->floats() : nullptr;
}

void Geometry::addVectorAttrib(AttribClass cls, const std::string& name, int tupleSize) {
    if (tupleSize < 2 || tupleSize > 4) throw std::runtime_error("vector attribute tuple size must be 2..4");
    static const char* components[] = {"x", "y", "z", "w"};
    for (int i = 0; i < tupleSize; ++i) attrib(cls, name + "." + components[i]);
}

AttribArray& Geometry::createGroup(AttribClass cls, const std::string& name) {
    return attrib(cls, "group." + name, AttribType::int32);
}

bool Geometry::inGroup(AttribClass cls, const std::string& name, size_t index) const {
    const AttribArray* a = findAttrib(cls, "group." + name);
    return a && index < a->size() && a->at(index) != 0.0f;
}

float Geometry::detail(const std::string& name, float fallback) const {
    auto it = detail_.find(name);
    return it == detail_.end() ? fallback : it->second;
}

void Geometry::append(const Geometry& other) {
    const size_t pointBase = pointCount_;
    const size_t primBase = primitives_.size();

    for (AttribClass cls : {AttribClass::point, AttribClass::primitive})
        for (const auto& [name, array] : other.attribMap(cls)) attrib(cls, name, array.type());

    resizePoints(pointBase + other.pointCount_);
    for (Primitive p : other.primitives_) {
        p.start += static_cast<uint32_t>(pointBase);
        primitives_.push_back(p);
    }
    for (auto& [name, array] : primAttribs_) array.resize(primitives_.size());

    for (const auto& [name, array] : other.pointAttribs_) pointAttribs_.at(name).copyFrom(pointBase, array);
    for (const auto& [name, array] : other.primAttribs_) primAttribs_.at(name).copyFrom(primBase, array);
    for (const auto& [name, value] : other.detail_) detail_.try_emplace(name, value);
}

}  // namespace jitnodes
