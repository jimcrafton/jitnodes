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

int64_t targetStep(const char* who, double time, float startTime, float stepsPerSecond) {
    const double elapsed = time - static_cast<double>(startTime);
    const int64_t target = elapsed <= 0.0 ? 0 : static_cast<int64_t>(std::floor(elapsed * stepsPerSecond + 1e-4));
    if (target > maxSimulationSteps) throw std::runtime_error(std::string(who) + ": " + std::to_string(target) + " steps to simulate; the limit is " + std::to_string(maxSimulationSteps));
    return target;
}

void limitLength(float& x, float& y, float maxLength) {
    const float l2 = x * x + y * y;
    if (l2 > maxLength * maxLength) {
        const float k = maxLength / std::sqrt(l2);
        x *= k;
        y *= k;
    }
}

// Reynolds' steering: the force that turns velocity (vx, vy) towards the desired direction, at most `maxForce` strong.
void steer(float dirX, float dirY, float vx, float vy, float maxSpeed, float maxForce, float& outX, float& outY) {
    const float l = std::hypot(dirX, dirY);
    if (l < 1e-9f) {
        outX = outY = 0.0f;
        return;
    }
    outX = dirX / l * maxSpeed - vx;
    outY = dirY / l * maxSpeed - vy;
    limitLength(outX, outY, maxForce);
}

struct BoidsSettings {
    float dt = 1.0f / 60.0f;
    float radius = 1.0f, separationRadius = 0.4f;
    float separation = 1.5f, alignment = 1.0f, cohesion = 1.0f;
    float minSpeed = 1.0f, maxSpeed = 3.0f, maxForce = 6.0f;
    float goalX = 0.0f, goalY = 0.0f, goal = 0.0f;
    float wander = 0.0f, wanderScale = 0.2f, wanderSpeed = 0.2f;
    float avoid = 3.0f, avoidDistance = 1.0f, obstacleRadius = 0.5f;
    Boundary boundary = Boundary::wrap;
    float minX = -10.0f, maxX = 10.0f, minY = -6.0f, maxY = 6.0f;
    uint32_t seed = 1;
    Fractal field;
};

struct BoidsState {
    GeometryHandle source;
    std::vector<GeometryHandle> obstacleSources;
    std::vector<float> signature;
    std::string boundary;
    int64_t step = -1;
    std::vector<float> x, y, vx, vy, nx, ny, nvx, nvy, neighbors;
    std::vector<float> ox, oy, orad;
    std::vector<float> baseX, baseY, ovx, ovy;
    bool obstaclesMove = false;
    float obstacleReach = 0.0f;
    PointGrid obstacleGrid;
};

// Circles from the points of the obstacle inputs: the point's `radius` attribute, or the fallback for points without one.
// `vx`/`vy` give an obstacle a constant velocity: it sits at its input position at the start time and drifts from there.
void gatherObstacles(BoidsState& st, const std::vector<GeometryHandle>& in, const BoidsSettings& s) {
    for (size_t k = 1; k < in.size(); ++k) {
        const Geometry& g = *in[k];
        const AttribArray* radius = g.findAttrib(AttribClass::point, "radius");
        const AttribArray* avx = g.findAttrib(AttribClass::point, "vx");
        const AttribArray* avy = g.findAttrib(AttribClass::point, "vy");
        for (size_t i = 0; i < g.pointCount(); ++i) {
            st.baseX.push_back(std::as_const(g).x()[i]);
            st.baseY.push_back(std::as_const(g).y()[i]);
            st.ovx.push_back(avx ? avx->at(i) : 0.0f);
            st.ovy.push_back(avy ? avy->at(i) : 0.0f);
            st.orad.push_back(radius ? std::max(radius->at(i), 0.0f) : s.obstacleRadius);
            st.obstacleReach = std::max(st.obstacleReach, st.orad.back());
            st.obstaclesMove = st.obstaclesMove || st.ovx.back() != 0.0f || st.ovy.back() != 0.0f;
        }
    }
    st.ox = st.baseX;
    st.oy = st.baseY;
}

