#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "jitnodes/graph/network.h"
#include "nodeUtil.h"

namespace jitnodes {

namespace {

constexpr int64_t maxSimulationSteps = 2'000'000;

struct GrayScott {
    int width = 0, height = 0;
    float feed = 0.055f, kill = 0.062f, du = 1.0f, dv = 0.5f, dt = 1.0f;
    bool periodic = true;
};

struct FieldState {
    std::vector<float> signature;
    int64_t step = -1;
    std::vector<float> u, v, nu, nv;
};

void seedField(FieldState& st, const GrayScott& g, uint32_t seed, int seedCount, float seedSize) {
    const size_t cells = static_cast<size_t>(g.width) * static_cast<size_t>(g.height);
    st.u.assign(cells, 1.0f);
    st.v.assign(cells, 0.0f);
    const int half = std::max(1, static_cast<int>(seedSize));
    for (int k = 0; k < seedCount; ++k) {
        const int cx = static_cast<int>(random01(seed, static_cast<uint32_t>(k), 1) * static_cast<float>(g.width));
        const int cy = static_cast<int>(random01(seed, static_cast<uint32_t>(k), 2) * static_cast<float>(g.height));
        for (int dy = -half; dy <= half; ++dy)
            for (int dx = -half; dx <= half; ++dx) {
                int x = cx + dx, y = cy + dy;
                if (g.periodic) {
                    x = (x % g.width + g.width) % g.width;
                    y = (y % g.height + g.height) % g.height;
                } else if (x < 0 || y < 0 || x >= g.width || y >= g.height) {
                    continue;
                }
                const size_t i = static_cast<size_t>(y) * static_cast<size_t>(g.width) + static_cast<size_t>(x);
                st.u[i] = 0.5f;
                st.v[i] = 0.25f;
            }
    }
    st.nu = st.u;
    st.nv = st.v;
    st.step = 0;
}

void stepField(FieldState& st, const GrayScott& g, ThreadPool* pool) {
    const int w = g.width, h = g.height;
    auto wrapIndex = [&](int i, int n) { return g.periodic ? (i < 0 ? i + n : i >= n ? i - n : i) : std::clamp(i, 0, n - 1); };
    const size_t rowsPerChunk = std::max<size_t>(1, 4096 / static_cast<size_t>(w));
    parallelChunks(pool, static_cast<size_t>(h), [&](size_t begin, size_t end) {
        for (size_t row = begin; row < end; ++row) {
            const int y = static_cast<int>(row);
            const size_t up = static_cast<size_t>(wrapIndex(y - 1, h)) * static_cast<size_t>(w);
            const size_t mid = row * static_cast<size_t>(w);
            const size_t down = static_cast<size_t>(wrapIndex(y + 1, h)) * static_cast<size_t>(w);
            for (int x = 0; x < w; ++x) {
                const size_t l = static_cast<size_t>(wrapIndex(x - 1, w)), c = static_cast<size_t>(x), r = static_cast<size_t>(wrapIndex(x + 1, w));
                const float u = st.u[mid + c], v = st.v[mid + c];
                const float lapU = 0.2f * (st.u[mid + l] + st.u[mid + r] + st.u[up + c] + st.u[down + c]) +
                                   0.05f * (st.u[up + l] + st.u[up + r] + st.u[down + l] + st.u[down + r]) - u;
                const float lapV = 0.2f * (st.v[mid + l] + st.v[mid + r] + st.v[up + c] + st.v[down + c]) +
                                   0.05f * (st.v[up + l] + st.v[up + r] + st.v[down + l] + st.v[down + r]) - v;
                const float uvv = u * v * v;
                st.nu[mid + c] = std::clamp(u + (g.du * lapU - uvv + g.feed * (1.0f - u)) * g.dt, 0.0f, 1.0f);
                st.nv[mid + c] = std::clamp(v + (g.dv * lapV + uvv - (g.feed + g.kill) * v) * g.dt, 0.0f, 1.0f);
            }
        }
    }, rowsPerChunk);
    st.u.swap(st.nu);
    st.v.swap(st.nv);
}

// The Gray-Scott reaction-diffusion model on a grid: two chemicals, u (food) and v (the catalyst that eats it), diffuse and react into spots,
// stripes, mazes or coral, depending on `feed` and `kill`. The result is a grid of points with attributes u and v. Cached and stepped like
// `particles`: the state at the playhead is a pure function of time, and playing forward only computes the new steps.
class ReactionDiffusionNode : public Node {
public:
    ReactionDiffusionNode() : Node("reactionDiffusion", 0) {
        declareFloat("startTime", 0.0f);
        declareFloat("stepsPerSecond", 400.0f);
        declareFloat("width", 160.0f);
        declareFloat("height", 90.0f);
        declareFloat("cellSize", 0.1f);
        declareFloat("cx", 0.0f);
        declareFloat("cy", 0.0f);
        declareFloat("feed", 0.055f);
        declareFloat("kill", 0.062f);
        declareFloat("diffuseU", 1.0f);
        declareFloat("diffuseV", 0.5f);
        declareFloat("stepSize", 1.0f);
        declareFloat("periodic", 1.0f);
        declareFloat("seed", 1.0f);
        declareFloat("seedCount", 12.0f);
        declareFloat("seedSize", 3.0f);
    }

