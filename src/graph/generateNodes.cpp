#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "jitnodes/geo/topology.h"
#include "jitnodes/graph/network.h"
#include "nodeUtil.h"

namespace jitnodes {

namespace {

constexpr size_t maxGeneratedPoints = 50'000'000;
constexpr float twoPi = 6.28318530718f;
constexpr int maxInsideAttempts = 256;

size_t countOf(float value, const char* node) {
    if (!(value >= 0.0f)) return 0;
    if (value > static_cast<float>(maxGeneratedPoints)) throw std::runtime_error(std::string(node) + ": count exceeds " + std::to_string(maxGeneratedPoints));
    return static_cast<size_t>(value);
}

struct RandomChannels {
    float* rand;
    float* rand2;
};

RandomChannels addRandomChannels(Geometry& geo) {
    return {geo.attrib(AttribClass::point, "rand").floats().data(), geo.attrib(AttribClass::point, "rand2").floats().data()};
}

class GenerateNode : public Node {
public:
    GenerateNode() : Node("generate", 0) {
        declareFloat("count", 100000.0f);
        declareFloat("seed", 1.0f);
        declareString("shape", "rect");
        declareFloat("cx", 0.0f);
        declareFloat("cy", 0.0f);
        declareFloat("width", 20.0f);
        declareFloat("height", 10.0f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>&) override {
        const size_t n = countOf(getFloat("count"), "generate");
        const std::string& shape = getString("shape");
        if (shape != "rect" && shape != "disc" && shape != "gaussian") throw std::runtime_error("generate: shape must be 'rect', 'disc' or 'gaussian', got '" + shape + "'");
        const uint32_t seed = seedOf(getFloat("seed"));
        const float cx = getFloat("cx"), cy = getFloat("cy");
        const float halfW = getFloat("width") * 0.5f, halfH = getFloat("height") * 0.5f;
        const int kind = shape == "rect" ? 0 : shape == "disc" ? 1 : 2;

        auto geo = std::make_shared<Geometry>();
        geo->resizePoints(n);
        float* xs = geo->x();
        float* ys = geo->y();
        const RandomChannels rnd = addRandomChannels(*geo);
        int32_t* ids = geo->attrib(AttribClass::point, "id", AttribType::int32).ints().data();

        parallelChunks(ctx.pool, n, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const uint32_t k = static_cast<uint32_t>(i);
                const float u = random01(seed, k, 0), v = random01(seed, k, 1);
                float px, py;
                if (kind == 0) {
                    px = (u * 2.0f - 1.0f) * halfW;
                    py = (v * 2.0f - 1.0f) * halfH;
                } else if (kind == 1) {
                    const float r = std::sqrt(u), a = twoPi * v;
                    px = r * std::cos(a) * halfW;
                    py = r * std::sin(a) * halfH;
                } else {
                    // Box-Muller; the box is about three standard deviations to each side.
                    const float r = std::sqrt(-2.0f * std::log(std::max(u, 1e-7f))), a = twoPi * v;
                    px = r * std::cos(a) * halfW / 3.0f;
                    py = r * std::sin(a) * halfH / 3.0f;
                }
                xs[i] = cx + px;
                ys[i] = cy + py;
                rnd.rand[i] = random01(seed, k, 2);
                rnd.rand2[i] = random01(seed, k, 3);
                ids[i] = static_cast<int32_t>(i);
            }
        });
        return geo;
    }
};

// A polygon to fill or a polyline to walk, with the slice of the output points that belong to it.
struct Region {
    std::vector<float> px, py;
    std::vector<float> along;  // running length at each vertex (open polylines)
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    double measure = 0.0;  // area, or length
    bool closed = false;
    uint32_t prim = 0;
    size_t first = 0;
    size_t count = 0;
};

bool insidePolygon(const Region& r, float x, float y) {
    bool in = false;
    const size_t n = r.px.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        if ((r.py[i] > y) != (r.py[j] > y) && x < (r.px[j] - r.px[i]) * (y - r.py[i]) / (r.py[j] - r.py[i]) + r.px[i]) in = !in;
    }
    return in;
}

// Distributes total over the regions in proportion to their measure; the counts add up to total exactly.
void allocate(std::vector<Region*>& regions, size_t total, size_t& firstOut) {
    double sum = 0.0;
    for (const Region* r : regions) sum += r->measure;
    double running = 0.0;
    size_t given = 0;
    for (Region* r : regions) {
        running += r->measure;
        const size_t upTo = sum > 0.0 ? static_cast<size_t>(std::llround(running / sum * static_cast<double>(total))) : 0;
        r->count = upTo - given;
        r->first = firstOut + given;
        given = upTo;
    }
    firstOut += total;
}

