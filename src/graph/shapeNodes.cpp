#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "jitnodes/geo/topology.h"
#include "jitnodes/graph/network.h"
#include "nodeUtil.h"

namespace jitnodes {

namespace {

constexpr float degToRad = 3.14159265359f / 180.0f;
constexpr size_t maxOutputPoints = 100'000'000;

const Geometry& flatInput(const Geometry& in, float tolerance, Geometry& storage) {
    if (!hasCurves(in)) return in;
    storage = flattenCurves(in, tolerance);
    return storage;
}

Geometry assemble(const Geometry& in, const std::vector<PointSample>& samples, const std::vector<Primitive>& prims, const std::vector<uint32_t>& srcPrim) {
    Geometry out = gatherPoints(in, samples);
    for (const Primitive& p : prims) out.addPrimitive(p);
    transferPrimAttribs(in, out, srcPrim);
    return out;
}

template <class Fn>
void forEach(ThreadPool* pool, size_t count, Fn&& fn) {
    if (pool && count > 1) pool->parallelFor(count, fn);
    else for (size_t i = 0; i < count; ++i) fn(i);
}

// Moves every interior point of a polyline toward the midpoint of its neighbours by `strength` (Laplacian smoothing). With `preserve`, each
// pass is followed by a slightly stronger pass in the opposite direction (Taubin's lambda|mu), which smooths without shrinking the curve.
// The ends of an open curve stay where they are.
class SmoothNode : public Node {
public:
    SmoothNode() : Node("smooth", 1) {
        declareFloat("iterations", 5.0f);
        declareFloat("strength", 0.5f);
        declareFloat("preserve", 1.0f);
        declareFloat("tolerance", 0.1f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        Geometry flat;
        const Geometry& src = flatInput(*in[0], getFloat("tolerance"), flat);
        const int iterations = std::clamp(static_cast<int>(getFloat("iterations")), 0, 10000);
        const float strength = std::clamp(getFloat("strength"), 0.0f, 1.0f);
        const bool preserve = getFloat("preserve") != 0.0f;
        auto geo = std::make_shared<Geometry>(src);
        const size_t n = geo->pointCount();
        if (n == 0 || iterations == 0 || strength == 0.0f) return geo;

        float* xs = geo->x();
        float* ys = geo->y();
        std::vector<float> bx(xs, xs + n), by(ys, ys + n);
        const std::vector<Primitive>& prims = geo->primitives();
        auto pass = [&](float k) {
            forEach(ctx.pool, prims.size(), [&](size_t pi) {
                const Primitive& p = prims[pi];
                if (p.count < 3) return;
                for (uint32_t i = 0; i < p.count; ++i) {
                    if (!p.closed && (i == 0 || i + 1 == p.count)) continue;
                    const uint32_t m = p.start + i;
                    const uint32_t a = p.start + (i + p.count - 1) % p.count, b = p.start + (i + 1) % p.count;
                    bx[m] = xs[m] + k * (0.5f * (xs[a] + xs[b]) - xs[m]);
                    by[m] = ys[m] + k * (0.5f * (ys[a] + ys[b]) - ys[m]);
                }
            });
            std::copy(bx.begin(), bx.end(), xs);
            std::copy(by.begin(), by.end(), ys);
        };
        const float mu = -strength / (1.0f - 0.1f * strength);
        for (int it = 0; it < iterations; ++it) {
            pass(strength);
            if (preserve) pass(mu);
        }
        return geo;
    }
};

// Parallel copies of every curve, pushed sideways along its normal (left of the direction of travel) by `distance`, 2 * distance, ... up to
// `count` copies. Corners are mitred, up to `miterLimit` times the distance; inside corners are not trimmed, so a large distance on a tight
// curve crosses itself. Copies are primitives with the int attribute `offsetIndex` (0 is the original, kept by `keepOriginal`). ribbon makes
// the two-sided stroke outline.
class OffsetNode : public Node {
public:
    OffsetNode() : Node("offset", 1) {
        declareFloat("distance", 0.1f);
        declareFloat("count", 1.0f);
        declareFloat("keepOriginal", 0.0f);
        declareFloat("miterLimit", 2.0f);
        declareFloat("tolerance", 0.1f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        Geometry flat;
        const Geometry& src = flatInput(*in[0], getFloat("tolerance"), flat);
        const float distance = getFloat("distance");
        const int copies = std::clamp(static_cast<int>(getFloat("count")), 0, 10000);
        const bool keep = getFloat("keepOriginal") != 0.0f;
        const float limit = std::max(getFloat("miterLimit"), 1.0f);
        const float* sx = src.pointCount() ? std::as_const(src).x() : nullptr;
        const float* sy = src.pointCount() ? std::as_const(src).y() : nullptr;

        std::vector<PointSample> samples;
        std::vector<Primitive> prims;
        std::vector<uint32_t> srcPrim;
        std::vector<int32_t> index;
        std::vector<float> px, py;
        std::vector<float> ox, oy;
        for (uint32_t pi = 0; pi < src.primitiveCount(); ++pi) {
            const Primitive& p = src.primitives()[pi];
            const size_t n = p.count;
            ox.assign(n, 0.0f);
            oy.assign(n, 0.0f);
            if (n >= 2) {
                for (size_t i = 0; i < n; ++i) {
                    const size_t prev = p.closed ? (i + n - 1) % n : (i == 0 ? 0 : i - 1);
                    const size_t next = p.closed ? (i + 1) % n : (i + 1 == n ? n - 1 : i + 1);
                    const uint32_t a = p.start + static_cast<uint32_t>(prev), m = p.start + static_cast<uint32_t>(i), b = p.start + static_cast<uint32_t>(next);
                    auto unit = [&](uint32_t from, uint32_t to, float& dx, float& dy) {
                        dx = sx[to] - sx[from];
                        dy = sy[to] - sy[from];
                        const float len = std::hypot(dx, dy);
                        dx = len > 1e-20f ? dx / len : 0.0f;
                        dy = len > 1e-20f ? dy / len : 0.0f;
                    };
                    float d1x, d1y, d2x, d2y;
                    unit(a, m, d1x, d1y);
                    unit(m, b, d2x, d2y);
                    if (i == 0 && !p.closed) {
                        d1x = d2x;
                        d1y = d2y;
                    } else if (i + 1 == n && !p.closed) {
                        d2x = d1x;
                        d2y = d1y;
                    }
                    float tx = d1x + d2x, ty = d1y + d2y;
                    const float tl = std::hypot(tx, ty);
                    if (tl > 1e-6f) {
                        tx /= tl;
                        ty /= tl;
                    } else {
                        tx = d2x;
                        ty = d2y;
                    }
                    const float miter = 1.0f / std::max(tx * d2x + ty * d2y, 1.0f / limit);
                    ox[i] = -ty * miter;
                    oy[i] = tx * miter;
                }
            }
            for (int k = keep ? 0 : 1; k <= copies; ++k) {
                if (n < 2 && k > 0) continue;
                const uint32_t start = static_cast<uint32_t>(samples.size());
                for (uint32_t i = 0; i < p.count; ++i) {
                    samples.push_back({p.start + i, p.start + i, 0.0f});
                    px.push_back(sx[p.start + i] + ox[i] * distance * static_cast<float>(k));
                    py.push_back(sy[p.start + i] + oy[i] * distance * static_cast<float>(k));
                }
                if (samples.size() > maxOutputPoints) throw std::runtime_error("offset: more than 100 million points");
                prims.push_back({start, p.count, p.closed, PrimKind::polyline});
                srcPrim.push_back(pi);
                index.push_back(k);
            }
        }
        auto geo = std::make_shared<Geometry>(assemble(src, samples, prims, srcPrim));
        if (!samples.empty()) {
            std::copy(px.begin(), px.end(), geo->x());
            std::copy(py.begin(), py.end(), geo->y());
        }
        if (!prims.empty()) {
            int32_t* out = geo->attrib(AttribClass::primitive, "offsetIndex", AttribType::int32).ints().data();
            std::copy(index.begin(), index.end(), out);
        }
        return geo;
    }
};

// Reorders points (only for geometry without primitives) or primitives by a key. Keys: x, y (a primitive's centroid), distance and angle
// about (centerX, centerY), length and size (primitives), random, index and attrib (a primitive attribute, else the mean of the point
// attribute over the primitive; string attributes sort by their text). Ties keep their order, `reverse` flips the result and `shift` rotates
// it, so the same node can pick a new start of a closed ordering.
class SortNode : public Node {
public:
    SortNode() : Node("sort", 1) {
        declareString("class", "primitive");
        declareString("key", "x");
        declareString("attrib", "");
        declareFloat("centerX", 0.0f);
        declareFloat("centerY", 0.0f);
        declareFloat("reverse", 0.0f);
        declareFloat("shift", 0.0f);
        declareFloat("seed", 1.0f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        const Geometry& src = *in[0];
        const std::string& cls = getString("class");
        if (cls != "point" && cls != "primitive") throw std::runtime_error("sort: class must be 'point' or 'primitive', got '" + cls + "'");
        const bool primitive = cls == "primitive";
        if (!primitive && src.primitiveCount() > 0) throw std::runtime_error("sort: points that belong to primitives cannot be reordered; sort the primitives");
        const std::string& key = getString("key");
        static const char* const keys[] = {"x", "y", "distance", "angle", "length", "size", "random", "index", "attrib"};
        if (std::find(std::begin(keys), std::end(keys), key) == std::end(keys)) throw std::runtime_error("sort: unknown key '" + key + "'");
        if (!primitive && (key == "length" || key == "size")) throw std::runtime_error("sort: key '" + key + "' needs class primitive");

        const size_t count = primitive ? src.primitiveCount() : src.pointCount();
        const float* sx = src.pointCount() ? std::as_const(src).x() : nullptr;
        const float* sy = src.pointCount() ? std::as_const(src).y() : nullptr;
        const auto& prims = src.primitives();
        const float cx = getFloat("centerX"), cy = getFloat("centerY");
        const uint32_t seed = seedOf(getFloat("seed"));

        auto position = [&](size_t e, float& x, float& y) {
            if (!primitive) {
                x = sx[e];
                y = sy[e];
                return;
            }
            const Primitive& p = prims[e];
            x = y = 0.0f;
            for (uint32_t j = 0; j < p.count; ++j) {
                x += sx[p.start + j];
                y += sy[p.start + j];
            }
            if (p.count) {
                x /= static_cast<float>(p.count);
                y /= static_cast<float>(p.count);
            }
        };

        std::vector<float> value(count, 0.0f);
        std::vector<const std::string*> text;
        if (key == "attrib") {
            const std::string& name = getString("attrib");
            if (name.empty()) throw std::runtime_error("sort: key 'attrib' needs the attrib name");
            const AttribArray* direct = src.findAttrib(primitive ? AttribClass::primitive : AttribClass::point, name);
            if (direct && direct->type() == AttribType::string) {
                text.resize(count);
                for (size_t e = 0; e < count; ++e) text[e] = &direct->stringAt(e);
            } else if (direct) {
                for (size_t e = 0; e < count; ++e) value[e] = direct->at(e);
            } else {
                const AttribArray* points = primitive ? src.findAttrib(AttribClass::point, name) : nullptr;
                if (!points) throw std::runtime_error("sort: the input has no attribute '" + name + "'");
                if (points->type() == AttribType::string) throw std::runtime_error("sort: a string point attribute cannot be averaged over a primitive");
                for (size_t e = 0; e < count; ++e) {
                    const Primitive& p = prims[e];
                    float sum = 0.0f;
                    for (uint32_t j = 0; j < p.count; ++j) sum += points->at(p.start + j);
                    value[e] = p.count ? sum / static_cast<float>(p.count) : 0.0f;
                }
            }
        } else {
            for (size_t e = 0; e < count; ++e) {
                float x = 0.0f, y = 0.0f;
                if (key == "x" || key == "y" || key == "distance" || key == "angle") position(e, x, y);
                if (key == "x") value[e] = x;
                else if (key == "y") value[e] = y;
                else if (key == "distance") value[e] = std::hypot(x - cx, y - cy);
                else if (key == "angle") value[e] = std::atan2(y - cy, x - cx);
                else if (key == "random") value[e] = random01(seed, static_cast<uint32_t>(e), 7);
                else if (key == "index") value[e] = static_cast<float>(e);
                else if (key == "size") value[e] = static_cast<float>(prims[e].count);
                else if (key == "length") {
                    const Primitive& p = prims[e];
                    float total = 0.0f;
                    const uint32_t segments = p.count < 2 ? 0 : (p.closed ? p.count : p.count - 1);
                    for (uint32_t j = 0; j < segments; ++j) {
                        const uint32_t a = p.start + j, b = p.start + (j + 1) % p.count;
                        total += std::hypot(sx[b] - sx[a], sy[b] - sy[a]);
                    }
                    value[e] = total;
                }
            }
        }

        std::vector<uint32_t> order(count);
        std::iota(order.begin(), order.end(), 0u);
        if (!text.empty()) {
            std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return *text[a] < *text[b]; });
        } else {
            std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
                const float va = value[a], vb = value[b];
                return !std::isnan(va) && (std::isnan(vb) || va < vb);
            });
        }
        if (getFloat("reverse") != 0.0f) std::reverse(order.begin(), order.end());
        if (count > 0) {
            const long long shift = static_cast<long long>(std::floor(getFloat("shift")));
            const long long by = ((shift % static_cast<long long>(count)) + static_cast<long long>(count)) % static_cast<long long>(count);
            std::rotate(order.begin(), order.begin() + by, order.end());
        }