void placeObstacles(BoidsState& st, const BoidsSettings& s, float simTime) {
    if (st.ox.empty()) return;
    for (size_t j = 0; j < st.ox.size(); ++j) {
        st.ox[j] = st.baseX[j] + st.ovx[j] * simTime;
        st.oy[j] = st.baseY[j] + st.ovy[j] * simTime;
    }
    st.obstacleGrid.build(st.ox.data(), st.oy.data(), st.ox.size(), std::max(st.obstacleReach + s.avoidDistance, 1e-3f));
}

void initialiseBoids(BoidsState& st, const Geometry& in, const BoidsSettings& s) {
    const size_t n = in.pointCount();
    st.x.assign(std::as_const(in).x(), std::as_const(in).x() + n);
    st.y.assign(std::as_const(in).y(), std::as_const(in).y() + n);
    st.vx.assign(n, 0.0f);
    st.vy.assign(n, 0.0f);
    const AttribArray* avx = in.findAttrib(AttribClass::point, "vx");
    const AttribArray* avy = in.findAttrib(AttribClass::point, "vy");
    const float speed = 0.5f * (s.minSpeed + s.maxSpeed);
    for (size_t i = 0; i < n; ++i) {
        if (avx || avy) {
            st.vx[i] = avx ? avx->at(i) : 0.0f;
            st.vy[i] = avy ? avy->at(i) : 0.0f;
        } else {
            const float a = twoPi * random01(s.seed, static_cast<uint32_t>(i), 1);
            st.vx[i] = std::cos(a) * speed;
            st.vy[i] = std::sin(a) * speed;
        }
    }
    st.nx = st.x;
    st.ny = st.y;
    st.nvx = st.vx;
    st.nvy = st.vy;
    st.neighbors.assign(n, 0.0f);
    st.step = 0;
}

