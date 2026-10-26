#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "jitnodes/graph/network.h"
#include "noise.h"
#include "nodeUtil.h"
#include "pointGrid.h"

namespace jitnodes {

namespace {

constexpr float twoPi = 6.28318530718f;
constexpr int64_t maxSimulationSteps = 1'000'000;

enum class Boundary { none, bounce, wrap };

struct Settings {
    float dt = 1.0f / 120.0f;
    float gravityX = 0.0f, gravityY = 0.0f;
    float dragFactor = 1.0f;
    float noiseForce = 0.0f, noiseScale = 0.2f, noiseSpeed = 0.2f;
    float attractX = 0.0f, attractY = 0.0f, attract = 0.0f, softening = 0.5f, swirl = 0.0f;
    float repel = 0.0f, repelRadius = 0.3f;
    float lifetime = 0.0f;
    float initialSpeed = 0.0f, speedVariation = 0.0f, direction = 0.0f, spread = 360.0f;
    Boundary boundary = Boundary::none;
    float minX = -10.0f, maxX = 10.0f, minY = -6.0f, maxY = 6.0f, bounce = 0.6f, friction = 0.0f;
    uint32_t seed = 1;
    Fractal field;
};

struct State {
    GeometryHandle source;
    std::vector<float> signature;
    std::string boundary;
    int64_t step = -1;  // -1: nothing simulated yet
    std::vector<float> x, y, vx, vy, age, x0, y0, vx0, vy0;
    std::vector<uint32_t> generation;
};

// The velocity a particle starts with, or restarts with after its `generation`th respawn.
void emission(const Settings& s, const State& st, size_t i, uint32_t generation, float& vx, float& vy) {
    vx = st.vx0[i];
    vy = st.vy0[i];
    if (s.initialSpeed == 0.0f) return;
    const float speed = s.initialSpeed * (1.0f + s.speedVariation * (random01(s.seed, static_cast<uint32_t>(i), 100 + 2 * generation) * 2.0f - 1.0f));
    const float angle = (s.direction + (random01(s.seed, static_cast<uint32_t>(i), 101 + 2 * generation) - 0.5f) * s.spread) * (twoPi / 360.0f);
    vx += speed * std::cos(angle);
    vy += speed * std::sin(angle);
}

void initialise(State& st, const Geometry& in, const Settings& s) {
    const size_t n = in.pointCount();
    st.x.assign(std::as_const(in).x(), std::as_const(in).x() + n);
    st.y.assign(std::as_const(in).y(), std::as_const(in).y() + n);
    st.x0 = st.x;
    st.y0 = st.y;
    st.vx0.assign(n, 0.0f);
    st.vy0.assign(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        if (const AttribArray* a = in.findAttrib(AttribClass::point, "vx")) st.vx0[i] = a->at(i);
        if (const AttribArray* a = in.findAttrib(AttribClass::point, "vy")) st.vy0[i] = a->at(i);
    }
    st.vx.assign(n, 0.0f);
    st.vy.assign(n, 0.0f);
    st.age.assign(n, 0.0f);
    st.generation.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
        emission(s, st, i, 0, st.vx[i], st.vy[i]);
        if (s.lifetime > 0.0f) st.age[i] = s.lifetime * random01(s.seed, static_cast<uint32_t>(i), 50);
    }
    st.step = 0;
}

void advance(State& st, const Settings& s, int64_t step, ThreadPool* pool, std::vector<float>& pushX, std::vector<float>& pushY) {
    const size_t n = st.x.size();
    const float dt = s.dt;
    const float simTime = static_cast<float>(step) * dt;
    const bool repelOn = s.repel != 0.0f && s.repelRadius > 0.0f;
    if (repelOn) {
        PointGrid grid;
        grid.build(st.x.data(), st.y.data(), n, s.repelRadius);
        const float r2 = s.repelRadius * s.repelRadius;
        pushX.assign(n, 0.0f);
        pushY.assign(n, 0.0f);
        parallelChunks(pool, n, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                float px = 0.0f, py = 0.0f;
                grid.forEachNear(st.x[i], st.y[i], [&](uint32_t j) {
                    if (j == i) return;
                    float dx = st.x[i] - st.x[j], dy = st.y[i] - st.y[j];
                    const float d2 = dx * dx + dy * dy;
                    if (!(d2 < r2)) return;
                    const float d = std::sqrt(d2);
                    if (d < 1e-6f) {
                        const float a = twoPi * random01(s.seed, static_cast<uint32_t>(std::min<size_t>(i, j)) * 0x9E3779B1u ^ static_cast<uint32_t>(std::max<size_t>(i, j)), 7);
                        dx = std::cos(a) * (i < j ? 1.0f : -1.0f);
                        dy = std::sin(a) * (i < j ? 1.0f : -1.0f);
                    } else {
                        dx /= d;
                        dy /= d;
                    }
                    const float push = (s.repelRadius - d) / s.repelRadius * s.repel;
                    px += dx * push;
                    py += dy * push;
                });
                pushX[i] = px;
                pushY[i] = py;
            }
        });
    }

    const float width = s.maxX - s.minX, height = s.maxY - s.minY;
    parallelChunks(pool, n, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            float x = st.x[i], y = st.y[i], vx = st.vx[i], vy = st.vy[i];
            float ax = s.gravityX, ay = s.gravityY;
            if (s.attract != 0.0f || s.swirl != 0.0f) {
                const float dx = s.attractX - x, dy = s.attractY - y;
                const float r2 = dx * dx + dy * dy + s.softening * s.softening;
                const float inv = 1.0f / (r2 * std::sqrt(r2));
                ax += (s.attract * dx - s.swirl * dy) * inv;
                ay += (s.attract * dy + s.swirl * dx) * inv;
            }
            if (s.noiseForce != 0.0f) {
                float cx, cy;
                curlOf(s.field, x * s.noiseScale, y * s.noiseScale, simTime * s.noiseSpeed, 0.02f, cx, cy);
                ax += cx * s.noiseForce;
                ay += cy * s.noiseForce;
            }
            if (repelOn) {
                ax += pushX[i];
                ay += pushY[i];
            }
            vx = (vx + ax * dt) * s.dragFactor;
            vy = (vy + ay * dt) * s.dragFactor;
            x += vx * dt;
            y += vy * dt;

            if (s.boundary == Boundary::bounce) {
                if (x < s.minX) {
                    x = s.minX + (s.minX - x);
                    vx = -vx * s.bounce;
                    vy *= 1.0f - s.friction;
                } else if (x > s.maxX) {
                    x = s.maxX - (x - s.maxX);
                    vx = -vx * s.bounce;
                    vy *= 1.0f - s.friction;
                }
                if (y < s.minY) {
                    y = s.minY + (s.minY - y);
                    vy = -vy * s.bounce;
                    vx *= 1.0f - s.friction;
                } else if (y > s.maxY) {
                    y = s.maxY - (y - s.maxY);
                    vy = -vy * s.bounce;
                    vx *= 1.0f - s.friction;
                }
                x = std::clamp(x, s.minX, s.maxX);
                y = std::clamp(y, s.minY, s.maxY);
            } else if (s.boundary == Boundary::wrap) {
                if (width > 0.0f) x = s.minX + (x - s.minX) - std::floor((x - s.minX) / width) * width;
                if (height > 0.0f) y = s.minY + (y - s.minY) - std::floor((y - s.minY) / height) * height;
            }

            float age = st.age[i];
            if (s.lifetime > 0.0f) {
                age += dt;
                if (age >= s.lifetime) {
                    age -= std::floor(age / s.lifetime) * s.lifetime;
                    const uint32_t g = ++st.generation[i];
                    x = st.x0[i];
                    y = st.y0[i];
                    emission(s, st, i, g, vx, vy);
                }
            } else {
                age += dt;
            }
            st.x[i] = x;
            st.y[i] = y;
            st.vx[i] = vx;
            st.vy[i] = vy;
            st.age[i] = age;
        }
    });
}

