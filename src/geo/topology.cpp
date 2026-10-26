#include "jitnodes/geo/topology.h"

#include <algorithm>
#include <cmath>

namespace jitnodes {

namespace {

float distance(const float* x, const float* y, uint32_t a, uint32_t b) { return std::hypot(x[b] - x[a], y[b] - y[a]); }

void copyPoints(const Primitive& p, std::vector<PointSample>& samples, std::vector<Primitive>& prims) {
    const uint32_t start = static_cast<uint32_t>(samples.size());
    for (uint32_t j = 0; j < p.count; ++j) samples.push_back({p.start + j, p.start + j, 0.0f});
    prims.push_back({start, p.count, p.closed});
}

Geometry finish(const Geometry& in, const std::vector<PointSample>& samples, const std::vector<Primitive>& prims,
                const std::vector<uint32_t>& srcPrim) {
    Geometry out = gatherPoints(in, samples);
    for (const Primitive& p : prims) out.addPrimitive(p);
    transferPrimAttribs(in, out, srcPrim);
    return out;
}

}  // namespace

Geometry gatherPoints(const Geometry& src, const std::vector<PointSample>& samples) {
    Geometry dst;
    dst.resizePoints(samples.size());
    for (const auto& [name, array] : src.attribs(AttribClass::point)) {
        AttribArray& out = dst.attribLike(AttribClass::point, name, array);
        if (array.type() == AttribType::float32) {
            const float* s = array.floats().data();
            float* o = out.floats().data();
            for (size_t i = 0; i < samples.size(); ++i) {
                const PointSample& p = samples[i];
                o[i] = s[p.a] + (s[p.b] - s[p.a]) * p.t;
            }
        } else {
            const int32_t* s = array.ints().data();
            int32_t* o = out.ints().data();
            for (size_t i = 0; i < samples.size(); ++i) o[i] = samples[i].t < 0.5f ? s[samples[i].a] : s[samples[i].b];
        }
    }
    for (const auto& [name, value] : src.details()) dst.setDetail(name, value);
    for (const auto& [name, array] : src.attribs(AttribClass::primitive)) dst.attribLike(AttribClass::primitive, name, array);
    return dst;
}

void transferPrimAttribs(const Geometry& src, Geometry& dst, const std::vector<uint32_t>& srcIndex) {
    for (const auto& [name, array] : src.attribs(AttribClass::primitive)) {
        AttribArray& out = dst.attribLike(AttribClass::primitive, name, array);
        for (size_t i = 0; i < srcIndex.size(); ++i) {
            if (array.type() == AttribType::float32) out.floats()[i] = array.floats()[srcIndex[i]];
            else out.ints()[i] = array.ints()[srcIndex[i]];
        }
    }
}

bool hasCurves(const Geometry& in) {
    for (const Primitive& p : in.primitives())
        if (cubicSegments(p) > 0) return true;
    return false;
}

Geometry flattenCurves(const Geometry& in, float tolerance) {
    if (!hasCurves(in)) return in;
    const float* x = in.x();
    const float* y = in.y();
    tolerance = std::max(tolerance, 1e-4f);

    struct At {
        uint32_t first = 0;   // the cubic segment the sample lies on: its first control point and its end anchor
        uint32_t end = 0;
        float t = 0.0f;
        bool curve = false;   // false for the points of a polyline primitive, which keep their position
    };
    std::vector<PointSample> samples;
    std::vector<At> at;
    std::vector<Primitive> prims;
    std::vector<uint32_t> srcPrim;

    for (uint32_t pi = 0; pi < in.primitiveCount(); ++pi) {
        const Primitive& p = in.primitives()[pi];
        srcPrim.push_back(pi);
        const uint32_t segments = cubicSegments(p);
        if (segments == 0) {
            copyPoints(p, samples, prims);
            prims.back().kind = PrimKind::polyline;
            at.resize(samples.size());
            continue;
        }
        const uint32_t start = static_cast<uint32_t>(samples.size());
        for (uint32_t s = 0; s < segments; ++s) {
            const uint32_t i0 = p.start + 3 * s;
            const uint32_t i3 = p.start + (3 * s + 3) % p.count;
            const uint32_t i1 = i0 + 1, i2 = i0 + 2;
            // |B''| <= 6 * dd, and a chord of n pieces is off by at most |B''| / (8 n^2)
            const float dd = std::max(std::hypot(x[i0] - 2 * x[i1] + x[i2], y[i0] - 2 * y[i1] + y[i2]),
                                      std::hypot(x[i1] - 2 * x[i2] + x[i3], y[i1] - 2 * y[i2] + y[i3]));
            const int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(0.75f * dd / tolerance))), 1, 64);
            for (int k = 0; k < n; ++k) {
                const float t = static_cast<float>(k) / static_cast<float>(n);
                samples.push_back({i0, i3, t});
                at.push_back({i0, i3, t, true});
            }
        }
        if (!p.closed) {
            const uint32_t last = p.start + p.count - 1;
            samples.push_back({last, last, 0.0f});
            at.push_back({last - 3, last, 1.0f, true});
        }
        prims.push_back({start, static_cast<uint32_t>(samples.size()) - start, p.closed, PrimKind::polyline});
    }

    Geometry out = gatherPoints(in, samples);
    float* ox = out.x();
    float* oy = out.y();
    for (size_t i = 0; i < samples.size(); ++i) {
        if (!at[i].curve) continue;
        const uint32_t i0 = at[i].first, i1 = i0 + 1, i2 = i0 + 2, i3 = at[i].end;
        const float t = at[i].t, u = 1.0f - t;
        const float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
        ox[i] = b0 * x[i0] + b1 * x[i1] + b2 * x[i2] + b3 * x[i3];
        oy[i] = b0 * y[i0] + b1 * y[i1] + b2 * y[i2] + b3 * y[i3];
    }
    for (const Primitive& p : prims) out.addPrimitive(p);
    transferPrimAttribs(in, out, srcPrim);
    return out;
}

