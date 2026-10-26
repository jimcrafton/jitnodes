#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "nodeUtil.h"
#include "noise.h"
#include "pointGrid.h"
#include "jitnodes/graph/network.h"

namespace jitnodes {

namespace {

constexpr int64_t maxGrowSteps = 100'000;
constexpr size_t maxStreamPoints = 50'000'000;

struct Chain {
    bool closed = false;
    std::vector<float> x, y;
    std::vector<int32_t> born;
};

struct GrowSettings {
    float dt = 1.0f / 60.0f;
    float repel = 0.0f, repelRadius = 0.5f, attract = 0.0f, smooth = 0.0f, maxSpeed = 3.0f;
    float maxLength = 0.25f;
    size_t maxPoints = 20000;
    bool bounds = false;
    float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
};

struct GrowState {
    std::vector<Chain> chains;
    int64_t step = -1;
    GeometryHandle source;
    std::vector<float> signature;
};

size_t pointTotal(const std::vector<Chain>& chains) {
    size_t n = 0;
    for (const Chain& c : chains) n += c.x.size();
    return n;
}

void initialiseGrow(GrowState& st, const Geometry& g) {
    st.chains.clear();
    const float* xs = std::as_const(g).x();
    const float* ys = std::as_const(g).y();
    for (const Primitive& p : g.primitives()) {
        if (p.count < 2) continue;
        Chain c;
        c.closed = p.closed;
        for (uint32_t k = 0; k < p.count; ++k) {
            c.x.push_back(xs[p.start + k]);
            c.y.push_back(ys[p.start + k]);
            c.born.push_back(0);
        }
        st.chains.push_back(std::move(c));
    }
    if (st.chains.empty()) throw std::runtime_error("grow: the input has no polylines (a primitive with at least 2 points)");
}

// Every node repels the nodes within `repelRadius` (but not its chain neighbours), is pulled towards its chain neighbours and towards the
// midpoint between them, and a segment longer than `maxLength` is split. All moves are computed from the old positions, so the result does
// not depend on the thread count.
void advanceGrow(GrowState& st, const GrowSettings& s, int64_t step, ThreadPool* pool) {
    const size_t n = pointTotal(st.chains);
    std::vector<float> px(n), py(n), nx(n), ny(n);
    std::vector<uint32_t> chainOf(n), local(n), first(st.chains.size());
    size_t at = 0;
    for (size_t c = 0; c < st.chains.size(); ++c) {
        first[c] = static_cast<uint32_t>(at);
        for (size_t k = 0; k < st.chains[c].x.size(); ++k, ++at) {
            px[at] = st.chains[c].x[k];
            py[at] = st.chains[c].y[k];
            chainOf[at] = static_cast<uint32_t>(c);
            local[at] = static_cast<uint32_t>(k);
        }
    }
    PointGrid grid;
    if (s.repel > 0.0f) grid.build(px.data(), py.data(), n, s.repelRadius);
    const float attractK = std::clamp(s.attract * s.dt, 0.0f, 1.0f), smoothK = std::clamp(s.smooth * s.dt, 0.0f, 1.0f);
    const float maxMove = s.maxSpeed * s.dt;

    parallelChunks(pool, n, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const Chain& chain = st.chains[chainOf[i]];
            const uint32_t count = static_cast<uint32_t>(chain.x.size()), k = local[i], base = first[chainOf[i]];
            const bool hasPrev = chain.closed || k > 0, hasNext = chain.closed || k + 1 < count;
            const size_t prev = hasPrev ? base + (k + count - 1) % count : i, next = hasNext ? base + (k + 1) % count : i;
            float mx = 0.0f, my = 0.0f;
            if (s.repel > 0.0f) {
                grid.forEachNear(px[i], py[i], [&](uint32_t j) {
                    if (j == i) return;
                    if (chainOf[j] == chainOf[i]) {
                        const uint32_t a = local[j] > k ? local[j] - k : k - local[j];
                        if (std::min(a, chain.closed ? count - a : a) <= 1) return;
                    }
                    float dx = px[i] - px[j], dy = py[i] - py[j];
                    const float d = std::sqrt(dx * dx + dy * dy);
                    if (d >= s.repelRadius) return;
                    if (d < 1e-9f) {
                        const float angle = random01(1, static_cast<uint32_t>(i), static_cast<uint32_t>(j)) * 6.2831853f;
                        dx = std::cos(angle);
                        dy = std::sin(angle);
                    } else {
                        dx /= d;
                        dy /= d;
                    }
                    const float push = s.repel * (1.0f - d / s.repelRadius) * s.dt;
                    mx += dx * push;
                    my += dy * push;
                });
            }
            if (hasPrev) {
                mx += (px[prev] - px[i]) * attractK;
                my += (py[prev] - py[i]) * attractK;
            }
            if (hasNext) {
                mx += (px[next] - px[i]) * attractK;
                my += (py[next] - py[i]) * attractK;
            }
            if (hasPrev && hasNext) {
                mx += (0.5f * (px[prev] + px[next]) - px[i]) * smoothK;
                my += (0.5f * (py[prev] + py[next]) - py[i]) * smoothK;
            }
            const float len = std::sqrt(mx * mx + my * my);
            if (len > maxMove) {
                mx *= maxMove / len;
                my *= maxMove / len;
            }
            nx[i] = px[i] + mx;
            ny[i] = py[i] + my;
            if (s.bounds) {
                nx[i] = std::clamp(nx[i], s.minX, s.maxX);
                ny[i] = std::clamp(ny[i], s.minY, s.maxY);
            }
        }
    });

