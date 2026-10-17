#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "jitnodes/core/alignedArray.h"

namespace jitnodes {

// A run of points in the point arrays: a polyline (open) or polygon (closed).
struct Primitive {
    uint32_t start = 0;
    uint32_t count = 0;
    bool closed = false;
};

// 2D geometry in Houdini terms: points carry named float attributes (structure-of-arrays;
// "x" and "y" always exist, like P), primitives index point ranges, and the detail holds
// single global values. Point attribute arrays are padded/aligned for SIMD kernels.
class Geometry {
public:
    Geometry();

    size_t pointCount() const { return pointCount_; }
    size_t paddedPointCount() const { return padToLanes(pointCount_); }
    void resizePoints(size_t n);

    bool hasPointAttrib(const std::string& name) const { return pointAttribs_.count(name) != 0; }
    // Creates the attribute (zero filled) when it does not exist.
    AlignedArray<float>& pointAttrib(const std::string& name);
    const AlignedArray<float>* findPointAttrib(const std::string& name) const;
    const std::map<std::string, AlignedArray<float>>& pointAttribs() const { return pointAttribs_; }

    float* x() { return pointAttrib("x").data(); }
    float* y() { return pointAttrib("y").data(); }
    const float* x() const { return findPointAttrib("x")->data(); }
    const float* y() const { return findPointAttrib("y")->data(); }

    std::vector<Primitive>& primitives() { return primitives_; }
    const std::vector<Primitive>& primitives() const { return primitives_; }

    void setDetail(const std::string& name, float value) { detail_[name] = value; }
    float detail(const std::string& name, float fallback = 0.0f) const;
    const std::map<std::string, float>& details() const { return detail_; }

    // Appends all points/primitives of other. Attributes missing on either side are zero filled.
    void append(const Geometry& other);

private:
    size_t pointCount_ = 0;
    std::map<std::string, AlignedArray<float>> pointAttribs_;
    std::vector<Primitive> primitives_;
    std::map<std::string, float> detail_;
};

// Cooked geometry is immutable and shared between nodes (cheap fan-out). A node that wants to
// modify its input makes a private copy first (copy-on-write at node granularity).
using GeometryHandle = std::shared_ptr<const Geometry>;

}  // namespace jitnodes