Geometry resamplePolylines(const Geometry& in, size_t count, float spacing) {
    if (hasCurves(in)) return resamplePolylines(flattenCurves(in), count, spacing);
    const float* x = in.x();
    const float* y = in.y();
    std::vector<PointSample> samples;
    std::vector<Primitive> prims;
    std::vector<uint32_t> srcPrim;

    for (uint32_t pi = 0; pi < in.primitiveCount(); ++pi) {
        const Primitive& p = in.primitives()[pi];
        srcPrim.push_back(pi);
        if (p.count < 2) {
            copyPoints(p, samples, prims);
            continue;
        }
        const uint32_t segments = p.closed ? p.count : p.count - 1;
        std::vector<float> cumulative(segments + 1, 0.0f);
        for (uint32_t s = 0; s < segments; ++s)
            cumulative[s + 1] = cumulative[s] + distance(x, y, p.start + s, p.start + (s + 1) % p.count);
        const float total = cumulative.back();

        size_t n = count;
        if (spacing > 0.0f) n = static_cast<size_t>(std::ceil(total / spacing)) + (p.closed ? 0 : 1);
        n = std::max<size_t>(n, p.closed ? 3 : 2);
        const float step = total / static_cast<float>(p.closed ? n : n - 1);

        const uint32_t start = static_cast<uint32_t>(samples.size());
        uint32_t s = 0;
        for (size_t i = 0; i < n; ++i) {
            const float d = (!p.closed && i + 1 == n) ? total : static_cast<float>(i) * step;
            while (s + 1 < segments && cumulative[s + 1] < d) ++s;
            const float len = cumulative[s + 1] - cumulative[s];
            const float t = len > 0.0f ? std::clamp((d - cumulative[s]) / len, 0.0f, 1.0f) : 0.0f;
            samples.push_back({p.start + s, p.start + (s + 1) % p.count, t});
        }
        prims.push_back({start, static_cast<uint32_t>(n), p.closed});
    }
    return finish(in, samples, prims, srcPrim);
}

namespace {

Geometry subdivideOnce(const Geometry& in) {
    std::vector<PointSample> samples;
    std::vector<Primitive> prims;
    std::vector<uint32_t> srcPrim;

    for (uint32_t pi = 0; pi < in.primitiveCount(); ++pi) {
        const Primitive& p = in.primitives()[pi];
        srcPrim.push_back(pi);
        if (p.count < 2) {
            copyPoints(p, samples, prims);
            continue;
        }
        const uint32_t start = static_cast<uint32_t>(samples.size());
        const uint32_t segments = p.closed ? p.count : p.count - 1;
        if (!p.closed) samples.push_back({p.start, p.start, 0.0f});
        for (uint32_t s = 0; s < segments; ++s) {
            const uint32_t a = p.start + s;
            const uint32_t b = p.start + (s + 1) % p.count;
            const bool first = !p.closed && s == 0;
            const bool last = !p.closed && s + 1 == segments;
            if (!first) samples.push_back({a, b, 0.25f});
            if (!last) samples.push_back({a, b, 0.75f});
        }
        if (!p.closed) samples.push_back({p.start + p.count - 1, p.start + p.count - 1, 0.0f});
        prims.push_back({start, static_cast<uint32_t>(samples.size()) - start, p.closed});
    }
    return finish(in, samples, prims, srcPrim);
}

}  // namespace

