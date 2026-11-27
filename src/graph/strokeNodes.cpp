#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "jitnodes/geo/brushEnvelope.h"
#include "jitnodes/geo/curveFit.h"
#include "jitnodes/geo/nibBrush.h"
#include "jitnodes/geo/outlineFit.h"
#include "jitnodes/geo/pathMotion.h"
#include "jitnodes/geo/trajectoryFit.h"
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
// between their neighbours (Ramer-Douglas-Peucker). `simplify` also keeps a point where an attribute named by `follow` differs by more than
// `followTolerance` from a straight change between the neighbours. A dropped point takes its attribute values with it. Primitives with fewer than
// three points and cubic primitives are copied whole.
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
        declareString("follow", "w");
        declareFloat("followTolerance", 0.05f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        const Geometry& src = *in[0];
        const float simplify = getFloat("simplify"), minDistance = getFloat("minDistance"), minTime = getFloat("minTime");
        const float followTolerance = getFloat("followTolerance");
        if (simplify < 0.0f || minDistance < 0.0f || minTime < 0.0f || followTolerance < 0.0f) {
            throw std::runtime_error("strokeClean: simplify, minDistance, minTime and followTolerance must not be negative");
        }
        std::vector<const AlignedArray<float>*> followed;
        const std::vector<std::string> followNames = splitWords(getString("follow"));
        for (const auto& entry : src.attribs(AttribClass::point)) {
            if (followTolerance > 0.0f && entry.first != "x" && entry.first != "y" && matchesAny(followNames, entry.first)) {
                if (const AlignedArray<float>* values = src.findPointAttrib(entry.first)) {
                    followed.push_back(values);
                }
            }
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
            out = std::make_shared<Geometry>(thinned(src, simplify, minDistance, minTime, keepEnds, time, followed, followTolerance));
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
                                            const AlignedArray<float>* time, const std::vector<const AlignedArray<float>*>& followed,
                                            float followTolerance) {
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
        // Path length along the kept points: what a brush spreads an attribute by.
        std::vector<float> along(kept.size(), 0.0f);
        for (size_t k = 1; k < kept.size(); ++k) {
            along[k] = along[k - 1] + std::hypot(x[kept[k]] - x[kept[k - 1]], y[kept[k]] - y[kept[k - 1]]);
        }
        while (!spans.empty()) {
            const auto [a, b] = spans.back();
            spans.pop_back();
            float worst = -1.0f;   // in tolerances: above 1 the point has to stay
            size_t at = a;
            for (size_t k = a + 1; k < b; ++k) {
                float off = segmentDistance(x[kept[k]], y[kept[k]], x[kept[a]], y[kept[a]], x[kept[b]], y[kept[b]]) / simplify;
                const float t = along[b] > along[a] ? (along[k] - along[a]) / (along[b] - along[a]) : 0.0f;
                for (const AlignedArray<float>* values : followed) {
                    const float from = (*values)[kept[a]], to = (*values)[kept[b]];
                    off = std::max(off, std::abs((*values)[kept[k]] - (from + (to - from) * t)) / followTolerance);
                }
                if (off > worst) {
                    worst = off;
                    at = k;
                }
            }
            if (worst > 1.0f) {
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

    static Geometry thinned(const Geometry& src, float simplify, float minDistance, float minTime, bool keepEnds, const AlignedArray<float>* time,
                            const std::vector<const AlignedArray<float>*>& followed, float followTolerance) {
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
                for (uint32_t index : keptPoints(src, prim, simplify, minDistance, minTime, keepEnds, time, followed, followTolerance)) {
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

struct PathSample {
    double x = 0.0, y = 0.0, scale = 1.0;
};

// The path as points `steps` apart along each cubic segment, each with a scale (interpolated between the anchors of a segment).
std::vector<PathSample> flatten(const Geometry& src, const Primitive& prim, const AlignedArray<float>* scale) {
    std::vector<PathSample> out;
    if (prim.count < 2) {
        return out;
    }
    const float* x = src.x();
    const float* y = src.y();
    const auto at = [&](uint32_t k) {
        const uint32_t i = prim.start + k % prim.count;
        return PathSample{x[i], y[i], scale != nullptr ? std::max(double((*scale)[i]), 0.02) : 1.0};
    };
    const uint32_t segments = cubicSegments(prim);
    if (prim.kind == PrimKind::cubic && segments > 0) {
        constexpr int kSteps = 12;
        out.push_back(at(0));
        for (uint32_t seg = 0; seg < segments; ++seg) {
            const PathSample p0 = at(seg * 3), p1 = at(seg * 3 + 1), p2 = at(seg * 3 + 2), p3 = at(seg * 3 + 3);
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

// The samples where a drawn-as-given polyline (a rectangle, a polygon: not a recorded stroke) has a corner: it turns 30 degrees or more between
// two edges of at least `shortest`. A closed one repeats its first sample at the end, so that sample can be a corner too.
std::vector<unsigned> authoredCorners(const std::vector<PathSample>& samples, bool closed, double shortest) {
    std::vector<unsigned> corners;
    if (samples.size() < 3) {
        return corners;
    }
    const std::size_t last = samples.size() - 1;
    for (std::size_t i = closed ? 0 : 1; i < last; ++i) {
        const PathSample& before = samples[i == 0 ? last - 1 : i - 1];
        const PathSample& here = samples[i];
        const PathSample& after = samples[i + 1];
        const double inX = here.x - before.x, inY = here.y - before.y, outX = after.x - here.x, outY = after.y - here.y;
        if (std::hypot(inX, inY) < shortest || std::hypot(outX, outY) < shortest) {
            continue;
        }
        if (std::abs(std::atan2(inX * outY - inY * outX, inX * outX + inY * outY)) >= 3.14159265358979 / 6.0) {
            corners.push_back(static_cast<unsigned>(i));
        }
    }
    return corners;
}

// 0 <= value < 1, the same for the same key, so scattered copies do not shimmer when the graph is cooked again.
double scatterNoise(uint32_t key) {
    key *= 2654435761u;
    key ^= key >> 15;
    key *= 2246822519u;
    key ^= key >> 13;
    key *= 3266489917u;
    key ^= key >> 16;
    return double(key & 0xFFFFFFu) / 16777216.0;
}

// A calligraphic pen: every path (polyline or cubic) is swept by a slanted oval nib and comes out as one closed cubic outline to fill (a closed path
// gives two, a ring: the outer and the inner loop, wound opposite ways under the one shape key so the inside stays open). `width` is the nib's long
// diameter, `angle` its tilt in degrees (counter-clockwise from horizontal), `roundness` its thickness as a percentage of its length (100 is a round
// pen). The point attribute named by `scaleAttr` (pen pressure `w`, when the points have it) scales the nib along the stroke. The path is first
// fitted to cubics within `trajectoryTolerance` of its samples (0: a quarter of the width, at most 1; a mouse path is a coarse polyline, and following every corner of it costs a cubic or two per sample), the nib is swept along the fitted curve, and
// the border of the sweep is fitted to cubics within `outlineTolerance` (0: 7.5% of the width or 30% of the nib's thickness at the stroke's thinnest scale (at least 0.3), whichever is less, between 0.05 and 0.6). The outlines keep the shape
// key of their path; the point attributes are dropped. See TrajectoryFit, BrushEnvelope and OutlineFit.
class CalligraphyNode : public Node {
public:
    CalligraphyNode() : Node("calligraphy", 1) {
        declareFloat("width", 8.0f);
        declareFloat("angle", 45.0f);
        declareFloat("roundness", 25.0f);
        declareString("scaleAttr", "w");
        declareFloat("trajectoryTolerance", 0.0f);
        declareFloat("outlineTolerance", 0.0f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        const Geometry& src = *in[0];
        const float width = getFloat("width"), roundness = getFloat("roundness");
        if (!(width > 0.0f) || !(roundness > 0.0f) || roundness > 100.0f) {
            throw std::runtime_error("calligraphy: width must be above 0 and roundness in 0..100");
        }
        const NibBrush nib = NibBrush::oval(width, getFloat("angle"), roundness, NibBrush::sidesFor(width));
        TrajectoryParams trajectoryParams;
        trajectoryParams.forward = true;
        trajectoryParams.tolerance =
            getFloat("trajectoryTolerance") > 0.0f ? double(getFloat("trajectoryTolerance")) : std::clamp(double(width) / 4.0, 0.05, 1.0);
        const double thickness = double(width) * double(roundness) / 100.0;
        const double outlineTolerance =
            getFloat("outlineTolerance") > 0.0f ? double(getFloat("outlineTolerance")) : std::max(std::min(double(width) * 0.075, thickness * 0.3), 0.05);

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
            const Primitive& path = src.primitives()[pi];
            const std::vector<PathSample> samples = flatten(src, path, scaled ? scale : nullptr);
            if (samples.size() < 2) {
                continue;
            }
            std::vector<CurveFit::Point> points;
            std::vector<double> scales;
            for (const PathSample& s : samples) {
                points.push_back({s.x, s.y});
                scales.push_back(s.scale);
            }
            std::vector<std::vector<double>> attributes;
            if (scaled) {
                attributes.push_back(scales);
            }
            // Two borders closer than the tolerance fit into one and the stroke drops out, so the automatic tolerance follows the stroke's thinnest scale.
            const double thinnest = scaled ? *std::min_element(scales.begin(), scales.end()) : 1.0;
            const double pathTolerance = getFloat("outlineTolerance") > 0.0f
                                             ? outlineTolerance
                                             : std::clamp(std::min(double(width) * 0.075, thickness * std::clamp(thinnest, 0.3, 1.0) * 0.3), 0.05, 0.6);
            // A scale change this small moves a border by half the outline tolerance.
            trajectoryParams.attributeTolerance = pathTolerance / double(width);
            // A recorded stroke carries the time of each sample; its corners are found by the fit, which smooths the jitter of a hand. The
            // vertices of any other polyline are meant as they are.
            const std::vector<unsigned> corners = path.kind == PrimKind::polyline && src.findPointAttrib("t") == nullptr
                                                      ? authoredCorners(samples, path.closed, 2.0 * trajectoryParams.tolerance)
                                                      : std::vector<unsigned>();
            trajectoryParams.closed = path.closed && (corners.empty() || corners.front() != 0);
            const Trajectory trajectory = TrajectoryFit::fit(points, attributes, trajectoryParams, corners);
            const BrushBorders borders = BrushEnvelope::sweep(nib, trajectory.vertices, scaled ? trajectory.attributes[0] : std::vector<double>(), path.closed);
            for (const std::vector<CurveFit::Point>& outline : OutlineFit::outlines(borders, pathTolerance)) {
                Primitive made;
                made.start = static_cast<uint32_t>(xs.size());
                made.count = static_cast<uint32_t>(outline.size());
                made.closed = true;
                made.kind = PrimKind::cubic;
                for (const CurveFit::Point& p : outline) {
                    xs.push_back(static_cast<float>(p.x));
                    ys.push_back(static_cast<float>(p.y));
                }
                prims.push_back(made);
                srcPrim.push_back(pi);
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
};

// A scatter brush: copies of the artwork (input 1) stand along the paths of input 0. `size` is the stroke weight (1 draws the artwork at its own
// size), `spacing` the distance between copies as a percentage of the artwork width (100: side by side), `scatter` how far a copy strays from the
// path as a percentage of the artwork height, `sizeVariation` the random change of size in percent, `rotation` the random turn in degrees either
// way, `followPath` (0 or 1) whether the copies turn with the path. The point attribute named by `scaleAttr` (pen pressure `w`) scales the copies
// along the stroke. The randomness depends only on `seed` and the copy's number. The artwork is centred on its box; every copy takes the shape key
// of the first path (so the stroke stays one shape, as with calligraphy) and carries the point and primitive attribute `copyIndex`.
class StrokeScatterNode : public Node {
public:
    StrokeScatterNode() : Node("strokeScatter", 2) {
        declareFloat("size", 1.0f);
        declareFloat("spacing", 100.0f);
        declareFloat("scatter", 0.0f);
        declareFloat("sizeVariation", 0.0f);
        declareFloat("rotation", 0.0f);
        declareFloat("followPath", 1.0f);
        declareFloat("seed", 1.0f);
        declareString("scaleAttr", "w");
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        const Geometry& path = *in[0];
        const Geometry& art = *in[1];
        const double size = getFloat("size"), spacing = getFloat("spacing");
        if (!(size > 0.0) || !(spacing > 0.0)) {
            throw std::runtime_error("strokeScatter: size and spacing must be above 0");
        }
        if (art.pointCount() == 0) {
            throw std::runtime_error("strokeScatter: the artwork input has no points");
        }

        double x0 = art.x()[0], x1 = x0, y0 = art.y()[0], y1 = y0;
        for (uint32_t i = 1; i < art.pointCount(); ++i) {
            x0 = std::min(x0, double(art.x()[i]));
            x1 = std::max(x1, double(art.x()[i]));
            y0 = std::min(y0, double(art.y()[i]));
            y1 = std::max(y1, double(art.y()[i]));
        }
        const double w = std::max(x1 - x0, 1e-3), h = std::max(y1 - y0, 1e-3), cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;
        Geometry centred = art;
        for (uint32_t i = 0; i < centred.pointCount(); ++i) {
            centred.x()[i] -= static_cast<float>(cx);
            centred.y()[i] -= static_cast<float>(cy);
        }

        const double step = std::max(w * size * spacing / 100.0, 0.5);
        const double scatter = getFloat("scatter") / 100.0, variation = getFloat("sizeVariation") / 100.0;
        const double rotation = getFloat("rotation") * 3.14159265358979323846 / 180.0;
        const bool follow = getFloat("followPath") >= 0.5f;
        uint32_t seed = static_cast<uint32_t>(std::max(0.0f, getFloat("seed"))) * 7919u + 1u;
        const AlignedArray<float>* weights = path.findPointAttrib(getString("scaleAttr"));
        bool weighted = false;
        for (uint32_t i = 0; weights != nullptr && i < path.pointCount() && !weighted; ++i) {
            weighted = (*weights)[i] > 0.0f;
        }

        std::vector<float> px, py, pscale, pangle;
        for (uint32_t pi = 0; pi < path.primitiveCount(); ++pi) {
            const std::vector<PathSample> line = flatten(path, path.primitives()[pi], weighted ? weights : nullptr);
            if (line.size() < 2) {
                continue;
            }
            std::vector<double> at(line.size(), 0.0);
            for (size_t k = 1; k < line.size(); ++k) {
                at[k] = at[k - 1] + std::hypot(line[k].x - line[k - 1].x, line[k].y - line[k - 1].y);
            }
            const double total = at.back();
            const int count = std::min(5000, int(std::floor(total / step)) + 1);
            size_t seg = 1;
            for (int i = 0; i < count; ++i, ++seed) {
                const double d = step * i;
                while (seg + 1 < line.size() && at[seg] < d) {
                    ++seg;
                }
                const PathSample& a = line[seg - 1];
                const PathSample& b = line[seg];
                const double length = at[seg] - at[seg - 1];
                const double u = length > 0.0 ? std::clamp((d - at[seg - 1]) / length, 0.0, 1.0) : 0.0;
                const double tx = length > 0.0 ? (b.x - a.x) / length : 1.0, ty = length > 0.0 ? (b.y - a.y) / length : 0.0;
                const double across = (scatterNoise(seed * 3u) - 0.5) * 2.0 * scatter * h * size;
                const double here = a.scale + (b.scale - a.scale) * u;
                const double scale = size * here * (1.0 + (scatterNoise(seed * 3u + 1u) - 0.5) * 2.0 * variation);
                double turn = (scatterNoise(seed * 3u + 2u) - 0.5) * 2.0 * rotation;
                if (follow) {
                    turn += std::atan2(ty, tx);
                }
                px.push_back(static_cast<float>(a.x + (b.x - a.x) * u + ty * across));
                py.push_back(static_cast<float>(a.y + (b.y - a.y) * u - tx * across));
                pscale.push_back(static_cast<float>(scale));
                pangle.push_back(static_cast<float>(turn));
            }
        }

        Geometry targets;
        targets.resizePoints(px.size());
        float* scaleOut = targets.attrib(AttribClass::point, "scale").floats().data();
        float* angleOut = targets.attrib(AttribClass::point, "angle").floats().data();
        for (size_t i = 0; i < px.size(); ++i) {
            targets.x()[i] = px[i];
            targets.y()[i] = py[i];
            scaleOut[i] = pscale[i];
            angleOut[i] = pangle[i];
        }
        auto out = std::make_shared<Geometry>(copyToPoints(centred, targets, 1.0f, 0.0f));
        if (path.primitiveCount() > 0 && path.hasShapeKeys() && !path.shapeKey(0).empty()) {
            for (size_t i = 0; i < out->primitiveCount(); ++i) {
                out->setShapeKey(i, path.shapeKey(0));
            }
        }
        return out;
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
    add<CalligraphyNode>("calligraphy");    add<StrokeScatterNode>("strokeScatter");
}

}  // namespace jitnodes