    size_t total = n;
    const float maxLength2 = s.maxLength * s.maxLength;
    for (size_t c = 0; c < st.chains.size(); ++c) {
        Chain& chain = st.chains[c];
        const size_t count = chain.x.size();
        Chain grown;
        grown.closed = chain.closed;
        for (size_t k = 0; k < count; ++k) {
            const size_t i = first[c] + k;
            grown.x.push_back(nx[i]);
            grown.y.push_back(ny[i]);
            grown.born.push_back(chain.born[k]);
            const bool last = k + 1 == count;
            if (last && !chain.closed) break;
            const size_t j = last ? first[c] : i + 1;
            const float dx = nx[j] - nx[i], dy = ny[j] - ny[i];
            if (dx * dx + dy * dy > maxLength2 && total < s.maxPoints) {
                grown.x.push_back(0.5f * (nx[i] + nx[j]));
                grown.y.push_back(0.5f * (ny[i] + ny[j]));
                grown.born.push_back(static_cast<int32_t>(step));
                ++total;
            }
        }
        chain = std::move(grown);
    }
}

// Differential growth: closed or open polylines whose nodes push each other apart while staying joined, and that split their long segments,
// so a short curve buckles into coral-like folds. Cached and stepped like `particles`, so the result is a pure function of time.
class GrowNode : public Node {
public:
    GrowNode() : Node("grow", 1) {
        declareFloat("startTime", 0.0f);
        declareFloat("stepsPerSecond", 60.0f);
        declareFloat("repel", 6.0f);
        declareFloat("repelRadius", 0.6f);
        declareFloat("attract", 8.0f);
        declareFloat("smooth", 4.0f);
        declareFloat("maxSpeed", 3.0f);
        declareFloat("maxLength", 0.25f);
        declareFloat("maxPoints", 20000.0f);
        declareFloat("useBounds", 0.0f);
        declareFloat("boundsMinX", -10.0f);
        declareFloat("boundsMaxX", 10.0f);
        declareFloat("boundsMinY", -6.0f);
        declareFloat("boundsMaxY", 6.0f);
    }

    bool dependsOnPlayhead() const override { return true; }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const float stepsPerSecond = std::clamp(getFloat("stepsPerSecond"), 10.0f, 1000.0f);
        GrowSettings s;
        s.dt = 1.0f / stepsPerSecond;
        s.repel = std::max(getFloat("repel"), 0.0f);
        s.repelRadius = getFloat("repelRadius");
        s.attract = std::max(getFloat("attract"), 0.0f);
        s.smooth = std::max(getFloat("smooth"), 0.0f);
        s.maxSpeed = std::max(getFloat("maxSpeed"), 0.0f);
        s.maxLength = getFloat("maxLength");
        s.maxPoints = static_cast<size_t>(std::clamp(getFloat("maxPoints"), 1.0f, 5.0e6f));
        s.bounds = getFloat("useBounds") != 0.0f;
        s.minX = getFloat("boundsMinX");
        s.maxX = getFloat("boundsMaxX");
        s.minY = getFloat("boundsMinY");
        s.maxY = getFloat("boundsMaxY");
        if (s.repel > 0.0f && !(s.repelRadius > 0.0f)) throw std::runtime_error("grow: repelRadius must be greater than 0");
        if (!(s.maxLength > 0.0f)) throw std::runtime_error("grow: maxLength must be greater than 0");
        if (s.bounds && (!(s.maxX > s.minX) || !(s.maxY > s.minY))) throw std::runtime_error("grow: the bounds must have a positive width and height");

