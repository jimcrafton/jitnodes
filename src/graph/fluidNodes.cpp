#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "nodeUtil.h"
#include "pointGrid.h"
#include "jitnodes/graph/network.h"

namespace jitnodes {

namespace {

constexpr int64_t maxFluidSteps = 100'000;
constexpr size_t maxFluidPoints = 1'000'000;
constexpr float pi = 3.14159265359f;

struct FluidSettings {
    float dt = 1.0f / 120.0f;
    float spacing = 0.1f, h = 0.22f;
    float gravityX = 0.0f, gravityY = -9.8f;
    int iterations = 4;
    float relaxation = 100.0f, tension = 0.0f, viscosity = 0.01f, vorticity = 0.0f, maxSpeed = 30.0f;
    float minX = -5.0f, maxX = 5.0f, minY = -3.0f, maxY = 3.0f;
};

struct FluidState {
    std::vector<float> x, y, vx, vy, density;
    float restDensity = 1.0f;
    int64_t step = -1;
    GeometryHandle source;
    std::vector<float> signature;
};

// 2D poly6 density kernel and the gradient of the spiky kernel, both normalised over the plane.
struct Kernels {
    float h = 1.0f, h2 = 1.0f, poly = 0.0f, spiky = 0.0f;

    explicit Kernels(float radius) : h(radius), h2(radius * radius) {
        poly = 4.0f / (pi * std::pow(radius, 8.0f));
        spiky = -30.0f / (pi * std::pow(radius, 5.0f));
    }
    float w(float r2) const {
        const float d = h2 - r2;
        return d > 0.0f ? poly * d * d * d : 0.0f;
    }
    // Magnitude of the gradient along the unit vector from j to i; zero at and beyond the radius.
    float gradient(float r) const {
        const float d = h - r;
        return d > 0.0f ? spiky * d * d : 0.0f;
    }
};

// The density a particle has inside a square lattice of the given spacing, so a block of fluid laid out that way starts at rest.
float latticeDensity(const Kernels& k, float spacing) {
    const int reach = static_cast<int>(std::ceil(k.h / spacing));
    float sum = 0.0f;
    for (int j = -reach; j <= reach; ++j)
        for (int i = -reach; i <= reach; ++i) sum += k.w(spacing * spacing * static_cast<float>(i * i + j * j));
    return sum;
}

void initialiseFluid(FluidState& st, const Geometry& g, const FluidSettings& s) {
    const size_t n = g.pointCount();
    if (n == 0) throw std::runtime_error("fluid: the input has no points");
    if (n > maxFluidPoints) throw std::runtime_error("fluid: " + std::to_string(n) + " particles; the limit is " + std::to_string(maxFluidPoints));
    const float* xs = std::as_const(g).x();
    const float* ys = std::as_const(g).y();
    st.x.assign(xs, xs + n);
    st.y.assign(ys, ys + n);
    st.vx.assign(n, 0.0f);
    st.vy.assign(n, 0.0f);
    st.density.assign(n, 1.0f);
    const AttribArray* ax = g.findAttrib(AttribClass::point, "vx");
    const AttribArray* ay = g.findAttrib(AttribClass::point, "vy");
    if (ax && ay && ax->type() == AttribType::float32 && ay->type() == AttribType::float32) {
        st.vx.assign(ax->floats().begin(), ax->floats().end());
        st.vy.assign(ay->floats().begin(), ay->floats().end());
    }
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(st.x[i]) || !std::isfinite(st.y[i]) || !std::isfinite(st.vx[i]) || !std::isfinite(st.vy[i]))
            throw std::runtime_error("fluid: point " + std::to_string(i) + " has a position or velocity that is not finite");
        st.x[i] = std::clamp(st.x[i], s.minX, s.maxX);
        st.y[i] = std::clamp(st.y[i], s.minY, s.maxY);
    }
    st.restDensity = latticeDensity(Kernels(s.h), s.spacing);
}