void advanceBoids(BoidsState& st, const BoidsSettings& s, int64_t step, ThreadPool* pool) {
    const size_t n = st.x.size();
    const float simTime = static_cast<float>(step) * s.dt;
    PointGrid grid;
    grid.build(st.x.data(), st.y.data(), n, s.radius);
    const float r2 = s.radius * s.radius, sr2 = s.separationRadius * s.separationRadius;
    const float width = s.maxX - s.minX, height = s.maxY - s.minY;
    const bool haveObstacles = !st.ox.empty();
    if (st.obstaclesMove) placeObstacles(st, s, simTime);
    parallelChunks(pool, n, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const float px = st.x[i], py = st.y[i];
            float vx = st.vx[i], vy = st.vy[i];
            float sepX = 0.0f, sepY = 0.0f, aliX = 0.0f, aliY = 0.0f, cohX = 0.0f, cohY = 0.0f;
            int count = 0;
            grid.forEachNear(px, py, [&](uint32_t j) {
                if (j == i) return;
                const float dx = px - st.x[j], dy = py - st.y[j];
                const float d2 = dx * dx + dy * dy;
                if (!(d2 < r2)) return;
                ++count;
                aliX += st.vx[j];
                aliY += st.vy[j];
                cohX += st.x[j];
                cohY += st.y[j];
                if (d2 < sr2 && d2 > 1e-12f) {
                    sepX += dx / d2;
                    sepY += dy / d2;
                }
            });
            float ax = 0.0f, ay = 0.0f, fx, fy;
            if (count > 0) {
                const float inv = 1.0f / static_cast<float>(count);
                steer(sepX, sepY, vx, vy, s.maxSpeed, s.maxForce, fx, fy);
                ax += fx * s.separation;
                ay += fy * s.separation;
                steer(aliX * inv, aliY * inv, vx, vy, s.maxSpeed, s.maxForce, fx, fy);
                ax += fx * s.alignment;
                ay += fy * s.alignment;
                steer(cohX * inv - px, cohY * inv - py, vx, vy, s.maxSpeed, s.maxForce, fx, fy);
                ax += fx * s.cohesion;
                ay += fy * s.cohesion;
            }
            if (s.goal != 0.0f) {
                steer(s.goalX - px, s.goalY - py, vx, vy, s.maxSpeed, s.maxForce, fx, fy);
                ax += fx * s.goal;
                ay += fy * s.goal;
            }
            if (haveObstacles && s.avoid != 0.0f) {
                st.obstacleGrid.forEachNear(px, py, [&](uint32_t j) {
                    const float dx = px - st.ox[j], dy = py - st.oy[j];
                    const float d = std::sqrt(dx * dx + dy * dy);
                    const float reach = st.orad[j] + s.avoidDistance;
                    if (!(d < reach)) return;
                    const float weight = d <= st.orad[j] ? 1.0f : (reach - d) / std::max(s.avoidDistance, 1e-6f);
                    float awayX = d > 1e-9f ? dx : 1.0f, awayY = d > 1e-9f ? dy : 0.0f;
                    steer(awayX, awayY, vx, vy, s.maxSpeed, s.maxForce, fx, fy);
                    ax += fx * s.avoid * weight;
                    ay += fy * s.avoid * weight;
                });
            }
            if (s.wander != 0.0f) {
                float cx, cy;
                curlOf(s.field, px * s.wanderScale, py * s.wanderScale, simTime * s.wanderSpeed, 0.02f, cx, cy);
                ax += cx * s.wander;
                ay += cy * s.wander;
            }
            vx += ax * s.dt;
            vy += ay * s.dt;
            const float speed = std::hypot(vx, vy);
            if (speed > s.maxSpeed) {
                vx *= s.maxSpeed / speed;
                vy *= s.maxSpeed / speed;
            } else if (speed < s.minSpeed) {
                if (speed > 1e-9f) {
                    vx *= s.minSpeed / speed;
                    vy *= s.minSpeed / speed;
                } else {
                    vx = s.minSpeed;
                }
            }
            float x = px + vx * s.dt, y = py + vy * s.dt;
            if (haveObstacles) {
                // A boid that ends up inside an obstacle is moved to its surface and loses the velocity into it.
                st.obstacleGrid.forEachNear(x, y, [&](uint32_t j) {
                    const float dx = x - st.ox[j], dy = y - st.oy[j];
                    const float d = std::sqrt(dx * dx + dy * dy);
                    if (!(d < st.orad[j])) return;
                    const float nx = d > 1e-9f ? dx / d : 1.0f, ny = d > 1e-9f ? dy / d : 0.0f;
                    x = st.ox[j] + nx * (st.orad[j] + 1e-3f);
                    y = st.oy[j] + ny * (st.orad[j] + 1e-3f);
                    const float inward = (vx - st.ovx[j]) * nx + (vy - st.ovy[j]) * ny;
                    if (inward < 0.0f) {
                        vx -= inward * nx;
                        vy -= inward * ny;
                    }
                });
            }
            if (s.boundary == Boundary::bounce) {
                if (x < s.minX || x > s.maxX) {
                    vx = -vx;
                    x = std::clamp(x < s.minX ? 2.0f * s.minX - x : 2.0f * s.maxX - x, s.minX, s.maxX);
                }
                if (y < s.minY || y > s.maxY) {
                    vy = -vy;
                    y = std::clamp(y < s.minY ? 2.0f * s.minY - y : 2.0f * s.maxY - y, s.minY, s.maxY);
                }
            } else if (s.boundary == Boundary::wrap) {
                x = s.minX + (x - s.minX) - std::floor((x - s.minX) / width) * width;
                y = s.minY + (y - s.minY) - std::floor((y - s.minY) / height) * height;
            }
            st.nx[i] = x;
            st.ny[i] = y;
            st.nvx[i] = vx;
            st.nvy[i] = vy;
            st.neighbors[i] = static_cast<float>(count);
        }
    }, 256);
    st.x.swap(st.nx);
    st.y.swap(st.ny);
    st.vx.swap(st.nvx);
    st.vy.swap(st.nvy);
}