// A particle simulation: the input points are the particles at time `startTime`, and the result is where they are at the playhead after
// fixed steps of force integration. The state of the latest cook is kept, so playing forward only advances the new steps; scrubbing back
// or changing anything restarts from the input. The same time always gives the same result.
class ParticlesNode : public Node {
public:
    ParticlesNode() : Node("particles", 1) {
        declareFloat("startTime", 0.0f);
        declareFloat("stepsPerSecond", 120.0f);
        declareFloat("seed", 1.0f);
        declareFloat("gravityX", 0.0f);
        declareFloat("gravityY", -2.0f);
        declareFloat("drag", 0.1f);
        declareFloat("noiseForce", 0.0f);
        declareFloat("noiseScale", 0.2f);
        declareFloat("noiseSpeed", 0.2f);
        declareFloat("attractX", 0.0f);
        declareFloat("attractY", 0.0f);
        declareFloat("attract", 0.0f);
        declareFloat("attractSoftening", 0.5f);
        declareFloat("swirl", 0.0f);
        declareFloat("repel", 0.0f);
        declareFloat("repelRadius", 0.3f);
        declareFloat("lifetime", 0.0f);
        declareFloat("initialSpeed", 0.0f);
        declareFloat("speedVariation", 0.0f);
        declareFloat("direction", 90.0f);
        declareFloat("spread", 360.0f);
        declareString("boundary", "none");
        declareFloat("boundsMinX", -10.0f);
        declareFloat("boundsMaxX", 10.0f);
        declareFloat("boundsMinY", -6.0f);
        declareFloat("boundsMaxY", 6.0f);
        declareFloat("bounce", 0.6f);
        declareFloat("friction", 0.0f);
    }