class ScatterNode : public Node {
public:
    ScatterNode() : Node("scatter", 1) {
        declareFloat("count", 10000.0f);
        declareFloat("lineCount", 1000.0f);
        declareFloat("spread", 0.0f);
        declareFloat("seed", 1.0f);
        declareFloat("tolerance", 0.1f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const size_t insideTotal = countOf(getFloat("count"), "scatter");
        const size_t lineTotal = countOf(getFloat("lineCount"), "scatter");
        const float spread = getFloat("spread");
        const uint32_t seed = seedOf(getFloat("seed"));

        Geometry flatCopy;
        const Geometry* shape = in[0].get();
        if (hasCurves(*shape)) {
            flatCopy = flattenCurves(*shape, getFloat("tolerance"));
            shape = &flatCopy;
        }

        std::vector<Region> regions;
        const float* sx = shape->pointCount() ? shape->x() : nullptr;
        const float* sy = shape->pointCount() ? shape->y() : nullptr;
        uint32_t primIndex = 0;
        for (const Primitive& p : shape->primitives()) {
            Region r;
            r.prim = primIndex++;
            r.closed = p.closed;
            if (p.count < (p.closed ? 3u : 2u)) continue;
            r.px.assign(sx + p.start, sx + p.start + p.count);
            r.py.assign(sy + p.start, sy + p.start + p.count);
            r.minX = *std::min_element(r.px.begin(), r.px.end());
            r.maxX = *std::max_element(r.px.begin(), r.px.end());
            r.minY = *std::min_element(r.py.begin(), r.py.end());
            r.maxY = *std::max_element(r.py.begin(), r.py.end());
            if (p.closed) {
                double twice = 0.0;
                for (size_t i = 0, j = r.px.size() - 1; i < r.px.size(); j = i++)
                    twice += static_cast<double>(r.px[j]) * r.py[i] - static_cast<double>(r.px[i]) * r.py[j];
                r.measure = std::fabs(twice) * 0.5;
            } else {
                r.along.assign(r.px.size(), 0.0f);
                double length = 0.0;
                for (size_t i = 1; i < r.px.size(); ++i) {
                    length += std::hypot(static_cast<double>(r.px[i] - r.px[i - 1]), static_cast<double>(r.py[i] - r.py[i - 1]));
                    r.along[i] = static_cast<float>(length);
                }
                r.measure = length;
            }
            if (r.measure > 1e-12) regions.push_back(std::move(r));
        }

        std::vector<Region*> filled, walked;
        for (Region& r : regions) (r.closed ? filled : walked).push_back(&r);
        size_t total = 0;
        if (!filled.empty()) allocate(filled, insideTotal, total);
        if (!walked.empty()) allocate(walked, lineTotal, total);

        auto geo = std::make_shared<Geometry>();
        geo->resizePoints(total);
        float* xs = geo->x();
        float* ys = geo->y();
        const RandomChannels rnd = addRandomChannels(*geo);
        int32_t* srcPrim = geo->attrib(AttribClass::point, "srcPrim", AttribType::int32).ints().data();
        float* along = geo->attrib(AttribClass::point, "u").floats().data();

        // Regions are few relative to points, so each task handles one region's slice in chunks.
        std::vector<std::pair<const Region*, std::pair<size_t, size_t>>> tasks;
        constexpr size_t chunk = 16384;
        for (const Region& r : regions)
            for (size_t b = 0; b < r.count; b += chunk) tasks.push_back({&r, {b, std::min(r.count, b + chunk)}});

        auto run = [&](size_t t) {
            const Region& r = *tasks[t].first;
            for (size_t j = tasks[t].second.first; j < tasks[t].second.second; ++j) {
                const size_t out = r.first + j;
                const uint32_t k = static_cast<uint32_t>(out);
                float x, y, u = 0.0f;
                if (r.closed) {
                    const float w = r.maxX - r.minX, h = r.maxY - r.minY;
                    bool found = false;
                    x = r.px[0];
                    y = r.py[0];
                    for (int attempt = 0; attempt < maxInsideAttempts && !found; ++attempt) {
                        const float cx = r.minX + random01(seed, k, 10 + 2 * attempt) * w;
                        const float cy = r.minY + random01(seed, k, 11 + 2 * attempt) * h;
                        if (insidePolygon(r, cx, cy)) {
                            x = cx;
                            y = cy;
                            found = true;
                        }
                    }
                } else {
                    const float d = random01(seed, k, 10) * static_cast<float>(r.measure);
                    size_t seg = static_cast<size_t>(std::upper_bound(r.along.begin(), r.along.end(), d) - r.along.begin());
                    seg = std::clamp<size_t>(seg == 0 ? 0 : seg - 1, 0, r.px.size() - 2);
                    const float dx = r.px[seg + 1] - r.px[seg], dy = r.py[seg + 1] - r.py[seg];
                    const float len = std::hypot(dx, dy);
                    const float f = len > 1e-20f ? std::clamp((d - r.along[seg]) / len, 0.0f, 1.0f) : 0.0f;
                    x = r.px[seg] + dx * f;
                    y = r.py[seg] + dy * f;
                    if (spread != 0.0f && len > 1e-20f) {
                        const float off = (random01(seed, k, 11) * 2.0f - 1.0f) * spread;
                        x += -dy / len * off;
                        y += dx / len * off;
                    }
                    u = d / static_cast<float>(r.measure);
                }
                xs[out] = x;
                ys[out] = y;
                along[out] = u;
                srcPrim[out] = static_cast<int32_t>(r.prim);
                rnd.rand[out] = random01(seed, k, 4);
                rnd.rand2[out] = random01(seed, k, 5);
            }
        };
        if (ctx.pool && tasks.size() > 1) ctx.pool->parallelFor(tasks.size(), run);
        else for (size_t t = 0; t < tasks.size(); ++t) run(t);
        return geo;
    }
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerGenerateNodes() {
    add<GenerateNode>("generate");
    add<ScatterNode>("scatter");
}

}  // namespace jitnodes