// A flock: every input point is a boid that steers by three rules (keep apart, match the neighbours' heading, move towards their centre)
// plus an optional goal and wander noise, between `minSpeed` and `maxSpeed`. Cached and stepped like `particles`: a pure function of time.
class BoidsNode : public Node {
public:
    BoidsNode() : Node("boids", 2) {
        declareFloat("startTime", 0.0f);
        declareFloat("stepsPerSecond", 60.0f);
        declareFloat("seed", 1.0f);
        declareFloat("radius", 1.0f);
        declareFloat("separationRadius", 0.4f);
        declareFloat("separation", 1.5f);
        declareFloat("alignment", 1.0f);
        declareFloat("cohesion", 1.0f);
        declareFloat("minSpeed", 1.0f);
        declareFloat("maxSpeed", 3.0f);
        declareFloat("maxForce", 6.0f);
        declareFloat("goalX", 0.0f);
        declareFloat("goalY", 0.0f);
        declareFloat("goal", 0.0f);
        declareFloat("wander", 0.0f);
        declareFloat("wanderScale", 0.2f);
        declareFloat("wanderSpeed", 0.2f);
        declareFloat("avoid", 3.0f);
        declareFloat("avoidDistance", 1.0f);
        declareFloat("obstacleRadius", 0.5f);
        declareString("boundary", "wrap");
        declareFloat("boundsMinX", -10.0f);
        declareFloat("boundsMaxX", 10.0f);
        declareFloat("boundsMinY", -6.0f);
        declareFloat("boundsMaxY", 6.0f);
    }

    bool dependsOnPlayhead() const override { return true; }
    bool hasVariableInputs() const override { return true; }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const std::string& boundary = getString("boundary");
        if (boundary != "none" && boundary != "bounce" && boundary != "wrap")
            throw std::runtime_error("boids: boundary must be 'none', 'bounce' or 'wrap', got '" + boundary + "'");
        BoidsSettings s;
        const float stepsPerSecond = std::clamp(getFloat("stepsPerSecond"), 10.0f, 1000.0f);
        s.dt = 1.0f / stepsPerSecond;
        s.seed = seedOf(getFloat("seed"));
        s.radius = getFloat("radius");
        if (!(s.radius > 0.0f)) throw std::runtime_error("boids: radius must be greater than 0");
        s.separationRadius = std::max(getFloat("separationRadius"), 0.0f);
        s.separation = getFloat("separation");
        s.alignment = getFloat("alignment");
        s.cohesion = getFloat("cohesion");
        s.maxSpeed = std::max(getFloat("maxSpeed"), 1e-3f);
        s.minSpeed = std::clamp(getFloat("minSpeed"), 0.0f, s.maxSpeed);
        s.maxForce = std::max(getFloat("maxForce"), 0.0f);
        s.goalX = getFloat("goalX");
        s.goalY = getFloat("goalY");
        s.goal = getFloat("goal");
        s.wander = getFloat("wander");
        s.wanderScale = getFloat("wanderScale");
        s.wanderSpeed = getFloat("wanderSpeed");
        s.avoid = getFloat("avoid");
        s.avoidDistance = std::max(getFloat("avoidDistance"), 0.0f);
        s.obstacleRadius = std::max(getFloat("obstacleRadius"), 0.0f);
        s.boundary = boundary == "bounce" ? Boundary::bounce : boundary == "wrap" ? Boundary::wrap : Boundary::none;
        s.minX = getFloat("boundsMinX");
        s.maxX = getFloat("boundsMaxX");
        s.minY = getFloat("boundsMinY");
        s.maxY = getFloat("boundsMaxY");
        if (s.boundary != Boundary::none && (!(s.maxX > s.minX) || !(s.maxY > s.minY))) throw std::runtime_error("boids: the bounds must have a positive width and height");
        s.field.octaves = 2;
        s.field.seed = s.seed;