        const double elapsed = ctx.time - static_cast<double>(getFloat("startTime"));
        const int64_t target = elapsed <= 0.0 ? 0 : static_cast<int64_t>(std::floor(elapsed * stepsPerSecond + 1e-4));
        if (target > maxGrowSteps) throw std::runtime_error("grow: " + std::to_string(target) + " steps to simulate; the limit is " + std::to_string(maxGrowSteps));

        std::vector<float> signature;
        for (const std::string& name : floatNames()) signature.push_back(getFloat(name));
        if (st_.step < 0 || st_.source != in[0] || st_.signature != signature || target < st_.step) {
            st_ = GrowState();
            st_.source = in[0];
            st_.signature = std::move(signature);
            initialiseGrow(st_, *in[0]);
            st_.step = 0;
        }
        for (; st_.step < target; ) advanceGrow(st_, s, ++st_.step, ctx.pool);

        auto geo = std::make_shared<Geometry>();
        geo->resizePoints(pointTotal(st_.chains));
        float* xs = geo->x();
        float* ys = geo->y();
        float* age = geo->attrib(AttribClass::point, "age").floats().data();
        float* u = geo->attrib(AttribClass::point, "u").floats().data();
        int32_t* chainIndex = geo->attrib(AttribClass::point, "chain", AttribType::int32).ints().data();
        size_t at = 0;
        for (size_t c = 0; c < st_.chains.size(); ++c) {
            const Chain& chain = st_.chains[c];
            const size_t count = chain.x.size();
            for (size_t k = 0; k < count; ++k, ++at) {
                xs[at] = chain.x[k];
                ys[at] = chain.y[k];
                age[at] = static_cast<float>(st_.step - chain.born[k]) * s.dt;
                u[at] = chain.closed ? static_cast<float>(k) / static_cast<float>(count) : static_cast<float>(k) / static_cast<float>(count - 1);
                chainIndex[at] = static_cast<int32_t>(c);
            }
            geo->addPrimitive(Primitive{static_cast<uint32_t>(at - count), static_cast<uint32_t>(count), chain.closed, PrimKind::polyline});
        }
        return geo;
    }

private:
    GrowState st_;
};

// A vector field sampled from a regular grid of points (row by row, like the output of `gasField`) with `vx` and `vy` attributes.
struct GridField {
    int nx = 0, ny = 0;
    float x0 = 0.0f, y0 = 0.0f, dx = 1.0f, dy = 1.0f;
    const float* vx = nullptr;
    const float* vy = nullptr;

    void setUp(const Geometry& g) {
        const AttribArray* ax = g.findAttrib(AttribClass::point, "vx");
        const AttribArray* ay = g.findAttrib(AttribClass::point, "vy");
        const size_t n = g.pointCount();
        if (!ax || !ay || ax->type() != AttribType::float32 || ay->type() != AttribType::float32 || n < 4)
            throw std::runtime_error("streamlines: the field input needs float vx and vy point attributes on a grid of points");
        const float* xs = std::as_const(g).x();
        const float* ys = std::as_const(g).y();
        const float step = xs[1] - xs[0];
        if (!(step > 0.0f)) throw std::runtime_error("streamlines: the field input must be a regular grid of points, row by row, left to right");
        size_t columns = 1;
        while (columns < n && std::fabs(ys[columns] - ys[0]) <= 1e-3f * step) ++columns;
        if (columns < 2 || columns == n || n % columns != 0 || !(ys[columns] > ys[0]))
            throw std::runtime_error("streamlines: the field input must be a regular grid of points, row by row, bottom to top");
        nx = static_cast<int>(columns);
        ny = static_cast<int>(n / columns);
        x0 = xs[0];
        y0 = ys[0];
        dx = step;
        dy = ys[columns] - ys[0];
        vx = ax->floats().data();
        vy = ay->floats().data();
    }

