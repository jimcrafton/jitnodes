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

AttribArray& Geometry::attribLike(AttribClass cls, const std::string& name, const AttribArray& like) {
    auto& map = attribMap(cls);
    const bool existed = map.count(name) != 0;
    AttribArray& a = attrib(cls, name, like.type());
    if (!existed && like.type() == AttribType::string) a.setTable(like.tableHandle());
    return a;
}

void Geometry::checkStrings() const {
    for (AttribClass cls : {AttribClass::point, AttribClass::primitive}) {
        for (const auto& [name, array] : attribMap(cls)) {
            if (array.type() != AttribType::string) continue;
            const size_t tableSize = array.table().strings.size();
            const int32_t* ids = array.ints().data();
            for (size_t i = 0; i < array.size(); ++i)
                if (ids[i] < 0 || static_cast<size_t>(ids[i]) >= tableSize)
                    throw std::runtime_error("string attribute '" + name + "' has an id outside its table at element " + std::to_string(i));
        }
    }
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
        for (const auto& [name, array] : other.attribMap(cls)) attribLike(cls, name, array);

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
            for (const auto& [name, array] : g->attribMap(cls)) out.attribLike(cls, name, array);

    // The tables are merged here, one part after another, so the parallel copies below never change a table.
    struct Copy {
        AttribArray* dst;
        size_t offset;
        const AttribArray* src;
        std::vector<int32_t> remap;
    };
    std::vector<Copy> copies;
    auto addCopy = [&](AttribArray& dst, size_t offset, const AttribArray& src) {
        Copy c{&dst, offset, &src, {}};
        if (src.type() == AttribType::string) c.remap = dst.mergeTable(src);
        copies.push_back(std::move(c));
    };
    size_t pointOffset = 0;
    size_t primOffset = 0;
    for (const Geometry* g : parts) {
        for (const auto& [name, array] : g->pointAttribs_) addCopy(out.pointAttribs_.at(name), pointOffset, array);
        for (const auto& [name, array] : g->primAttribs_) addCopy(out.primAttribs_.at(name), primOffset, array);
        for (const auto& [name, value] : g->detail_) out.detail_.try_emplace(name, value);
        pointOffset += g->pointCount_;
        primOffset += g->primitives_.size();
    }

    auto run = [&](size_t i) {
        Copy& c = copies[i];
        if (c.src->type() == AttribType::string) c.dst->copyIds(c.offset, *c.src, c.remap);
        else c.dst->copyFrom(c.offset, *c.src);
    };
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
            if (l->second.type() == AttribType::string) {
                for (size_t i = 0; i < l->second.size(); ++i)
                    if (l->second.stringAt(i) != r->second.stringAt(i)) return false;
                continue;
            }
            for (size_t i = 0; i < l->second.size(); ++i)
                if (l->second.at(i) != r->second.at(i)) return false;
        }
    }
    return true;
}

}  // namespace jitnodes
