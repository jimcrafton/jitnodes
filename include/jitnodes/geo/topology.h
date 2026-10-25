#pragma once

#include <cstdint>
#include <vector>

#include "jitnodes/geo/geometry.h"

namespace jitnodes {

// A new point built from two source points: float attributes lerp(a, b, t); int attributes take
// a when t < 0.5, else b.
struct PointSample {
    uint32_t a = 0;
    uint32_t b = 0;
    float t = 0.0f;
};

// Builds a geometry whose points come from samples. Carries every point attribute, the detail
// values, and creates empty primitive attribute arrays of the same types (no primitives yet).
Geometry gatherPoints(const Geometry& src, const std::vector<PointSample>& samples);

// Fills dst's primitive attributes from src; srcIndex[i] is the source primitive of dst primitive i.
void transferPrimAttribs(const Geometry& src, Geometry& dst, const std::vector<uint32_t>& srcIndex);

// True when any primitive is a (well formed) cubic curve.
bool hasCurves(const Geometry& in);
// Replaces every cubic primitive by a polyline that stays within `tolerance` (geometry units) of the curve. Anchors keep their attributes, the
// points in between lerp them along the segment, handles disappear. Polyline primitives pass through untouched.
Geometry flattenCurves(const Geometry& in, float tolerance = 0.25f);

// The polyline operations below flatten curves first.
Geometry resamplePolylines(const Geometry& in, size_t count, float spacing);
Geometry subdividePolylines(const Geometry& in, int iterations);
Geometry copyToPoints(const Geometry& source, const Geometry& targets, float scale, float angle);
Geometry blast(const Geometry& in, AttribClass cls, const std::string& group, bool keepGroup);

}  // namespace jitnodes