    bool sample(float x, float y, float& ox, float& oy) const {
        const float fx = (x - x0) / dx, fy = (y - y0) / dy;
        if (!(fx >= 0.0f && fx <= static_cast<float>(nx - 1) && fy >= 0.0f && fy <= static_cast<float>(ny - 1))) return false;
        const int i = std::min(static_cast<int>(fx), nx - 2), j = std::min(static_cast<int>(fy), ny - 2);
        const float tx = fx - static_cast<float>(i), ty = fy - static_cast<float>(j);
        auto at = [&](const float* v, int a, int b) { return v[static_cast<size_t>(j + b) * static_cast<size_t>(nx) + static_cast<size_t>(i + a)]; };
        auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
        ox = lerp(lerp(at(vx, 0, 0), at(vx, 1, 0), tx), lerp(at(vx, 0, 1), at(vx, 1, 1), tx), ty);
        oy = lerp(lerp(at(vy, 0, 0), at(vy, 1, 0), tx), lerp(at(vy, 0, 1), at(vy, 1, 1), tx), ty);
        return true;
    }
};

struct StreamSettings {
    bool curl = true;
    Fractal fractal;
    float scale = 0.1f, phase = 0.0f, turns = 1.0f;
    const GridField* grid = nullptr;
    float minSpeed = 0.0f;
    bool bounds = false;
    float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
};

// The field direction (unit length) and strength at a point; false outside the domain or where the field vanishes.
bool fieldAt(const StreamSettings& s, float x, float y, float& ux, float& uy, float& mag) {
    if (s.bounds && (x < s.minX || x > s.maxX || y < s.minY || y > s.maxY)) return false;
    if (s.grid) {
        if (!s.grid->sample(x, y, ux, uy)) return false;
    } else if (s.curl) {
        curlOf(s.fractal, x * s.scale, y * s.scale, s.phase, 0.02f, ux, uy);
    } else {
        const float v = s.fractal.sample(x * s.scale, y * s.scale, s.phase) * 0.5f / 0.7f + 0.5f;
        const float angle = v * s.turns * 6.2831853f;
        ux = std::cos(angle);
        uy = std::sin(angle);
    }
    mag = std::sqrt(ux * ux + uy * uy);
    if (!std::isfinite(mag) || mag <= std::max(s.minSpeed, 1e-12f)) return false;
    ux /= mag;
    uy /= mag;
    return true;
}

// Points of the lines traced so far, in a hash of cells `cell` wide, so a new line can keep its distance from them.
class Occupancy {
public:
    explicit Occupancy(float cell) : cell_(cell) {}

    void add(float x, float y, uint32_t seed, int32_t k) {
        const uint32_t id = static_cast<uint32_t>(x_.size());
        x_.push_back(x);
        y_.push_back(y);
        seed_.push_back(seed);
        k_.push_back(k);
        cells_[key(cellOf(x), cellOf(y))].push_back(id);
    }

    void removeLast() {
        const uint32_t id = static_cast<uint32_t>(x_.size() - 1);
        cells_[key(cellOf(x_[id]), cellOf(y_[id]))].pop_back();
        x_.pop_back();
        y_.pop_back();
        seed_.pop_back();
        k_.pop_back();
    }

    // Is a point closer than `distance` (at most one cell) to (x, y), ignoring the points of `seed` within `skip` steps of `k`?
    bool near(float x, float y, float distance, uint32_t seed, int32_t k, int32_t skip) const {
        const int64_t cx = cellOf(x), cy = cellOf(y);
        const float d2 = distance * distance;
        for (int64_t j = cy - 1; j <= cy + 1; ++j) {
            for (int64_t i = cx - 1; i <= cx + 1; ++i) {
                const auto it = cells_.find(key(i, j));
                if (it == cells_.end()) continue;
                for (uint32_t id : it->second) {
                    if (seed_[id] == seed && std::abs(k_[id] - k) <= skip) continue;
                    const float dx = x_[id] - x, dy = y_[id] - y;
                    if (dx * dx + dy * dy < d2) return true;
                }
            }
        }
        return false;
    }

private:
    int64_t cellOf(float v) const { return static_cast<int64_t>(std::floor(v / cell_)); }
    static int64_t key(int64_t i, int64_t j) { return i * 1000003 + j; }

