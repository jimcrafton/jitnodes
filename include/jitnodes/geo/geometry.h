#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "jitnodes/core/alignedArray.h"
#include "jitnodes/geo/attribute.h"

namespace jitnodes {

struct Primitive {
    uint32_t start = 0;
    uint32_t count = 0;
    bool closed = false;
};

class Geometry {
public:
    Geometry();

    size_t pointCount() const { return pointCount_; }
    size_t paddedPointCount() const { return padToLanes(pointCount_); }
    void resizePoints(size_t n);

    size_t primitiveCount() const { return primitives_.size(); }
    void addPrimitive(const Primitive& p);
    const std::vector<Primitive>& primitives() const { return primitives_; }

    size_t count(AttribClass cls) const { return cls == AttribClass::point ? pointCount() : primitiveCount(); }
    size_t paddedCount(AttribClass cls) const { return padToLanes(count(cls)); }

    // Creates the attribute (zero filled) when missing; throws if it exists with another type.
    AttribArray& attrib(AttribClass cls, const std::string& name, AttribType type = AttribType::float32);
    const AttribArray* findAttrib(AttribClass cls, const std::string& name) const;
    bool hasAttrib(AttribClass cls, const std::string& name) const { return findAttrib(cls, name) != nullptr; }
    const std::map<std::string, AttribArray>& attribs(AttribClass cls) const;

    // Vector attributes are stored as float components "name.x", "name.y", ... (tupleSize 2..4).
    void addVectorAttrib(AttribClass cls, const std::string& name, int tupleSize);

    // Groups are int attributes named "group.<name>" holding 0/1, so kernels can read and write them.
    AttribArray& createGroup(AttribClass cls, const std::string& name);
    bool inGroup(AttribClass cls, const std::string& name, size_t index) const;

    bool hasPointAttrib(const std::string& name) const { return hasAttrib(AttribClass::point, name); }
    AlignedArray<float>& pointAttrib(const std::string& name) { return attrib(AttribClass::point, name).floats(); }
    const AlignedArray<float>* findPointAttrib(const std::string& name) const;

    float* x() { return pointAttrib("x").data(); }
    float* y() { return pointAttrib("y").data(); }
    const float* x() const { return findPointAttrib("x")->data(); }
    const float* y() const { return findPointAttrib("y")->data(); }

    void setDetail(const std::string& name, float value) { detail_[name] = value; }
    float detail(const std::string& name, float fallback = 0.0f) const;
    const std::map<std::string, float>& details() const { return detail_; }

    // Appends points, primitives and attributes of other; attributes missing on either side are zero filled.
    void append(const Geometry& other);

private:
    std::map<std::string, AttribArray>& attribMap(AttribClass cls) { return cls == AttribClass::point ? pointAttribs_ : primAttribs_; }
    const std::map<std::string, AttribArray>& attribMap(AttribClass cls) const { return cls == AttribClass::point ? pointAttribs_ : primAttribs_; }

    size_t pointCount_ = 0;
    std::map<std::string, AttribArray> pointAttribs_;
    std::map<std::string, AttribArray> primAttribs_;
    std::vector<Primitive> primitives_;
    std::map<std::string, float> detail_;
};

using GeometryHandle = std::shared_ptr<const Geometry>;

}  // namespace jitnodes