    bool dependsOnPlayhead() const override { return true; }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>&) override {
        GrayScott g;
        g.width = std::clamp(static_cast<int>(getFloat("width")), 8, 2048);
        g.height = std::clamp(static_cast<int>(getFloat("height")), 8, 2048);
        if (static_cast<int64_t>(g.width) * g.height > 4'000'000) throw std::runtime_error("reactionDiffusion: more than 4 million cells; lower width or height");
        g.feed = getFloat("feed");
        g.kill = getFloat("kill");
        g.du = std::max(getFloat("diffuseU"), 0.0f);
        g.dv = std::max(getFloat("diffuseV"), 0.0f);
        // Explicit diffusion on this 9-point stencil is stable while diffusion * step stays below about 1.25.
        g.dt = std::clamp(getFloat("stepSize"), 0.01f, 1.2f / std::max({g.du, g.dv, 1e-3f}));
        g.periodic = getFloat("periodic") != 0.0f;
        const float cellSize = getFloat("cellSize");
        if (!(cellSize > 0.0f)) throw std::runtime_error("reactionDiffusion: cellSize must be greater than 0");
        const float stepsPerSecond = std::clamp(getFloat("stepsPerSecond"), 1.0f, 20000.0f);
        const double elapsed = ctx.time - static_cast<double>(getFloat("startTime"));
        const int64_t target = elapsed <= 0.0 ? 0 : static_cast<int64_t>(std::floor(elapsed * stepsPerSecond + 1e-4));
        if (target > maxSimulationSteps) throw std::runtime_error("reactionDiffusion: " + std::to_string(target) + " steps to simulate; the limit is " + std::to_string(maxSimulationSteps));

        std::vector<float> signature;
        for (const std::string& name : floatNames()) signature.push_back(getFloat(name));
        FieldState& st = state_;
        if (st.step < 0 || st.signature != signature || target < st.step) {
            st = FieldState();
            st.signature = std::move(signature);
            seedField(st, g, seedOf(getFloat("seed")), std::clamp(static_cast<int>(getFloat("seedCount")), 0, 100000), getFloat("seedSize"));
        }
        for (int64_t step = st.step + 1; step <= target; ++step) stepField(st, g, ctx.pool);
        st.step = target;

        auto geo = std::make_shared<Geometry>();
        const size_t cells = static_cast<size_t>(g.width) * static_cast<size_t>(g.height);
        geo->resizePoints(cells);
        float* xs = geo->x();
        float* ys = geo->y();
        float* u = geo->attrib(AttribClass::point, "u").floats().data();
        float* v = geo->attrib(AttribClass::point, "v").floats().data();
        const float cx = getFloat("cx"), cy = getFloat("cy");
        const float x0 = cx - 0.5f * static_cast<float>(g.width - 1) * cellSize, y0 = cy - 0.5f * static_cast<float>(g.height - 1) * cellSize;
        parallelChunks(ctx.pool, cells, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                xs[i] = x0 + static_cast<float>(i % static_cast<size_t>(g.width)) * cellSize;
                ys[i] = y0 + static_cast<float>(i / static_cast<size_t>(g.width)) * cellSize;
                u[i] = st.u[i];
                v[i] = st.v[i];
            }
        });
        return geo;
    }

private:
    FieldState state_;
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerFieldNodes() { add<ReactionDiffusionNode>("reactionDiffusion"); }

}  // namespace jitnodes