// One step of Position Based Fluids (Macklin and Mueller): predict with gravity, then solve the density constraint of every particle for a few
// Jacobi iterations (an under-dense particle is only pulled in by the fraction `tension`, which acts as surface tension) over neighbour lists fixed at the start of the step, and finish with XSPH viscosity and vorticity confinement. Every sum runs
// in neighbour-list order and every update reads the previous iterate only, so the result does not depend on the thread count.
void advanceFluid(FluidState& st, const FluidSettings& s, ThreadPool* pool) {
    const size_t n = st.x.size();
    const Kernels kern(s.h);
    const float rho0 = st.restDensity;
    std::vector<float> px(n), py(n), lambda(n), dx(n), dy(n);
    for (size_t i = 0; i < n; ++i) {
        st.vx[i] += s.gravityX * s.dt;
        st.vy[i] += s.gravityY * s.dt;
        const float speed = std::sqrt(st.vx[i] * st.vx[i] + st.vy[i] * st.vy[i]);
        if (speed > s.maxSpeed) {
            st.vx[i] *= s.maxSpeed / speed;
            st.vy[i] *= s.maxSpeed / speed;
        }
        px[i] = std::clamp(st.x[i] + st.vx[i] * s.dt, s.minX, s.maxX);
        py[i] = std::clamp(st.y[i] + st.vy[i] * s.dt, s.minY, s.maxY);
    }

    PointGrid grid;
    grid.build(px.data(), py.data(), n, s.h);
    std::vector<uint32_t> start(n + 1, 0), neighbours;
    std::vector<uint32_t> counts(n, 0);
    const float h2 = s.h * s.h;
    auto forNeighbours = [&](size_t i, auto&& fn) {
        grid.forEachNear(px[i], py[i], [&](uint32_t j) {
            if (j == i) return;
            const float ddx = px[i] - px[j], ddy = py[i] - py[j];
            if (ddx * ddx + ddy * ddy < h2) fn(j);
        });
    };
    parallelChunks(pool, n, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) forNeighbours(i, [&](uint32_t) { ++counts[i]; });
    });
    for (size_t i = 0; i < n; ++i) start[i + 1] = start[i] + counts[i];
    neighbours.resize(start[n]);
    parallelChunks(pool, n, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            uint32_t at = start[i];
            forNeighbours(i, [&](uint32_t j) { neighbours[at++] = j; });
        }
    });

    // A clump of overlapping particles would otherwise be pushed apart in one jump and leave with a huge velocity.
    const float maxMove = 0.1f * s.spacing;
    for (int iteration = 0; iteration < s.iterations; ++iteration) {
        parallelChunks(pool, n, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                float rho = kern.w(0.0f), sumGrad2 = 0.0f, gix = 0.0f, giy = 0.0f;
                for (uint32_t k = start[i]; k < start[i + 1]; ++k) {
                    const uint32_t j = neighbours[k];
                    const float ex = px[i] - px[j], ey = py[i] - py[j];
                    const float r2 = ex * ex + ey * ey, r = std::sqrt(r2);
                    rho += kern.w(r2);
                    if (r < 1e-9f) continue;
                    const float g = kern.gradient(r) / (rho0 * r);
                    sumGrad2 += g * g * r2;
                    gix += g * ex;
                    giy += g * ey;
                }
                const float c0 = rho / rho0 - 1.0f;
                const float c = c0 > 0.0f ? c0 : s.tension * c0;
                lambda[i] = -c / (sumGrad2 + gix * gix + giy * giy + s.relaxation);
                st.density[i] = rho / rho0;
            }
        });
        parallelChunks(pool, n, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                float mx = 0.0f, my = 0.0f;
                for (uint32_t k = start[i]; k < start[i + 1]; ++k) {
                    const uint32_t j = neighbours[k];
                    const float ex = px[i] - px[j], ey = py[i] - py[j];
                    const float r2 = ex * ex + ey * ey, r = std::sqrt(r2);
                    if (r < 1e-9f) continue;
                    const float g = (lambda[i] + lambda[j]) * kern.gradient(r) / (rho0 * r);
                    mx += g * ex;
                    my += g * ey;
                }
                const float len = std::sqrt(mx * mx + my * my);
                if (len > maxMove) {
                    mx *= maxMove / len;
                    my *= maxMove / len;
                }
                dx[i] = mx;
                dy[i] = my;
            }
        });
        parallelChunks(pool, n, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                px[i] = std::clamp(px[i] + dx[i], s.minX, s.maxX);
                py[i] = std::clamp(py[i] + dy[i], s.minY, s.maxY);
            }
        });
    }

    std::vector<float> nvx(n), nvy(n), omega(n, 0.0f);
    const float inv = 1.0f / s.dt;
    for (size_t i = 0; i < n; ++i) {
        nvx[i] = (px[i] - st.x[i]) * inv;
        nvy[i] = (py[i] - st.y[i]) * inv;
    }
    const bool viscous = s.viscosity > 0.0f, swirly = s.vorticity > 0.0f;
    std::vector<float> vx(nvx), vy(nvy);
    if (viscous || swirly) {
        parallelChunks(pool, n, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                float ax = 0.0f, ay = 0.0f, w = 0.0f;
                for (uint32_t k = start[i]; k < start[i + 1]; ++k) {
                    const uint32_t j = neighbours[k];
                    const float ex = px[i] - px[j], ey = py[i] - py[j];
                    const float r2 = ex * ex + ey * ey, r = std::sqrt(r2);
                    const float dvx = nvx[j] - nvx[i], dvy = nvy[j] - nvy[i];
                    ax += dvx * kern.w(r2);
                    ay += dvy * kern.w(r2);
                    if (swirly && r > 1e-9f) {
                        const float g = kern.gradient(r) / r;
                        w += dvx * (g * ey) - dvy * (g * ex);
                    }
                }
                if (viscous) {
                    vx[i] = nvx[i] + s.viscosity * ax / rho0;
                    vy[i] = nvy[i] + s.viscosity * ay / rho0;
                }
                omega[i] = w / rho0;
            }
        });
    }
    if (swirly) {
        std::vector<float> fx(n, 0.0f), fy(n, 0.0f);
        parallelChunks(pool, n, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                float ex = 0.0f, ey = 0.0f;
                for (uint32_t k = start[i]; k < start[i + 1]; ++k) {
                    const uint32_t j = neighbours[k];
                    const float ux = px[i] - px[j], uy = py[i] - py[j];
                    const float r = std::sqrt(ux * ux + uy * uy);
                    if (r < 1e-9f) continue;
                    const float g = (std::fabs(omega[j]) - std::fabs(omega[i])) * kern.gradient(r) / (rho0 * r);
                    ex += g * ux;
                    ey += g * uy;
                }
                const float len = std::sqrt(ex * ex + ey * ey);
                if (len < 1e-9f) continue;
                fx[i] = s.vorticity * omega[i] * (ey / len);
                fy[i] = -s.vorticity * omega[i] * (ex / len);
            }
        });
        for (size_t i = 0; i < n; ++i) {
            vx[i] += fx[i] * s.dt;
            vy[i] += fy[i] * s.dt;
        }
    }
    st.x.swap(px);
    st.y.swap(py);
    st.vx.swap(vx);
    st.vy.swap(vy);
}