        const int64_t target = targetStep("boids", ctx.time, getFloat("startTime"), stepsPerSecond);
        std::vector<float> signature;
        for (const std::string& name : floatNames()) signature.push_back(getFloat(name));
        BoidsState& st = state_;
        const std::vector<GeometryHandle> obstacleSources(in.begin() + 1, in.end());
        if (st.step < 0 || st.source != in[0] || st.obstacleSources != obstacleSources || st.signature != signature || st.boundary != boundary || target < st.step) {
            st = BoidsState();
            st.source = in[0];
            st.obstacleSources = obstacleSources;
            st.signature = std::move(signature);
            st.boundary = boundary;
            initialiseBoids(st, *in[0], s);
            gatherObstacles(st, in, s);
            placeObstacles(st, s, 0.0f);
        }
        for (int64_t step = st.step + 1; step <= target; ++step) advanceBoids(st, s, step, ctx.pool);
        st.step = target;

        auto geo = std::make_shared<Geometry>(*in[0]);
        const size_t n = geo->pointCount();
        float* xs = geo->x();
        float* ys = geo->y();
        float* vx = geo->attrib(AttribClass::point, "vx").floats().data();
        float* vy = geo->attrib(AttribClass::point, "vy").floats().data();
        float* speed = geo->attrib(AttribClass::point, "speed").floats().data();
        float* angle = geo->attrib(AttribClass::point, "angle").floats().data();
        float* neighbors = geo->attrib(AttribClass::point, "neighbors").floats().data();
        parallelChunks(ctx.pool, n, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                xs[i] = st.x[i];
                ys[i] = st.y[i];
                vx[i] = st.vx[i];
                vy[i] = st.vy[i];
                speed[i] = std::hypot(st.vx[i], st.vy[i]);
                angle[i] = std::atan2(st.vy[i], st.vx[i]);
                neighbors[i] = st.neighbors[i];
            }
        });
        return geo;
    }

private:
    BoidsState state_;
};

struct Spring {
    uint32_t a, b;
    float rest;
};

struct SpringSettings {
    float dt = 1.0f / 120.0f;
    float gravityX = 0.0f, gravityY = -4.0f;
    float dampFactor = 1.0f;
    float stiffness = 1.0f;
    int iterations = 8;
    float windForce = 0.0f, windScale = 0.2f, windSpeed = 0.3f;
    bool bounds = false;
    float minX = -10.0f, maxX = 10.0f, minY = -6.0f, maxY = 6.0f;
    uint32_t seed = 1;
    Fractal field;
};

struct SpringState {
    GeometryHandle source;
    std::vector<float> signature;
    int64_t step = -1;
    std::vector<float> x, y, px, py;
    std::vector<uint8_t> pinned;
    std::vector<Spring> springs;
};