        std::vector<PointSample> samples;
        samples.reserve(src.pointCount());
        if (!primitive) {
            for (uint32_t e : order) samples.push_back({e, e, 0.0f});
            return std::make_shared<Geometry>(assemble(src, samples, {}, {}));
        }
        std::vector<Primitive> out;
        std::vector<char> owned(src.pointCount(), 0);
        for (uint32_t e : order) {
            Primitive p = prims[e];
            const uint32_t start = static_cast<uint32_t>(samples.size());
            for (uint32_t j = 0; j < p.count; ++j) {
                samples.push_back({p.start + j, p.start + j, 0.0f});
                owned[p.start + j] = 1;
            }
            p.start = start;
            out.push_back(p);
        }
        for (uint32_t i = 0; i < owned.size(); ++i)
            if (!owned[i]) samples.push_back({i, i, 0.0f});
        return std::make_shared<Geometry>(assemble(src, samples, out, order));
    }
};

struct JoinedCurve {
    uint32_t key;
    std::vector<uint32_t> points;
    bool closed;
    uint32_t srcPrim;
};

// Joins open curves into longer ones. `ends`: curves whose end points lie within `distance` of each other become one curve (a curve may
// be reversed to fit; every end joins at most one other, nearest pairs first), and a chain that returns to its start becomes closed.
// `all`: every curve in order becomes one curve, closed when `close` is set. The merged curve keeps the attributes of its first point and
// first curve; at a join the second point is dropped. Closed curves and single points pass through.
class JoinNode : public Node {
public:
    JoinNode() : Node("join", 1) {
        declareString("mode", "ends");
        declareFloat("distance", 0.001f);
        declareFloat("close", 0.0f);
        declareFloat("tolerance", 0.1f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        const std::string& mode = getString("mode");
        if (mode != "ends" && mode != "all") throw std::runtime_error("join: mode must be 'ends' or 'all', got '" + mode + "'");
        Geometry flat;
        const Geometry& src = flatInput(*in[0], getFloat("tolerance"), flat);
        const float distance = std::max(getFloat("distance"), 0.0f);
        const float* sx = src.pointCount() ? std::as_const(src).x() : nullptr;
        const float* sy = src.pointCount() ? std::as_const(src).y() : nullptr;
        const auto& prims = src.primitives();
        const size_t np = prims.size();

        std::vector<JoinedCurve> curves;
        auto indices = [&](const Primitive& p, bool reversed, std::vector<uint32_t>& dst, bool skipFirst) {
            for (uint32_t j = skipFirst ? 1 : 0; j < p.count; ++j) dst.push_back(p.start + (reversed ? p.count - 1 - j : j));
        };

        if (mode == "all") {
            JoinedCurve all{0, {}, getFloat("close") != 0.0f, 0};
            bool first = true;
            for (size_t pi = 0; pi < np; ++pi) {
                const Primitive& p = prims[pi];
                if (p.count == 0) continue;
                if (first) all.srcPrim = static_cast<uint32_t>(pi);
                const bool touching = !first && std::hypot(sx[p.start] - sx[all.points.back()], sy[p.start] - sy[all.points.back()]) <= distance;
                indices(p, false, all.points, touching);
                first = false;
            }
            if (!first) curves.push_back(std::move(all));
        } else {
            std::vector<char> joinable(np, 0);
            std::vector<uint32_t> ends;
            for (size_t pi = 0; pi < np; ++pi) {
                joinable[pi] = !prims[pi].closed && prims[pi].count >= 2;
                if (joinable[pi]) {
                    ends.push_back(static_cast<uint32_t>(2 * pi));
                    ends.push_back(static_cast<uint32_t>(2 * pi + 1));
                }
            }
            auto endPoint = [&](uint32_t e) { return prims[e / 2].start + (e % 2 ? prims[e / 2].count - 1 : 0); };

            const float cell = std::max(distance, 1e-6f);
            auto cellOf = [&](float v) { return static_cast<int64_t>(std::floor(v / cell)); };
            auto cellKey = [](int64_t cx, int64_t cy) { return static_cast<uint64_t>(cx) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(cy) * 0xC2B2AE3D27D4EB4Full; };
            std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
            for (uint32_t e : ends) grid[cellKey(cellOf(sx[endPoint(e)]), cellOf(sy[endPoint(e)]))].push_back(e);

            struct Pair {
                float d;
                uint32_t a, b;
            };
            std::vector<Pair> pairs;
            for (uint32_t e : ends) {
                const float x = sx[endPoint(e)], y = sy[endPoint(e)];
                for (int64_t gx = cellOf(x) - 1; gx <= cellOf(x) + 1; ++gx)
                    for (int64_t gy = cellOf(y) - 1; gy <= cellOf(y) + 1; ++gy) {
                        auto found = grid.find(cellKey(gx, gy));
                        if (found == grid.end()) continue;
                        for (uint32_t o : found->second) {
                            if (o <= e) continue;
                            if (o / 2 == e / 2 && prims[e / 2].count < 3) continue;
                            const float d = std::hypot(sx[endPoint(o)] - x, sy[endPoint(o)] - y);
                            if (d <= distance) pairs.push_back({d, e, o});
                        }
                    }
            }
            std::sort(pairs.begin(), pairs.end(), [](const Pair& l, const Pair& r) { return l.d != r.d ? l.d < r.d : (l.a != r.a ? l.a < r.a : l.b < r.b); });
            std::vector<int64_t> link(2 * np, -1);
            for (const Pair& p : pairs)
                if (link[p.a] < 0 && link[p.b] < 0) {
                    link[p.a] = p.b;
                    link[p.b] = p.a;
                }

            std::vector<char> visited(np, 0);
            auto walk = [&](size_t start, int enter) {
                JoinedCurve c{static_cast<uint32_t>(start), {}, false, static_cast<uint32_t>(start)};
                size_t cur = start;
                int e = enter;
                bool first = true;
                while (true) {
                    visited[cur] = 1;
                    c.key = std::min(c.key, static_cast<uint32_t>(cur));
                    indices(prims[cur], e == 1, c.points, !first);
                    first = false;
                    const int64_t next = link[2 * cur + (1 - e)];
                    if (next < 0) break;
                    const size_t nextPrim = static_cast<size_t>(next) / 2;
                    if (visited[nextPrim]) {
                        c.closed = nextPrim == start;
                        break;
                    }
                    cur = nextPrim;
                    e = static_cast<int>(next % 2);
                }
                if (c.closed) c.points.pop_back();
                curves.push_back(std::move(c));
            };
            for (size_t pi = 0; pi < np; ++pi) {
                if (!joinable[pi] || visited[pi]) continue;
                if (link[2 * pi] < 0) walk(pi, 0);
                else if (link[2 * pi + 1] < 0) walk(pi, 1);
            }
            for (size_t pi = 0; pi < np; ++pi)
                if (joinable[pi] && !visited[pi]) walk(pi, 0);
            for (size_t pi = 0; pi < np; ++pi) {
                if (joinable[pi]) continue;
                JoinedCurve c{static_cast<uint32_t>(pi), {}, prims[pi].closed, static_cast<uint32_t>(pi)};
                indices(prims[pi], false, c.points, false);
                curves.push_back(std::move(c));
            }
            std::sort(curves.begin(), curves.end(), [](const JoinedCurve& l, const JoinedCurve& r) { return l.key < r.key; });
        }

        std::vector<PointSample> samples;
        std::vector<Primitive> out;
        std::vector<uint32_t> srcPrim;
        for (const JoinedCurve& c : curves) {
            const uint32_t start = static_cast<uint32_t>(samples.size());
            for (uint32_t i : c.points) samples.push_back({i, i, 0.0f});
            out.push_back({start, static_cast<uint32_t>(c.points.size()), c.closed, PrimKind::polyline});
            srcPrim.push_back(c.srcPrim);
        }
        return std::make_shared<Geometry>(assemble(src, samples, out, srcPrim));
    }
};

// Cuts every corner of a polyline: the vertex is replaced by a point `distance` back along the incoming edge and one `distance` forward
// along the outgoing edge (a chamfer), or by `segments` pieces of the quadratic curve between them (a round). The cut is at most half of
// either edge, corners that turn less than `minAngle` degrees are left alone, and the ends of an open curve stay. The new points take the
// attributes of the edge points they lie on; the points of a round take the corner's.
class BevelNode : public Node {
public:
    BevelNode() : Node("bevel", 1) {
        declareFloat("distance", 0.1f);
        declareFloat("segments", 1.0f);
        declareFloat("minAngle", 1.0f);
        declareFloat("tolerance", 0.1f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        Geometry flat;
        const Geometry& src = flatInput(*in[0], getFloat("tolerance"), flat);
        const float distance = std::max(getFloat("distance"), 0.0f);
        const int segments = std::clamp(static_cast<int>(getFloat("segments")), 1, 64);
        const float minCos = std::cos(std::clamp(getFloat("minAngle"), 0.0f, 179.0f) * degToRad);
        const float* sx = src.pointCount() ? std::as_const(src).x() : nullptr;
        const float* sy = src.pointCount() ? std::as_const(src).y() : nullptr;

        std::vector<PointSample> samples;
        std::vector<Primitive> prims;
        std::vector<uint32_t> srcPrim;
        std::vector<float> px, py;
        auto emit = [&](PointSample s, float x, float y) {
            samples.push_back(s);
            px.push_back(x);
            py.push_back(y);
        };
        for (uint32_t pi = 0; pi < src.primitiveCount(); ++pi) {
            const Primitive& p = src.primitives()[pi];
            const uint32_t start = static_cast<uint32_t>(samples.size());
            for (uint32_t i = 0; i < p.count; ++i) {
                const uint32_t m = p.start + i;
                const bool end = !p.closed && (i == 0 || i + 1 == p.count);
                if (end || p.count < 3 || distance == 0.0f) {
                    emit({m, m, 0.0f}, sx[m], sy[m]);
                    continue;
                }
                const uint32_t a = p.start + (i + p.count - 1) % p.count, b = p.start + (i + 1) % p.count;
                const float v1x = sx[m] - sx[a], v1y = sy[m] - sy[a], v2x = sx[b] - sx[m], v2y = sy[b] - sy[m];
                const float l1 = std::hypot(v1x, v1y), l2 = std::hypot(v2x, v2y);
                if (l1 < 1e-9f || l2 < 1e-9f || (v1x * v2x + v1y * v2y) / (l1 * l2) > minCos) {
                    emit({m, m, 0.0f}, sx[m], sy[m]);
                    continue;
                }
                const float cut = std::min({distance, 0.5f * l1, 0.5f * l2});
                const float ax = sx[m] - v1x / l1 * cut, ay = sy[m] - v1y / l1 * cut;
                const float bx = sx[m] + v2x / l2 * cut, by = sy[m] + v2y / l2 * cut;
                emit({a, m, 1.0f - cut / l1}, ax, ay);
                for (int k = 1; k < segments; ++k) {
                    const float t = static_cast<float>(k) / static_cast<float>(segments), u = 1.0f - t;
                    emit({m, m, 0.0f}, u * u * ax + 2.0f * u * t * sx[m] + t * t * bx, u * u * ay + 2.0f * u * t * sy[m] + t * t * by);
                }
                emit({m, b, cut / l2}, bx, by);
            }
            if (samples.size() > maxOutputPoints) throw std::runtime_error("bevel: more than 100 million points");
            prims.push_back({start, static_cast<uint32_t>(samples.size()) - start, p.closed, PrimKind::polyline});
            srcPrim.push_back(pi);
        }
        auto geo = std::make_shared<Geometry>(assemble(src, samples, prims, srcPrim));
        if (!samples.empty()) {
            std::copy(px.begin(), px.end(), geo->x());
            std::copy(py.begin(), py.end(), geo->y());
        }
        return geo;
    }
};

// Bends a flat layout around a centre, or flattens a ring back out. toPolar: x runs along the angle (`width` of x is `sweep` degrees,
// starting at `startAngle`) and y is the radius. fromPolar is the inverse, so a polar warp of a grid gives rings and spokes and the inverse
// unrolls them. Points move, edges stay straight, so resample first when long edges should curve. `amount` blends from the input.
class PolarNode : public Node {
public:
    PolarNode() : Node("polar", 1) {
        declareString("mode", "toPolar");
        declareFloat("centerX", 0.0f);
        declareFloat("centerY", 0.0f);
        declareFloat("width", 1.0f);
        declareFloat("sweep", 360.0f);
        declareFloat("startAngle", 0.0f);
        declareFloat("amount", 1.0f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const std::string& mode = getString("mode");
        if (mode != "toPolar" && mode != "fromPolar") throw std::runtime_error("polar: mode must be 'toPolar' or 'fromPolar', got '" + mode + "'");
        const float width = getFloat("width"), sweep = getFloat("sweep");
        if (width == 0.0f || sweep == 0.0f) throw std::runtime_error("polar: width and sweep must not be 0");
        const float cx = getFloat("centerX"), cy = getFloat("centerY"), start = getFloat("startAngle"), amount = getFloat("amount");
        const bool toPolar = mode == "toPolar";

        auto geo = std::make_shared<Geometry>(*in[0]);
        if (geo->pointCount() == 0) return geo;
        float* xs = geo->x();
        float* ys = geo->y();
        parallelChunks(ctx.pool, geo->pointCount(), [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                float nx, ny;
                if (toPolar) {
                    const float angle = (start + xs[i] / width * sweep) * degToRad;
                    nx = cx + ys[i] * std::cos(angle);
                    ny = cy + ys[i] * std::sin(angle);
                } else {
                    const float dx = xs[i] - cx, dy = ys[i] - cy;
                    float relative = std::atan2(dy, dx) / degToRad - start;
                    relative -= 360.0f * std::floor(relative / 360.0f);
                    nx = relative / sweep * width;
                    ny = std::hypot(dx, dy);
                }
                xs[i] += (nx - xs[i]) * amount;
                ys[i] += (ny - ys[i]) * amount;
            }
        });
        return geo;
    }
};

// Rotates points about a centre by an angle that depends on their distance r from it. With `radius` > 0 the swirl is strongest at the centre
// and fades to nothing at the radius: angle * (1 - r / radius)^power. With radius 0 the angle grows with distance, `angle` degrees per unit.
class TwistNode : public Node {
public:
    TwistNode() : Node("twist", 1) {
        declareFloat("centerX", 0.0f);
        declareFloat("centerY", 0.0f);
        declareFloat("angle", 90.0f);
        declareFloat("radius", 0.0f);
        declareFloat("power", 1.0f);
        declareFloat("amount", 1.0f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const float cx = getFloat("centerX"), cy = getFloat("centerY"), angle = getFloat("angle"), radius = getFloat("radius");
        const float power = std::max(getFloat("power"), 0.0f), amount = getFloat("amount");
        auto geo = std::make_shared<Geometry>(*in[0]);
        if (geo->pointCount() == 0) return geo;
        float* xs = geo->x();
        float* ys = geo->y();
        parallelChunks(ctx.pool, geo->pointCount(), [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const float dx = xs[i] - cx, dy = ys[i] - cy;
                const float r = std::hypot(dx, dy);
                const float turn = radius > 0.0f ? angle * std::pow(std::max(1.0f - r / radius, 0.0f), power) : angle * r;
                const float a = turn * amount * degToRad, c = std::cos(a), s = std::sin(a);
                xs[i] = cx + dx * c - dy * s;
                ys[i] = cy + dx * s + dy * c;
            }
        });
        return geo;
    }
};

// Pushes points along their normal by `amount` (times the `scaleAttrib` point attribute when named). The normal is the left normal of the
// curve, as in offset and curveSample: the point attributes `nx` / `ny` when the input has them, else the average over the primitives that
// use the point. mode = radial pushes away from (centerX, centerY) instead and needs no primitives. Points without a normal stay.
class PeakNode : public Node {
public:
    PeakNode() : Node("peak", 1) {
        declareString("mode", "normal");
        declareFloat("amount", 0.1f);
        declareString("scaleAttrib", "");
        declareFloat("centerX", 0.0f);
        declareFloat("centerY", 0.0f);
        declareFloat("tolerance", 0.1f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const std::string& mode = getString("mode");
        if (mode != "normal" && mode != "radial") throw std::runtime_error("peak: mode must be 'normal' or 'radial', got '" + mode + "'");
        const bool radial = mode == "radial";
        Geometry flat;
        const Geometry& src = radial ? *in[0] : flatInput(*in[0], getFloat("tolerance"), flat);
        const float amount = getFloat("amount");
        const std::string& scaleName = getString("scaleAttrib");
        const AttribArray* scale = nullptr;
        if (!scaleName.empty()) {
            scale = src.findAttrib(AttribClass::point, scaleName);
            if (!scale) throw std::runtime_error("peak: the input has no point attribute '" + scaleName + "'");
        }
        auto geo = std::make_shared<Geometry>(src);
        const size_t n = geo->pointCount();
        if (n == 0) return geo;
        float* xs = geo->x();
        float* ys = geo->y();
        const float* ox = std::as_const(src).x();
        const float* oy = std::as_const(src).y();

        std::vector<float> nx(n, 0.0f), ny(n, 0.0f);
        if (radial) {
            const float cx = getFloat("centerX"), cy = getFloat("centerY");
            for (size_t i = 0; i < n; ++i) {
                const float dx = ox[i] - cx, dy = oy[i] - cy, r = std::hypot(dx, dy);
                if (r > 1e-20f) {
                    nx[i] = dx / r;
                    ny[i] = dy / r;
                }
            }
        } else {
            const AttribArray* ax = src.findAttrib(AttribClass::point, "nx");
            const AttribArray* ay = src.findAttrib(AttribClass::point, "ny");
            if (ax && ay) {
                for (size_t i = 0; i < n; ++i) {
                    nx[i] = ax->at(i);
                    ny[i] = ay->at(i);
                }
            } else {
                for (const Primitive& p : src.primitives()) {
                    if (p.count < 2) continue;
                    const uint32_t segments = p.closed ? p.count : p.count - 1;
                    for (uint32_t k = 0; k < segments; ++k) {
                        const uint32_t a = p.start + k, b = p.start + (k + 1) % p.count;
                        const float dx = ox[b] - ox[a], dy = oy[b] - oy[a], len = std::hypot(dx, dy);
                        if (len < 1e-20f) continue;
                        for (uint32_t v : {a, b}) {
                            nx[v] += -dy / len;
                            ny[v] += dx / len;
                        }
                    }
                }
                for (size_t i = 0; i < n; ++i) {
                    const float len = std::hypot(nx[i], ny[i]);
                    if (len > 1e-6f) {
                        nx[i] /= len;
                        ny[i] /= len;
                    } else {
                        nx[i] = ny[i] = 0.0f;
                    }
                }
            }
        }
        parallelChunks(ctx.pool, n, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const float d = amount * (scale ? scale->at(i) : 1.0f);
                xs[i] = ox[i] + nx[i] * d;
                ys[i] = oy[i] + ny[i] * d;
            }
        });
        return geo;
    }
};

// Free-form deformation: input 0 is moved with a lattice of `columns` x `rows` control points laid out evenly over a box (minX..maxX,
// minY..maxY; all four 0 takes the bounds of input 0). Input 1 holds the control points moved to their new places, row by row from the
// bottom left, x fastest. Each point is displaced by the Bezier blend of the control point displacements at its place in the box; a point
// outside the box gets the displacement of the nearest edge. Moving the control points back to the even grid undoes the deformation.
class LatticeNode : public Node {
public:
    LatticeNode() : Node("lattice", 2) {
        declareFloat("columns", 4.0f);
        declareFloat("rows", 4.0f);
        declareFloat("minX", 0.0f);
        declareFloat("minY", 0.0f);
        declareFloat("maxX", 0.0f);
        declareFloat("maxY", 0.0f);
        declareFloat("amount", 1.0f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const size_t cols = static_cast<size_t>(std::clamp(getFloat("columns"), 2.0f, 16.0f));
        const size_t rows = static_cast<size_t>(std::clamp(getFloat("rows"), 2.0f, 16.0f));
        const Geometry& deformed = *in[1];
        if (deformed.pointCount() != cols * rows)
            throw std::runtime_error("lattice: input 1 needs columns * rows = " + std::to_string(cols * rows) + " control points, it has " + std::to_string(deformed.pointCount()));
        auto geo = std::make_shared<Geometry>(*in[0]);
        const size_t n = geo->pointCount();
        if (n == 0) return geo;
        float* xs = geo->x();
        float* ys = geo->y();

        float minX = getFloat("minX"), minY = getFloat("minY"), maxX = getFloat("maxX"), maxY = getFloat("maxY");
        if (minX == 0.0f && minY == 0.0f && maxX == 0.0f && maxY == 0.0f) {
            minX = minY = 1e30f;
            maxX = maxY = -1e30f;
            for (size_t i = 0; i < n; ++i) {
                minX = std::min(minX, xs[i]);
                maxX = std::max(maxX, xs[i]);
                minY = std::min(minY, ys[i]);
                maxY = std::max(maxY, ys[i]);
            }
        }
        const float width = maxX > minX ? maxX - minX : 1.0f, height = maxY > minY ? maxY - minY : 1.0f;
        std::vector<float> dxs(cols * rows), dys(cols * rows);
        const float* cx = std::as_const(deformed).x();
        const float* cy = std::as_const(deformed).y();
        for (size_t j = 0; j < rows; ++j)
            for (size_t i = 0; i < cols; ++i) {
                const size_t k = j * cols + i;
                dxs[k] = cx[k] - (minX + width * static_cast<float>(i) / static_cast<float>(cols - 1));
                dys[k] = cy[k] - (minY + height * static_cast<float>(j) / static_cast<float>(rows - 1));
            }

        auto bernstein = [](size_t degree, float t, float* out) {
            float c = 1.0f;
            for (size_t i = 0; i <= degree; ++i) {
                out[i] = c * std::pow(t, static_cast<float>(i)) * std::pow(1.0f - t, static_cast<float>(degree - i));
                c = c * static_cast<float>(degree - i) / static_cast<float>(i + 1);
            }
        };
        const float amount = getFloat("amount");
        parallelChunks(ctx.pool, n, [&](size_t begin, size_t end) {
            float bu[16], bv[16];
            for (size_t p = begin; p < end; ++p) {
                bernstein(cols - 1, std::clamp((xs[p] - minX) / width, 0.0f, 1.0f), bu);
                bernstein(rows - 1, std::clamp((ys[p] - minY) / height, 0.0f, 1.0f), bv);
                float mx = 0.0f, my = 0.0f;
                for (size_t j = 0; j < rows; ++j)
                    for (size_t i = 0; i < cols; ++i) {
                        const float w = bu[i] * bv[j];
                        mx += w * dxs[j * cols + i];
                        my += w * dys[j * cols + i];
                    }
                xs[p] += mx * amount;
                ys[p] += my * amount;
            }
        });
        return geo;
    }
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerShapeNodes() {
    add<SmoothNode>("smooth");
    add<OffsetNode>("offset");
    add<SortNode>("sort");
    add<JoinNode>("join");
    add<BevelNode>("bevel");
    add<PolarNode>("polar");
    add<TwistNode>("twist");
    add<PeakNode>("peak");
    add<LatticeNode>("lattice");
}

}  // namespace jitnodes