// A liquid made of particles (Position Based Fluids): the input points are the particles, laid out on a lattice of `spacing` they start at rest
// density. Cached and stepped like `particles`, so the result is a pure function of time. Outputs `vx`, `vy`, `speed` and `density`
// (1 at rest density).
class FluidNode : public Node {
public:
    FluidNode() : Node("fluid", 1) {
        declareFloat("startTime", 0.0f);
        declareFloat("stepsPerSecond", 120.0f);
        declareFloat("spacing", 0.1f);
        declareFloat("radiusScale", 2.2f);
        declareFloat("gravityX", 0.0f);
        declareFloat("gravityY", -9.8f);
        declareFloat("iterations", 4.0f);
        declareFloat("relaxation", 100.0f);
        declareFloat("tension", 0.0f);
        declareFloat("viscosity", 0.01f);
        declareFloat("vorticity", 0.0f);
        declareFloat("maxSpeed", 30.0f);
        declareFloat("boundsMinX", -5.0f);
        declareFloat("boundsMaxX", 5.0f);
        declareFloat("boundsMinY", -3.0f);
        declareFloat("boundsMaxY", 3.0f);
    }

    bool dependsOnPlayhead() const override { return true; }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const float stepsPerSecond = std::clamp(getFloat("stepsPerSecond"), 10.0f, 1000.0f);
        FluidSettings s;
        s.dt = 1.0f / stepsPerSecond;
        s.spacing = getFloat("spacing");
        if (!(s.spacing > 0.0f)) throw std::runtime_error("fluid: spacing must be greater than 0");
        const float radiusScale = getFloat("radiusScale");
        if (!(radiusScale >= 1.2f)) throw std::runtime_error("fluid: radiusScale must be at least 1.2");
        s.h = s.spacing * radiusScale;
        s.gravityX = getFloat("gravityX");
        s.gravityY = getFloat("gravityY");
        s.iterations = std::clamp(static_cast<int>(getFloat("iterations")), 1, 50);
        s.relaxation = std::max(getFloat("relaxation"), 1e-3f);
        s.tension = std::clamp(getFloat("tension"), 0.0f, 1.0f);
        s.viscosity = std::clamp(getFloat("viscosity"), 0.0f, 1.0f);
        s.vorticity = std::max(getFloat("vorticity"), 0.0f);
        s.maxSpeed = std::max(getFloat("maxSpeed"), 1e-3f);
        s.minX = getFloat("boundsMinX");
        s.maxX = getFloat("boundsMaxX");
        s.minY = getFloat("boundsMinY");
        s.maxY = getFloat("boundsMaxY");
        if (!(s.maxX > s.minX) || !(s.maxY > s.minY)) throw std::runtime_error("fluid: the bounds must have a positive width and height");