void initialiseSprings(SpringState& st, const Geometry& in, const SpringSettings& s, float restScale, float neighborRadius, bool pinEnds) {
    const size_t n = in.pointCount();
    st.x.assign(std::as_const(in).x(), std::as_const(in).x() + n);
    st.y.assign(std::as_const(in).y(), std::as_const(in).y() + n);
    st.px = st.x;
    st.py = st.y;
    const AttribArray* avx = in.findAttrib(AttribClass::point, "vx");
    const AttribArray* avy = in.findAttrib(AttribClass::point, "vy");
    for (size_t i = 0; i < n; ++i) {
        if (avx) st.px[i] -= avx->at(i) * s.dt;
        if (avy) st.py[i] -= avy->at(i) * s.dt;
    }
    st.pinned.assign(n, 0);
    if (const AttribArray* pin = in.findAttrib(AttribClass::point, "pin"))
        for (size_t i = 0; i < n; ++i) st.pinned[i] = pin->at(i) != 0.0f ? 1 : 0;

    auto addSpring = [&](uint32_t a, uint32_t b) {
        if (a == b) return;
        const float d = std::hypot(st.x[a] - st.x[b], st.y[a] - st.y[b]);
        st.springs.push_back({a, b, d * restScale});
    };
    for (const Primitive& p : in.primitives()) {
        for (uint32_t k = 0; k + 1 < p.count; ++k) addSpring(p.start + k, p.start + k + 1);
        if (p.closed && p.count > 2) addSpring(p.start + p.count - 1, p.start);
        if (pinEnds && !p.closed && p.count > 0) st.pinned[p.start] = 1;
    }
    if (neighborRadius > 0.0f && n > 1) {
        PointGrid grid;
        grid.build(st.x.data(), st.y.data(), n, neighborRadius);
        const float r2 = neighborRadius * neighborRadius;
        for (uint32_t i = 0; i < n; ++i) {
            grid.forEachNear(st.x[i], st.y[i], [&](uint32_t j) {
                if (j <= i) return;
                const float dx = st.x[i] - st.x[j], dy = st.y[i] - st.y[j];
                if (dx * dx + dy * dy < r2) addSpring(i, j);
                if (st.springs.size() > 20'000'000) throw std::runtime_error("springs: more than 20 million springs; lower neighborRadius");
            });
        }
    }
    st.step = 0;
}

void advanceSprings(SpringState& st, const SpringSettings& s, int64_t step, ThreadPool* pool) {
    const size_t n = st.x.size();
    const float simTime = static_cast<float>(step) * s.dt;
    const float dt2 = s.dt * s.dt;
    parallelChunks(pool, n, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            if (st.pinned[i]) continue;
            float ax = s.gravityX, ay = s.gravityY;
            if (s.windForce != 0.0f) {
                float cx, cy;
                curlOf(s.field, st.x[i] * s.windScale, st.y[i] * s.windScale, simTime * s.windSpeed, 0.02f, cx, cy);
                ax += cx * s.windForce;
                ay += cy * s.windForce;
            }
            const float x = st.x[i], y = st.y[i];
            st.x[i] = x + (x - st.px[i]) * s.dampFactor + ax * dt2;
            st.y[i] = y + (y - st.py[i]) * s.dampFactor + ay * dt2;
            st.px[i] = x;
            st.py[i] = y;
        }
    });
    // Position-based dynamics: each spring moves its two ends towards its rest length. Done in order, so the result does not depend on threads.
    for (int it = 0; it < s.iterations; ++it) {
        for (const Spring& sp : st.springs) {
            const float wa = st.pinned[sp.a] ? 0.0f : 1.0f, wb = st.pinned[sp.b] ? 0.0f : 1.0f;
            if (wa + wb == 0.0f) continue;
            const float dx = st.x[sp.b] - st.x[sp.a], dy = st.y[sp.b] - st.y[sp.a];
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < 1e-9f) continue;
            const float k = (d - sp.rest) / d * s.stiffness / (wa + wb);
            st.x[sp.a] += dx * k * wa;
            st.y[sp.a] += dy * k * wa;
            st.x[sp.b] -= dx * k * wb;
            st.y[sp.b] -= dy * k * wb;
        }
    }
    if (s.bounds) {
        for (size_t i = 0; i < n; ++i) {
            if (st.pinned[i]) continue;
            st.x[i] = std::clamp(st.x[i], s.minX, s.maxX);
            st.y[i] = std::clamp(st.y[i], s.minY, s.maxY);
        }
    }
}

