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

Geometry Geometry::concat(const std::vector<const Geometry*>& parts, ThreadPool* pool) {
    Geometry out;
    size_t totalPoints = 0;
    size_t totalPrims = 0;
    for (const Geometry* g : parts) {
        totalPoints += g->pointCount_;
        totalPrims += g->primitives_.size();
    }

    out.primitives_.reserve(totalPrims);
    size_t pointBase = 0;
    for (const Geometry* g : parts) {
        for (Primitive p : g->primitives_) {
            p.start += static_cast<uint32_t>(pointBase);
            out.primitives_.push_back(p);
        }
        pointBase += g->pointCount_;
    }
    out.resizePoints(totalPoints);

    for (AttribClass cls : {AttribClass::point, AttribClass::primitive})
        for (const Geometry* g : parts)
            for (const auto& [name, array] : g->attribMap(cls)) out.attrib(cls, name, array.type());

    struct Copy {
        AttribArray* dst;
        size_t offset;
        const AttribArray* src;
    };
    std::vector<Copy> copies;
    size_t pointOffset = 0;
    size_t primOffset = 0;
    for (const Geometry* g : parts) {
        for (const auto& [name, array] : g->pointAttribs_) copies.push_back({&out.pointAttribs_.at(name), pointOffset, &array});
        for (const auto& [name, array] : g->primAttribs_) copies.push_back({&out.primAttribs_.at(name), primOffset, &array});
        for (const auto& [name, value] : g->detail_) out.detail_.try_emplace(name, value);
        pointOffset += g->pointCount_;
        primOffset += g->primitives_.size();
    }

    auto run = [&](size_t i) { copies[i].dst->copyFrom(copies[i].offset, *copies[i].src); };
    if (pool && copies.size() > 1) pool->parallelFor(copies.size(), run);
    else
        for (size_t i = 0; i < copies.size(); ++i) run(i);
    return out;
}

bool sameGeometry(const Geometry& a, const Geometry& b) {
    if (a.pointCount() != b.pointCount() || a.primitiveCount() != b.primitiveCount() || a.details() != b.details()) return false;
    for (size_t i = 0; i < a.primitiveCount(); ++i) {
        const Primitive& p = a.primitives()[i];
        const Primitive& q = b.primitives()[i];
        if (p.start != q.start || p.count != q.count || p.closed != q.closed || p.kind != q.kind) return false;
    }
    for (AttribClass cls : {AttribClass::point, AttribClass::primitive}) {
        const auto& left = a.attribs(cls);
        const auto& right = b.attribs(cls);
        if (left.size() != right.size()) return false;
        for (auto l = left.begin(), r = right.begin(); l != left.end(); ++l, ++r) {
            if (l->first != r->first || l->second.type() != r->second.type() || l->second.size() != r->second.size()) return false;
            if (l->second.sharesStorageWith(r->second)) continue;
            for (size_t i = 0; i < l->second.size(); ++i)
                if (l->second.at(i) != r->second.at(i)) return false;
        }
    }
    return true;
}

}  // namespace jitnodes