    bool dependsOnPlayhead() const override { return true; }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const std::string& boundary = getString("boundary");
        if (boundary != "none" && boundary != "bounce" && boundary != "wrap")
            throw std::runtime_error("particles: boundary must be 'none', 'bounce' or 'wrap', got '" + boundary + "'");

        Settings s;
        const float stepsPerSecond = std::clamp(getFloat("stepsPerSecond"), 10.0f, 2000.0f);
        s.dt = 1.0f / stepsPerSecond;
        s.seed = seedOf(getFloat("seed"));
        s.gravityX = getFloat("gravityX");
        s.gravityY = getFloat("gravityY");
        s.dragFactor = std::exp(-std::max(getFloat("drag"), 0.0f) * s.dt);
        s.noiseForce = getFloat("noiseForce");
        s.noiseScale = getFloat("noiseScale");
        s.noiseSpeed = getFloat("noiseSpeed");
        s.attractX = getFloat("attractX");
        s.attractY = getFloat("attractY");
        s.attract = getFloat("attract");
        s.softening = std::max(getFloat("attractSoftening"), 1e-3f);
        s.swirl = getFloat("swirl");
        s.repel = getFloat("repel");
        s.repelRadius = getFloat("repelRadius");
        s.lifetime = std::max(getFloat("lifetime"), 0.0f);
        s.initialSpeed = getFloat("initialSpeed");
        s.speedVariation = getFloat("speedVariation");
        s.direction = getFloat("direction");
        s.spread = getFloat("spread");
        s.boundary = boundary == "bounce" ? Boundary::bounce : boundary == "wrap" ? Boundary::wrap : Boundary::none;
        s.minX = getFloat("boundsMinX");
        s.maxX = getFloat("boundsMaxX");
        s.minY = getFloat("boundsMinY");
        s.maxY = getFloat("boundsMaxY");
        if (s.boundary != Boundary::none && (!(s.maxX > s.minX) || !(s.maxY > s.minY))) throw std::runtime_error("particles: the bounds must have a positive width and height");
        s.bounce = getFloat("bounce");
        s.friction = getFloat("friction");
        s.field.octaves = 2;
        s.field.seed = s.seed;

        const double elapsed = ctx.time - static_cast<double>(getFloat("startTime"));
        const int64_t target = elapsed <= 0.0 ? 0 : static_cast<int64_t>(std::floor(elapsed * stepsPerSecond + 1e-4));
        if (target > maxSimulationSteps) throw std::runtime_error("particles: " + std::to_string(target) + " steps to simulate; the limit is " + std::to_string(maxSimulationSteps));

        std::vector<float> signature;
        for (const std::string& name : floatNames()) signature.push_back(getFloat(name));
        State& st = state_;
        if (st.step < 0 || st.source != in[0] || st.signature != signature || st.boundary != boundary || target < st.step) {
            st = State();
            st.source = in[0];
            st.signature = std::move(signature);
            st.boundary = boundary;
            initialise(st, *in[0], s);
        }
        std::vector<float> pushX, pushY;
        for (int64_t step = st.step + 1; step <= target; ++step) advance(st, s, step, ctx.pool, pushX, pushY);
        st.step = target;

        auto geo = std::make_shared<Geometry>(*in[0]);
        const size_t n = geo->pointCount();
        float* xs = geo->x();
        float* ys = geo->y();
        float* vx = geo->attrib(AttribClass::point, "vx").floats().data();
        float* vy = geo->attrib(AttribClass::point, "vy").floats().data();
        float* age = geo->attrib(AttribClass::point, "age").floats().data();
        float* life = geo->attrib(AttribClass::point, "life").floats().data();
        float* speed = geo->attrib(AttribClass::point, "speed").floats().data();
        parallelChunks(ctx.pool, n, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                xs[i] = st.x[i];
                ys[i] = st.y[i];
                vx[i] = st.vx[i];
                vy[i] = st.vy[i];
                age[i] = st.age[i];
                life[i] = s.lifetime > 0.0f ? st.age[i] / s.lifetime : 0.0f;
                speed[i] = std::hypot(st.vx[i], st.vy[i]);
            }
        });
        return geo;
    }

private:
    State state_;
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerParticleNodes() { add<ParticlesNode>("particles"); }

}  // namespace jitnodes