    float cell_;
    std::vector<float> x_, y_;
    std::vector<uint32_t> seed_;
    std::vector<int32_t> k_;
    std::unordered_map<int64_t, std::vector<uint32_t>> cells_;
};

struct Line {
    std::vector<float> x, y, speed;
};

// Curves that follow a vector field: every input point is a seed, traced forwards and/or backwards through the field with a midpoint
// integrator. The field is the curl of a noise (`curl`, never compresses, so lines swirl), the angle of a noise (`angle`, Hobbs's flow field), or,
// with a second input, a grid of points with `vx`/`vy` such as the output of `gasField`. `separation` keeps lines apart (Jobard and Lefer's
// evenly spaced streamlines): a seed closer than that to an earlier line is dropped and a line ends when it comes closer than half of it.
class StreamlinesNode : public Node {
public:
    StreamlinesNode() : Node("streamlines", 1) {
        declareString("field", "curl");
        declareString("direction", "both");
        declareFloat("scale", 0.1f);
        declareFloat("octaves", 3.0f);
        declareFloat("lacunarity", 2.0f);
        declareFloat("gain", 0.5f);
        declareFloat("seed", 1.0f);
        declareFloat("phase", 0.0f);
        declareFloat("turns", 1.0f);
        declareFloat("stepSize", 0.05f);
        declareFloat("maxSteps", 400.0f);
        declareFloat("separation", 0.0f);
        declareFloat("endRatio", 0.5f);
        declareFloat("minSpeed", 0.0f);
        declareFloat("minLength", 0.0f);
        declareFloat("useBounds", 0.0f);
        declareFloat("boundsMinX", -10.0f);
        declareFloat("boundsMaxX", 10.0f);
        declareFloat("boundsMinY", -6.0f);
        declareFloat("boundsMaxY", 6.0f);
    }

    bool hasVariableInputs() const override { return true; }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        const std::string& fieldName = getString("field");
        if (fieldName != "curl" && fieldName != "angle") throw std::runtime_error("streamlines: field must be 'curl' or 'angle', got '" + fieldName + "'");
        const std::string& direction = getString("direction");
        if (direction != "both" && direction != "forward" && direction != "backward")
            throw std::runtime_error("streamlines: direction must be 'both', 'forward' or 'backward', got '" + direction + "'");
        const float h = getFloat("stepSize");
        if (!(h > 0.0f)) throw std::runtime_error("streamlines: stepSize must be greater than 0");
        const int maxSteps = std::clamp(static_cast<int>(getFloat("maxSteps")), 1, 100000);
        const float separation = std::max(getFloat("separation"), 0.0f);
        const float endRatio = std::clamp(getFloat("endRatio"), 0.0f, 1.0f);
        const float minLength = std::max(getFloat("minLength"), 0.0f);

        GridField grid;
        StreamSettings s;
        if (in.size() > 1) {
            grid.setUp(*in[1]);
            s.grid = &grid;
        }
        s.curl = fieldName == "curl";
        s.fractal.octaves = std::clamp(static_cast<int>(getFloat("octaves")), 1, 12);
        s.fractal.lacunarity = getFloat("lacunarity");
        s.fractal.gain = getFloat("gain");
        s.fractal.seed = seedOf(getFloat("seed"));
        s.scale = getFloat("scale");
        s.phase = getFloat("phase");
        s.turns = getFloat("turns");
        s.minSpeed = std::max(getFloat("minSpeed"), 0.0f);
        s.bounds = getFloat("useBounds") != 0.0f;
        s.minX = getFloat("boundsMinX");
        s.maxX = getFloat("boundsMaxX");
        s.minY = getFloat("boundsMinY");
        s.maxY = getFloat("boundsMaxY");
        if (s.bounds && (!(s.maxX > s.minX) || !(s.maxY > s.minY))) throw std::runtime_error("streamlines: the bounds must have a positive width and height");

        const Geometry& seeds = *in[0];
        const float* sx = std::as_const(seeds).x();
        const float* sy = std::as_const(seeds).y();
        const float testDistance = separation * endRatio;
        const int32_t skip = static_cast<int32_t>(std::ceil(testDistance / h)) + 2;
        Occupancy occupancy(std::max(separation, 1e-6f));
        const bool spaced = separation > 0.0f;
        const bool forward = direction != "backward", backward = direction != "forward";

