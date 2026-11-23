#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "jitnodes/geo/pathMotion.h"
#include "jitnodes/geo/topology.h"
#include "jitnodes/graph/network.h"

namespace jitnodes {

namespace {

std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> words;
    std::string item;
    for (char c : text + " ") {
        if (c == ' ' || c == ',' || c == ';' || c == '\n' || c == '\t') {
            if (!item.empty()) {
                words.push_back(item);
            }
            item.clear();
        } else {
            item.push_back(c);
        }
    }
    return words;
}

// `*` matches any run of characters, everything else matches itself.
bool matchesPattern(const std::string& pattern, const std::string& name) {
    size_t p = 0, n = 0, star = std::string::npos, resume = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = n;
        } else if (p < pattern.size() && pattern[p] == name[n]) {
            ++p;
            ++n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

bool matchesAny(const std::vector<std::string>& patterns, const std::string& name) {
    for (const std::string& pattern : patterns) {
        if (matchesPattern(pattern, name)) {
            return true;
        }
    }
    return false;
}

// Recomputes the motion attributes of a stroke (arc length, smoothed velocity, acceleration, heading) from its positions and the sample times in
// `timeAttr`, so the attributes follow the points after the stroke was smoothed or reshaped. See PathMotion for the definitions.
class PathMotionNode : public Node {
public:
    PathMotionNode() : Node("pathMotion", 1) {
        declareString("timeAttr", "t");
        declareFloat("smoothing", 0.3f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        auto geo = std::make_shared<Geometry>(*in[0]);
        PathMotionParams params;
        params.timeAttr = getString("timeAttr");
        params.smoothing = getFloat("smoothing");
        PathMotion::compute(*geo, params);
        return geo;
    }
};

// Trims a stroke: removes point attributes (`remove` names, `keep` the only ones to stay; x, y and the primitive attributes always stay) and
// drops points that carry little: closer than `minDistance` to the last kept point, less than `minTime` after it, or within `simplify` of the line
// between their neighbours (Ramer-Douglas-Peucker). A dropped point takes its attribute values with it. Primitives with fewer than three points
// and cubic primitives are copied whole.
class StrokeCleanNode : public Node {
public:
    StrokeCleanNode() : Node("strokeClean", 1) {
        declareString("remove", "");
        declareString("keep", "");
        declareString("timeAttr", "t");
        declareFloat("simplify", 0.0f);
        declareFloat("minDistance", 0.0f);
        declareFloat("minTime", 0.0f);
        declareFloat("keepEnds", 1.0f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        const Geometry& src = *in[0];
        const float simplify = getFloat("simplify"), minDistance = getFloat("minDistance"), minTime = getFloat("minTime");
        if (simplify < 0.0f || minDistance < 0.0f || minTime < 0.0f) {
            throw std::runtime_error("strokeClean: simplify, minDistance and minTime must not be negative");
        }
        const bool keepEnds = getFloat("keepEnds") != 0.0f;
        const std::vector<std::string> removeNames = splitWords(getString("remove"));
        const std::vector<std::string> keepNames = splitWords(getString("keep"));

        const bool filters = minDistance > 0.0f || minTime > 0.0f || simplify > 0.0f;
        const AlignedArray<float>* time = minTime > 0.0f ? src.findPointAttrib(getString("timeAttr")) : nullptr;
        if (minTime > 0.0f && time == nullptr) {
            throw std::runtime_error("strokeClean: the points have no '" + getString("timeAttr") + "' attribute for minTime");
        }

        std::shared_ptr<Geometry> out;
        if (filters) {
            out = std::make_shared<Geometry>(thinned(src, simplify, minDistance, minTime, keepEnds, time));
        } else {
            out = std::make_shared<Geometry>(src);
        }

        std::vector<std::string> doomed;
        for (const auto& entry : out->attribs(AttribClass::point)) {
            const std::string& name = entry.first;
            if (name == "x" || name == "y") {
                continue;
            }
            if (matchesAny(removeNames, name) || (!keepNames.empty() && !matchesAny(keepNames, name))) {
                doomed.push_back(name);
            }
        }
        for (const std::string& name : doomed) {
            out->removeAttrib(AttribClass::point, name);
        }
        return out;
    }

private:
    static float segmentDistance(float px, float py, float ax, float ay, float bx, float by) {
        const float dx = bx - ax, dy = by - ay;
        const float length2 = dx * dx + dy * dy;
        if (length2 <= 0.0f) {
            return std::hypot(px - ax, py - ay);
        }
        const float t = std::clamp(((px - ax) * dx + (py - ay) * dy) / length2, 0.0f, 1.0f);
        return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
    }

    // The kept point indices of one primitive, ascending.
    static std::vector<uint32_t> keptPoints(const Geometry& src, const Primitive& prim, float simplify, float minDistance, float minTime, bool keepEnds,
                                            const AlignedArray<float>* time) {
        const float* x = src.x();
        const float* y = src.y();
        const uint32_t first = prim.start, last = prim.start + prim.count - 1;

        std::vector<uint32_t> kept{first};
        for (uint32_t i = first + 1; i <= last; ++i) {
            const uint32_t previous = kept.back();
            const bool close = std::hypot(x[i] - x[previous], y[i] - y[previous]) < minDistance;
            const bool soon = time != nullptr && (*time)[i] - (*time)[previous] < minTime;
            const bool isLast = i == last;
            if ((close || soon) && !(isLast && keepEnds)) {
                continue;
            }
            if (isLast && keepEnds && (close || soon) && kept.size() > 1) {
                kept.back() = i;
                continue;
            }
            kept.push_back(i);
        }

        if (simplify <= 0.0f || kept.size() < 3) {
            return kept;
        }
        std::vector<char> keep(kept.size(), 0);
        keep.front() = keep.back() = 1;
        std::vector<std::pair<size_t, size_t>> spans{{0, kept.size() - 1}};
        while (!spans.empty()) {
            const auto [a, b] = spans.back();
            spans.pop_back();
            float worst = -1.0f;
            size_t at = a;
            for (size_t k = a + 1; k < b; ++k) {
                const float d = segmentDistance(x[kept[k]], y[kept[k]], x[kept[a]], y[kept[a]], x[kept[b]], y[kept[b]]);
                if (d > worst) {
                    worst = d;
                    at = k;
                }
            }
            if (worst > simplify) {
                keep[at] = 1;
                spans.push_back({a, at});
                spans.push_back({at, b});
            }
        }
        std::vector<uint32_t> result;
        for (size_t k = 0; k < kept.size(); ++k) {
            if (keep[k]) {
                result.push_back(kept[k]);
            }
        }
        return result;
    }

    static Geometry thinned(const Geometry& src, float simplify, float minDistance, float minTime, bool keepEnds, const AlignedArray<float>* time) {
        std::vector<PointSample> samples;
        std::vector<Primitive> prims;
        std::vector<uint32_t> srcPrim;
        for (uint32_t pi = 0; pi < src.primitiveCount(); ++pi) {
            const Primitive& prim = src.primitives()[pi];
            Primitive made = prim;
            made.start = static_cast<uint32_t>(samples.size());
            if (prim.count < 3 || prim.kind != PrimKind::polyline) {
                for (uint32_t k = 0; k < prim.count; ++k) {
                    samples.push_back({prim.start + k, prim.start + k, 0.0f});
                }
            } else {
                for (uint32_t index : keptPoints(src, prim, simplify, minDistance, minTime, keepEnds, time)) {
                    samples.push_back({index, index, 0.0f});
                }
            }
            made.count = static_cast<uint32_t>(samples.size()) - made.start;
            prims.push_back(made);
            srcPrim.push_back(pi);
        }
        Geometry out = gatherPoints(src, samples);
        for (const Primitive& prim : prims) {
            out.addPrimitive(prim);
        }
        transferPrimAttribs(src, out, srcPrim);
        return out;
    }
};

// A calligraphic pen: every path (polyline or cubic) is swept by a slanted oval nib and comes out as closed outlines to fill. `width` is the nib's
// long diameter, `angle` its tilt in degrees (counter-clockwise from horizontal), `roundness` its thickness as a percentage of its length (100 is a
// round pen). The point attribute named by `scaleAttr` (pen pressure `w`, when the points have it) scales the nib along the stroke. The outlines are
// one convex piece per step of the path, all wound alike, so filling them non-zero is their union; they keep the shape key of their path. The point
// attributes are dropped: the result is plain polygons.
class CalligraphyNode : public Node {
public:
    CalligraphyNode() : Node("calligraphy", 1) {
        declareFloat("width", 8.0f);
        declareFloat("angle", 45.0f);
        declareFloat("roundness", 25.0f);
        declareString("scaleAttr", "w");
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        const Geometry& src = *in[0];
        const float width = getFloat("width"), roundness = getFloat("roundness");
        if (!(width > 0.0f) || !(roundness > 0.0f) || roundness > 100.0f) {
            throw std::runtime_error("calligraphy: width must be above 0 and roundness in 0..100");
        }
        const double major = width * 0.5, minor = std::max(major * roundness / 100.0, 0.05);
        const double turn = -double(getFloat("angle")) * 3.14159265358979323846 / 180.0, c = std::cos(turn), s = std::sin(turn);
        constexpr int kSides = 16;
        std::vector<std::pair<double, double>> nib;
        for (int k = 0; k < kSides; ++k) {
            const double t = 2.0 * 3.14159265358979323846 * k / kSides, x = major * std::cos(t), y = minor * std::sin(t);
            nib.push_back({x * c - y * s, x * s + y * c});
        }

        const AlignedArray<float>* scale = src.findPointAttrib(getString("scaleAttr"));
        bool scaled = false;
        if (scale != nullptr) {
            for (uint32_t i = 0; i < src.pointCount() && !scaled; ++i) {
                scaled = (*scale)[i] > 0.0f;
            }
        }

        std::vector<float> xs, ys;
        std::vector<Primitive> prims;
        std::vector<uint32_t> srcPrim;
        for (uint32_t pi = 0; pi < src.primitiveCount(); ++pi) {
            std::vector<Sample> samples = flatten(src, src.primitives()[pi], scaled ? scale : nullptr);
            if (samples.size() < 2) {
                continue;
            }
            Sample last = samples.front();
            for (size_t i = 1; i < samples.size(); ++i) {
                const Sample& next = samples[i];
                if (std::hypot(next.x - last.x, next.y - last.y) < 0.05 && i + 1 < samples.size()) {
                    continue;
                }
                std::vector<std::pair<double, double>> both;
                for (const auto& p : nib) {
                    both.push_back({p.first * last.scale + last.x, p.second * last.scale + last.y});
                    both.push_back({p.first * next.scale + next.x, p.second * next.scale + next.y});
                }
                const std::vector<std::pair<double, double>> hull = convexHull(std::move(both));
                if (hull.size() >= 3) {
                    Primitive made;
                    made.start = static_cast<uint32_t>(xs.size());
                    made.count = static_cast<uint32_t>(hull.size());
                    made.closed = true;
                    for (const auto& p : hull) {
                        xs.push_back(static_cast<float>(p.first));
                        ys.push_back(static_cast<float>(p.second));
                    }
                    prims.push_back(made);
                    srcPrim.push_back(pi);
                }
                last = next;
            }
        }

        auto out = std::make_shared<Geometry>(gatherPoints(src, {}));
        std::vector<std::string> extra;
        for (const auto& entry : out->attribs(AttribClass::point)) {
            if (entry.first != "x" && entry.first != "y") {
                extra.push_back(entry.first);
            }
        }
        for (const std::string& name : extra) {
            out->removeAttrib(AttribClass::point, name);
        }
        out->resizePoints(xs.size());
        for (size_t i = 0; i < xs.size(); ++i) {
            out->x()[i] = xs[i];
            out->y()[i] = ys[i];
        }
        for (const Primitive& prim : prims) {
            out->addPrimitive(prim);
        }
        transferPrimAttribs(src, *out, srcPrim);
        return out;
    }

private:
    struct Sample {
        double x = 0.0, y = 0.0, scale = 1.0;
    };

    // The path as points `steps` apart along each cubic segment, each with the nib scale (interpolated between the anchors of a segment).
    static std::vector<Sample> flatten(const Geometry& src, const Primitive& prim, const AlignedArray<float>* scale) {
        std::vector<Sample> out;
        if (prim.count < 2) {
            return out;
        }
        const float* x = src.x();
        const float* y = src.y();
        const auto at = [&](uint32_t k) {
            const uint32_t i = prim.start + k % prim.count;
            return Sample{x[i], y[i], scale != nullptr ? std::max(double((*scale)[i]), 0.02) : 1.0};
        };
        const uint32_t segments = cubicSegments(prim);
        if (prim.kind == PrimKind::cubic && segments > 0) {
            constexpr int kSteps = 12;
            out.push_back(at(0));
            for (uint32_t seg = 0; seg < segments; ++seg) {
                const Sample p0 = at(seg * 3), p1 = at(seg * 3 + 1), p2 = at(seg * 3 + 2), p3 = at(seg * 3 + 3);
                for (int st = 1; st <= kSteps; ++st) {
                    const double t = double(st) / kSteps, u = 1.0 - t;
                    const double b0 = u * u * u, b1 = 3.0 * u * u * t, b2 = 3.0 * u * t * t, b3 = t * t * t;
                    out.push_back({b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x, b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y,
                                   p0.scale + (p3.scale - p0.scale) * t});
                }
            }
            return out;
        }
        for (uint32_t k = 0; k < prim.count; ++k) {
            out.push_back(at(k));
        }
        if (prim.closed) {
            out.push_back(at(0));
        }
        return out;
    }

    static double cross(const std::pair<double, double>& o, const std::pair<double, double>& a, const std::pair<double, double>& b) {
        return (a.first - o.first) * (b.second - o.second) - (a.second - o.second) * (b.first - o.first);
    }

    // Andrew's monotone chain: always wound the same way.
    static std::vector<std::pair<double, double>> convexHull(std::vector<std::pair<double, double>> pts) {
        std::sort(pts.begin(), pts.end());
        std::vector<std::pair<double, double>> hull(pts.size() * 2);
        size_t k = 0;
        for (size_t i = 0; i < pts.size(); ++i) {
            while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0.0) {
                --k;
            }
            hull[k++] = pts[i];
        }
        for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
            while (k >= t && cross(hull[k - 2], hull[k - 1], pts[i - 1]) <= 0.0) {
                --k;
            }
            hull[k++] = pts[i - 1];
        }
        hull.resize(k > 1 ? k - 1 : k);
        return hull;
    }
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerStrokeNodes() {
    add<PathMotionNode>("pathMotion");
    add<StrokeCleanNode>("strokeClean");
    add<CalligraphyNode>("calligraphy");
}

}  // namespace jitnodes
