#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "gasSolver.h"
#include "jitnodes/graph/network.h"

namespace jitnodes {

namespace {

constexpr int64_t maxGasSteps = 100'000;

// The injectors of the source inputs: one per point, with the point's `density`, `heat`, `radius`, `vx` and `vy` where it has them.
std::vector<GasSource> gatherSources(const std::vector<GeometryHandle>& in, float density, float heat, float radius) {
    std::vector<GasSource> out;
    for (const GeometryHandle& handle : in) {
        const Geometry& g = *handle;
        const AttribArray* d = g.findAttrib(AttribClass::point, "density");
        const AttribArray* t = g.findAttrib(AttribClass::point, "heat");
        const AttribArray* r = g.findAttrib(AttribClass::point, "radius");
        const AttribArray* vx = g.findAttrib(AttribClass::point, "vx");
        const AttribArray* vy = g.findAttrib(AttribClass::point, "vy");
        for (size_t i = 0; i < g.pointCount(); ++i) {
            GasSource s;
            s.x = std::as_const(g).x()[i];
            s.y = std::as_const(g).y()[i];
            s.density = d ? d->at(i) : density;
            s.heat = t ? t->at(i) : heat;
            s.radius = r ? r->at(i) : radius;
            s.vx = vx ? vx->at(i) : 0.0f;
            s.vy = vy ? vy->at(i) : 0.0f;
            out.push_back(s);
        }
    }
    return out;
}

std::vector<float> sourceContent(const std::vector<GasSource>& sources) {
    std::vector<float> out;
    for (const GasSource& s : sources) out.insert(out.end(), {s.x, s.y, s.radius, s.density, s.heat, s.vx, s.vy});
    return out;
}

// A 2D gas (smoke) on a grid. The input points are injectors; the result is the grid at the playhead, one point per cell with
// `density`, `temperature`, `vx`, `vy` and `speed`. Stepped from `startTime` and cached like `particles`, so a pure function of time.
class GasFieldNode : public Node {
public:
    GasFieldNode() : Node("gasField", 1) {
        declareFloat("startTime", 0.0f);
        declareFloat("stepsPerSecond", 60.0f);
        declareFloat("density", 1.0f);
        declareFloat("heat", 0.0f);
        declareFloat("radius", 0.5f);
        declareFloat("boundsMinX", -10.0f);
        declareFloat("boundsMaxX", 10.0f);
        declareFloat("boundsMinY", -6.0f);
        declareFloat("boundsMaxY", 6.0f);
        forEachGasParam("", [&](const std::string& name, float value) { declareFloat(name, value); });
    }

    bool dependsOnPlayhead() const override { return true; }
    bool hasVariableInputs() const override { return true; }
    bool inputsOptional() const override { return true; }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const GasConfig config = readGasConfig(*this, "", "gasField");
        const float stepsPerSecond = std::clamp(getFloat("stepsPerSecond"), 10.0f, 500.0f);
        const float dt = 1.0f / stepsPerSecond;
        std::vector<GasSource> sources = gatherSources(in, getFloat("density"), getFloat("heat"), std::max(getFloat("radius"), 1e-3f));
        GasSource nozzle;
        if (readGasNozzle(*this, "", nozzle)) sources.push_back(nozzle);

        const double elapsed = ctx.time - static_cast<double>(getFloat("startTime"));
        const int64_t target = elapsed <= 0.0 ? 0 : static_cast<int64_t>(std::floor(elapsed * stepsPerSecond + 1e-4));
        if (target > maxGasSteps) throw std::runtime_error("gasField: " + std::to_string(target) + " steps to simulate; the limit is " + std::to_string(maxGasSteps));

        std::vector<float> signature;
        for (const std::string& name : floatNames()) signature.push_back(getFloat(name));
        std::vector<float> content = sourceContent(sources);
        if (step_ < 0 || signature != signature_ || content != content_ || target < step_) {
            solver_.reset(config);
            signature_ = std::move(signature);
            content_ = std::move(content);
            step_ = 0;
        }
        for (; step_ < target; ++step_) {
            for (const GasSource& s : sources) solver_.addSource(s, dt);
            solver_.step(dt);
        }

        auto geo = std::make_shared<Geometry>();
        const int w = solver_.width(), h = solver_.height();
        geo->resizePoints(static_cast<size_t>(w) * static_cast<size_t>(h));
        float* xs = geo->x();
        float* ys = geo->y();
        float* density = geo->attrib(AttribClass::point, "density").floats().data();
        float* temperature = geo->attrib(AttribClass::point, "temperature").floats().data();
        float* vx = geo->attrib(AttribClass::point, "vx").floats().data();
        float* vy = geo->attrib(AttribClass::point, "vy").floats().data();
        float* speed = geo->attrib(AttribClass::point, "speed").floats().data();
        for (int j = 0; j < h; ++j) {
            for (int i = 0; i < w; ++i) {
                const size_t k = static_cast<size_t>(j) * static_cast<size_t>(w) + static_cast<size_t>(i);
                xs[k] = solver_.cellX(i);
                ys[k] = solver_.cellY(j);
                density[k] = solver_.density(i, j);
                temperature[k] = solver_.temperature(i, j);
                vx[k] = solver_.velocityX(i, j);
                vy[k] = solver_.velocityY(i, j);
                speed[k] = std::hypot(vx[k], vy[k]);
            }
        }
        return geo;
    }

private:
    GasSolver solver_;
    std::vector<float> signature_, content_;
    int64_t step_ = -1;
};

}  // namespace

void registerGasNodes() {
    NodeRegistry::instance().registerType("gasField", [] { return std::make_unique<GasFieldNode>(); });
}

}  // namespace jitnodes