// Ropes, chains, cloth and soft blobs: the points of every input primitive are joined by springs (and, with `neighborRadius`, so are points that
// start close together). Verlet integration with position-based springs, which stay stable however stiff. Points with a nonzero `pin` attribute
// stay where they are. Cached and stepped like `particles`: a pure function of time.
class SpringsNode : public Node {
public:
    SpringsNode() : Node("springs", 1) {
        declareFloat("startTime", 0.0f);
        declareFloat("stepsPerSecond", 120.0f);
        declareFloat("seed", 1.0f);
        declareFloat("gravityX", 0.0f);
        declareFloat("gravityY", -4.0f);
        declareFloat("damping", 0.2f);
        declareFloat("stiffness", 1.0f);
        declareFloat("iterations", 8.0f);
        declareFloat("restScale", 1.0f);
        declareFloat("neighborRadius", 0.0f);
        declareFloat("pinEnds", 0.0f);
        declareString("output", "points");
        declareFloat("windForce", 0.0f);
        declareFloat("windScale", 0.2f);
        declareFloat("windSpeed", 0.3f);
        declareFloat("useBounds", 0.0f);
        declareFloat("boundsMinX", -10.0f);
        declareFloat("boundsMaxX", 10.0f);
        declareFloat("boundsMinY", -6.0f);
        declareFloat("boundsMaxY", 6.0f);
    }

    bool dependsOnPlayhead() const override { return true; }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const std::string& output = getString("output");
        if (output != "points" && output != "springs") throw std::runtime_error("springs: output must be 'points' or 'springs', got '" + output + "'");
        SpringSettings s;
        const float stepsPerSecond = std::clamp(getFloat("stepsPerSecond"), 10.0f, 2000.0f);
        s.dt = 1.0f / stepsPerSecond;
        s.seed = seedOf(getFloat("seed"));
        s.gravityX = getFloat("gravityX");
        s.gravityY = getFloat("gravityY");
        s.dampFactor = std::exp(-std::max(getFloat("damping"), 0.0f) * s.dt);
        s.stiffness = std::clamp(getFloat("stiffness"), 0.0f, 1.0f);
        s.iterations = std::clamp(static_cast<int>(getFloat("iterations")), 1, 100);
        s.windForce = getFloat("windForce");
        s.windScale = getFloat("windScale");
        s.windSpeed = getFloat("windSpeed");
        s.bounds = getFloat("useBounds") != 0.0f;
        s.minX = getFloat("boundsMinX");
        s.maxX = getFloat("boundsMaxX");
        s.minY = getFloat("boundsMinY");
        s.maxY = getFloat("boundsMaxY");
        if (s.bounds && (!(s.maxX > s.minX) || !(s.maxY > s.minY))) throw std::runtime_error("springs: the bounds must have a positive width and height");
        const float restScale = getFloat("restScale");
        if (!(restScale > 0.0f)) throw std::runtime_error("springs: restScale must be greater than 0");
        const float neighborRadius = std::max(getFloat("neighborRadius"), 0.0f);
        s.field.octaves = 2;
        s.field.seed = s.seed;

        const int64_t target = targetStep("springs", ctx.time, getFloat("startTime"), stepsPerSecond);
        std::vector<float> signature;
        for (const std::string& name : floatNames()) signature.push_back(getFloat(name));
        SpringState& st = state_;
        if (st.step < 0 || st.source != in[0] || st.signature != signature || target < st.step) {
            st = SpringState();
            st.source = in[0];
            st.signature = std::move(signature);
            initialiseSprings(st, *in[0], s, restScale, neighborRadius, getFloat("pinEnds") != 0.0f);
        }
        for (int64_t step = st.step + 1; step <= target; ++step) advanceSprings(st, s, step, ctx.pool);
        st.step = target;