        std::vector<Line> lines;
        std::vector<uint32_t> seedOfLine;
        size_t total = 0;
        for (size_t p = 0; p < seeds.pointCount(); ++p) {
            float ux, uy, mag;
            if (!fieldAt(s, sx[p], sy[p], ux, uy, mag)) continue;
            const uint32_t id = static_cast<uint32_t>(p);
            if (spaced && occupancy.near(sx[p], sy[p], separation, id, 0, -1)) continue;
            size_t registered = 0;
            if (spaced) {
                occupancy.add(sx[p], sy[p], id, 0);
                ++registered;
            }
            Line arms[2];
            for (int arm = 0; arm < 2; ++arm) {
                if ((arm == 0 && !forward) || (arm == 1 && !backward)) continue;
                const float sign = arm == 0 ? 1.0f : -1.0f;
                Line& line = arms[arm];
                float x = sx[p], y = sy[p];
                for (int k = 1; k <= maxSteps; ++k) {
                    float ax, ay, am, bx, by, bm;
                    if (!fieldAt(s, x, y, ax, ay, am)) break;
                    if (!fieldAt(s, x + sign * ax * h * 0.5f, y + sign * ay * h * 0.5f, bx, by, bm)) break;
                    const float nx = x + sign * bx * h, ny = y + sign * by * h;
                    if (!fieldAt(s, nx, ny, ax, ay, am)) break;
                    const int32_t signedK = static_cast<int32_t>(k) * (arm == 0 ? 1 : -1);
                    if (spaced && occupancy.near(nx, ny, testDistance, id, signedK, skip)) break;
                    x = nx;
                    y = ny;
                    line.x.push_back(x);
                    line.y.push_back(y);
                    line.speed.push_back(am);
                    if (spaced) {
                        occupancy.add(x, y, id, signedK);
                        ++registered;
                    }
                }
            }
            Line line;
            for (size_t k = arms[1].x.size(); k-- > 0;) {
                line.x.push_back(arms[1].x[k]);
                line.y.push_back(arms[1].y[k]);
                line.speed.push_back(arms[1].speed[k]);
            }
            line.x.push_back(sx[p]);
            line.y.push_back(sy[p]);
            line.speed.push_back(mag);
            line.x.insert(line.x.end(), arms[0].x.begin(), arms[0].x.end());
            line.y.insert(line.y.end(), arms[0].y.begin(), arms[0].y.end());
            line.speed.insert(line.speed.end(), arms[0].speed.begin(), arms[0].speed.end());

            const float length = h * static_cast<float>(line.x.size() - 1);
            if (line.x.size() < 2 || length < minLength) {
                for (; registered > 0; --registered) occupancy.removeLast();
                continue;
            }
            total += line.x.size();
            if (total > maxStreamPoints) throw std::runtime_error("streamlines: more than 50 million points; lower maxSteps or the seed count");
            lines.push_back(std::move(line));
            seedOfLine.push_back(id);
        }

        auto geo = std::make_shared<Geometry>();
        geo->resizePoints(total);
        float* xs = geo->x();
        float* ys = geo->y();
        float* u = geo->attrib(AttribClass::point, "u").floats().data();
        float* speed = geo->attrib(AttribClass::point, "speed").floats().data();
        int32_t* lineIndex = geo->attrib(AttribClass::point, "line", AttribType::int32).ints().data();
        int32_t* seedIndex = geo->attrib(AttribClass::point, "seed", AttribType::int32).ints().data();
        size_t at = 0;
        for (size_t l = 0; l < lines.size(); ++l) {
            const Line& line = lines[l];
            const size_t count = line.x.size();
            for (size_t k = 0; k < count; ++k, ++at) {
                xs[at] = line.x[k];
                ys[at] = line.y[k];
                u[at] = static_cast<float>(k) / static_cast<float>(count - 1);
                speed[at] = line.speed[k];
                lineIndex[at] = static_cast<int32_t>(l);
                seedIndex[at] = static_cast<int32_t>(seedOfLine[l]);
            }
            geo->addPrimitive(Primitive{static_cast<uint32_t>(at - count), static_cast<uint32_t>(count), false, PrimKind::polyline});
        }
        return geo;
    }
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerGenerativeNodes() {
    add<GrowNode>("grow");
    add<StreamlinesNode>("streamlines");
}

}  // namespace jitnodes