        const double elapsed = ctx.time - static_cast<double>(getFloat("startTime"));
        const int64_t target = elapsed <= 0.0 ? 0 : static_cast<int64_t>(std::floor(elapsed * stepsPerSecond + 1e-4));
        if (target > maxFluidSteps) throw std::runtime_error("fluid: " + std::to_string(target) + " steps to simulate; the limit is " + std::to_string(maxFluidSteps));

        std::vector<float> signature;
        for (const std::string& name : floatNames()) signature.push_back(getFloat(name));
        const bool changed = st_.step < 0 || st_.source != in[0] || st_.signature != signature;
        if (changed) checkpoints_.clear();
        const auto* saved = !changed ? checkpoints_.resumeFor(st_.step, target) : nullptr;
        if (saved) {
            st_ = saved->state;
            st_.step = saved->step;
        } else if (changed || target < st_.step) {
            st_ = FluidState();
            st_.source = in[0];
            st_.signature = std::move(signature);
            initialiseFluid(st_, *in[0], s);
            st_.step = 0;
            checkpoints_.offer(0, st_, vectorBytes(st_.x) * 5);
        }
        for (; st_.step < target;) {
            advanceFluid(st_, s, ctx.pool);
            ++st_.step;
            checkpoints_.offer(st_.step, st_, vectorBytes(st_.x) * 5);
        }

        const size_t n = st_.x.size();
        auto geo = std::make_shared<Geometry>();
        geo->resizePoints(n);
        float* xs = geo->x();
        float* ys = geo->y();
        float* vx = geo->attrib(AttribClass::point, "vx").floats().data();
        float* vy = geo->attrib(AttribClass::point, "vy").floats().data();
        float* speed = geo->attrib(AttribClass::point, "speed").floats().data();
        float* density = geo->attrib(AttribClass::point, "density").floats().data();
        for (size_t i = 0; i < n; ++i) {
            xs[i] = st_.x[i];
            ys[i] = st_.y[i];
            vx[i] = st_.vx[i];
            vy[i] = st_.vy[i];
            speed[i] = std::sqrt(st_.vx[i] * st_.vx[i] + st_.vy[i] * st_.vy[i]);
            density[i] = st_.density[i];
        }
        return geo;
    }

private:
    FluidState st_;
    StateCheckpoints<FluidState> checkpoints_;
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerFluidNodes() { add<FluidNode>("fluid"); }

}  // namespace jitnodes