        auto geo = std::make_shared<Geometry>(*in[0]);
        const size_t n = geo->pointCount();
        float* xs = geo->x();
        float* ys = geo->y();
        float* vx = geo->attrib(AttribClass::point, "vx").floats().data();
        float* vy = geo->attrib(AttribClass::point, "vy").floats().data();
        float* speed = geo->attrib(AttribClass::point, "speed").floats().data();
        float* strain = geo->attrib(AttribClass::point, "strain").floats().data();
        std::vector<float> sum(n, 0.0f), count(n, 0.0f);
        for (const Spring& sp : st.springs) {
            if (!(sp.rest > 1e-9f)) continue;
            const float e = (std::hypot(st.x[sp.b] - st.x[sp.a], st.y[sp.b] - st.y[sp.a]) - sp.rest) / sp.rest;
            sum[sp.a] += e;
            sum[sp.b] += e;
            count[sp.a] += 1.0f;
            count[sp.b] += 1.0f;
        }
        parallelChunks(ctx.pool, n, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                xs[i] = st.x[i];
                ys[i] = st.y[i];
                vx[i] = st.pinned[i] ? 0.0f : (st.x[i] - st.px[i]) / s.dt;
                vy[i] = st.pinned[i] ? 0.0f : (st.y[i] - st.py[i]) / s.dt;
                speed[i] = std::hypot(vx[i], vy[i]);
                strain[i] = count[i] > 0.0f ? sum[i] / count[i] : 0.0f;
            }
        });
        if (output == "points") return geo;

        // One two-point line per spring, so a mesh can be drawn, sampled with scatter or measured; each end keeps its point's attributes.
        auto lines = std::make_shared<Geometry>();
        const size_t springCount = st.springs.size();
        lines->resizePoints(2 * springCount);
        for (const auto& entry : geo->attribs(AttribClass::point)) {
            const AttribArray& source = entry.second;
            AttribArray& destination = lines->attrib(AttribClass::point, entry.first, source.type());
            if (source.type() == AttribType::float32) {
                float* dst = destination.floats().data();
                const float* src = source.floats().data();
                parallelChunks(ctx.pool, springCount, [&](size_t begin, size_t end) {
                    for (size_t k = begin; k < end; ++k) {
                        dst[2 * k] = src[st.springs[k].a];
                        dst[2 * k + 1] = src[st.springs[k].b];
                    }
                });
            } else {
                int32_t* dst = destination.ints().data();
                const int32_t* src = source.ints().data();
                parallelChunks(ctx.pool, springCount, [&](size_t begin, size_t end) {
                    for (size_t k = begin; k < end; ++k) {
                        dst[2 * k] = src[st.springs[k].a];
                        dst[2 * k + 1] = src[st.springs[k].b];
                    }
                });
            }
        }
        for (const auto& entry : geo->details()) lines->setDetail(entry.first, entry.second);
        if (springCount == 0) return lines;
        float* springStrain = lines->attrib(AttribClass::point, "strain").floats().data();
        int32_t* springIndex = lines->attrib(AttribClass::point, "spring", AttribType::int32).ints().data();
        parallelChunks(ctx.pool, springCount, [&](size_t begin, size_t end) {
            for (size_t k = begin; k < end; ++k) {
                const Spring& sp = st.springs[k];
                const float e = sp.rest > 1e-9f ? (std::hypot(st.x[sp.b] - st.x[sp.a], st.y[sp.b] - st.y[sp.a]) - sp.rest) / sp.rest : 0.0f;
                springStrain[2 * k] = springStrain[2 * k + 1] = e;
                springIndex[2 * k] = springIndex[2 * k + 1] = static_cast<int32_t>(k);
            }
        });
        for (size_t k = 0; k < springCount; ++k) lines->addPrimitive({static_cast<uint32_t>(2 * k), 2u, false, PrimKind::polyline});
        return lines;
    }

private:
    SpringState state_;
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerParticleNodes() {
    add<ParticlesNode>("particles");
    add<BoidsNode>("boids");
    add<SpringsNode>("springs");
}

}  // namespace jitnodes