Geometry subdividePolylines(const Geometry& in, int iterations) {
    if (hasCurves(in)) return subdividePolylines(flattenCurves(in), iterations);
    if (iterations <= 0) return in;
    Geometry current = subdivideOnce(in);
    for (int i = 1; i < iterations; ++i) current = subdivideOnce(current);
    return current;
}

Geometry copyToPoints(const Geometry& source, const Geometry& targets, float scale, float angle) {
    const size_t srcCount = source.pointCount();
    const size_t targetCount = targets.pointCount();

    std::vector<PointSample> samples;
    samples.reserve(srcCount * targetCount);
    for (size_t t = 0; t < targetCount; ++t)
        for (uint32_t i = 0; i < srcCount; ++i) samples.push_back({i, i, 0.0f});

    Geometry out = gatherPoints(source, samples);
    std::vector<uint32_t> srcPrim;
    for (size_t t = 0; t < targetCount; ++t) {
        for (uint32_t pi = 0; pi < source.primitiveCount(); ++pi) {
            Primitive p = source.primitives()[pi];
            p.start += static_cast<uint32_t>(t * srcCount);
            out.addPrimitive(p);
            srcPrim.push_back(pi);
        }
    }
    transferPrimAttribs(source, out, srcPrim);

    const AttribArray* scaleAttr = targets.findAttrib(AttribClass::point, "scale");
    const AttribArray* angleAttr = targets.findAttrib(AttribClass::point, "angle");
    int32_t* pointIndex = out.attrib(AttribClass::point, "copyIndex", AttribType::int32).ints().data();
    float* ox = out.x();
    float* oy = out.y();
    const float* tx = targets.x();
    const float* ty = targets.y();
    for (size_t t = 0; t < targetCount; ++t) {
        const float s = scale * (scaleAttr ? scaleAttr->at(t) : 1.0f);
        const float a = angle + (angleAttr ? angleAttr->at(t) : 0.0f);
        const float c = std::cos(a);
        const float sn = std::sin(a);
        for (size_t i = 0; i < srcCount; ++i) {
            const size_t k = t * srcCount + i;
            const float px = ox[k];
            const float py = oy[k];
            ox[k] = s * (px * c - py * sn) + tx[t];
            oy[k] = s * (px * sn + py * c) + ty[t];
            pointIndex[k] = static_cast<int32_t>(t);
        }
    }
    int32_t* primIndex = out.attrib(AttribClass::primitive, "copyIndex", AttribType::int32).ints().data();
    for (size_t i = 0; i < srcPrim.size(); ++i) primIndex[i] = static_cast<int32_t>(i / std::max<size_t>(source.primitiveCount(), 1));
    return out;
}

Geometry blast(const Geometry& in, AttribClass cls, const std::string& group, bool keepGroup) {
    if (hasCurves(in)) return blast(flattenCurves(in), cls, group, keepGroup);
    const size_t n = in.pointCount();
    std::vector<char> keepPoint(n, 1);
    std::vector<char> keepPrim(in.primitiveCount(), 1);

    if (cls == AttribClass::point) {
        for (size_t i = 0; i < n; ++i) keepPoint[i] = in.inGroup(AttribClass::point, group, i) == keepGroup;
    } else {
        for (size_t pi = 0; pi < in.primitiveCount(); ++pi) {
            keepPrim[pi] = in.inGroup(AttribClass::primitive, group, pi) == keepGroup;
            if (keepPrim[pi]) continue;
            const Primitive& p = in.primitives()[pi];
            for (uint32_t j = 0; j < p.count; ++j) keepPoint[p.start + j] = 0;
        }
    }

    std::vector<uint32_t> remap(n, 0);
    std::vector<PointSample> samples;
    for (uint32_t i = 0; i < n; ++i) {
        if (!keepPoint[i]) continue;
        remap[i] = static_cast<uint32_t>(samples.size());
        samples.push_back({i, i, 0.0f});
    }

    Geometry out = gatherPoints(in, samples);
    std::vector<uint32_t> srcPrim;
    for (uint32_t pi = 0; pi < in.primitiveCount(); ++pi) {
        if (!keepPrim[pi]) continue;
        const Primitive& p = in.primitives()[pi];
        uint32_t survivors = 0, first = 0;
        for (uint32_t j = 0; j < p.count; ++j) {
            if (!keepPoint[p.start + j]) continue;
            if (survivors++ == 0) first = remap[p.start + j];
        }
        if (survivors == 0) continue;
        out.addPrimitive({first, survivors, p.closed && survivors == p.count});
        srcPrim.push_back(pi);
    }
    transferPrimAttribs(in, out, srcPrim);
    return out;
}

}  // namespace jitnodes
