#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "jitnodes/graph/network.h"
#include "noise.h"
#include "nodeUtil.h"

namespace jitnodes {

namespace {

FractalMode modeFromString(const std::string& s, const char* node) {
    if (s == "fbm") return FractalMode::fbm;
    if (s == "ridged") return FractalMode::ridged;
    if (s == "turbulence") return FractalMode::turbulence;
    throw std::runtime_error(std::string(node) + ": mode must be 'fbm', 'ridged' or 'turbulence', got '" + s + "'");
}

struct Rgb {
    float r = 0.0f, g = 0.0f, b = 0.0f;
};

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "#rgb" or "#rrggbb".
Rgb parseHex(const std::string& s) {
    std::string digits = s.size() && s[0] == '#' ? s.substr(1) : s;
    if (digits.size() == 3) digits = {digits[0], digits[0], digits[1], digits[1], digits[2], digits[2]};
    if (digits.size() != 6) throw std::runtime_error("bad colour '" + s + "': use #rgb or #rrggbb");
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = hexDigit(digits[static_cast<size_t>(i)]);
        if (v[i] < 0) throw std::runtime_error("bad colour '" + s + "': use #rgb or #rrggbb");
    }
    return {static_cast<float>(v[0] * 16 + v[1]) / 255.0f, static_cast<float>(v[2] * 16 + v[3]) / 255.0f, static_cast<float>(v[4] * 16 + v[5]) / 255.0f};
}

struct Stop {
    float pos;
    Rgb color;
};

// "0 #000020, 0.5 #ff7090, 1 #ffffff"; stops are separated by commas, semicolons or newlines, and a colour is a hex code or three 0-1 floats.
std::vector<Stop> parseRamp(const std::string& text) {
    std::vector<Stop> stops;
    std::string item;
    auto flush = [&] {
        std::istringstream in(item);
        item.clear();
        std::string posText, first;
        if (!(in >> posText)) return;
        if (!(in >> first)) throw std::runtime_error("colorRamp: stop '" + posText + "' has no colour");
        Stop s;
        s.pos = static_cast<float>(std::atof(posText.c_str()));
        if (first[0] == '#') {
            s.color = parseHex(first);
        } else {
            std::string g, b;
            if (!(in >> g >> b)) throw std::runtime_error("colorRamp: a colour is #rrggbb or three numbers");
            s.color = {static_cast<float>(std::atof(first.c_str())), static_cast<float>(std::atof(g.c_str())), static_cast<float>(std::atof(b.c_str()))};
        }
        stops.push_back(s);
    };
    for (char c : text) {
        if (c == ',' || c == ';' || c == '\n') flush();
        else item.push_back(c);
    }
    flush();
    if (stops.empty()) throw std::runtime_error("colorRamp: the ramp has no stops");
    std::stable_sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.pos < b.pos; });
    return stops;
}

constexpr int rampSteps = 1024;

std::vector<Rgb> rampTable(const std::vector<Stop>& stops) {
    std::vector<Rgb> table(rampSteps + 1);
    for (int i = 0; i <= rampSteps; ++i) {
        const float t = static_cast<float>(i) / rampSteps;
        size_t hi = 0;
        while (hi < stops.size() && stops[hi].pos < t) ++hi;
        if (hi == 0) table[static_cast<size_t>(i)] = stops.front().color;
        else if (hi == stops.size()) table[static_cast<size_t>(i)] = stops.back().color;
        else {
            const Stop& a = stops[hi - 1];
            const Stop& b = stops[hi];
            const float f = b.pos > a.pos ? (t - a.pos) / (b.pos - a.pos) : 1.0f;
            table[static_cast<size_t>(i)] = {a.color.r + (b.color.r - a.color.r) * f, a.color.g + (b.color.g - a.color.g) * f,
                                             a.color.b + (b.color.b - a.color.b) * f};
        }
    }
    return table;
}

Fractal fractalFrom(const Node& n, const std::string& mode, const char* node) {
    Fractal f;
    f.mode = modeFromString(mode, node);
    f.octaves = std::clamp(static_cast<int>(n.getFloat("octaves")), 1, 12);
    f.lacunarity = n.getFloat("lacunarity");
    f.gain = n.getFloat("gain");
    f.seed = seedOf(n.getFloat("seed"));
    return f;
}

class NoiseNode : public Node {
public:
    NoiseNode() : Node("noise", 1) {
        declareString("name", "noise");
        declareString("mode", "fbm");
        declareFloat("scale", 0.1f);
        declareFloat("offsetX", 0.0f);
        declareFloat("offsetY", 0.0f);
        declareFloat("phase", 0.0f);
        declareFloat("octaves", 5.0f);
        declareFloat("lacunarity", 2.0f);
        declareFloat("gain", 0.5f);
        declareFloat("seed", 1.0f);
        declareFloat("amplitude", 1.0f);
        declareFloat("bias", 0.0f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        auto geo = std::make_shared<Geometry>(*in[0]);
        const std::string& mode = getString("mode");
        const Fractal fractal = fractalFrom(*this, mode, "noise");
        const float scale = getFloat("scale"), ox = getFloat("offsetX"), oy = getFloat("offsetY"), phase = getFloat("phase");
        const float amplitude = getFloat("amplitude"), bias = getFloat("bias");
        // fbm is centred on 0.5 so every mode lands in a similar 0..1 range.
        const bool centre = fractal.mode == FractalMode::fbm;
        const float* xs = std::as_const(*geo).x();
        const float* ys = std::as_const(*geo).y();
        float* out = geo->attrib(AttribClass::point, getString("name")).floats().data();
        parallelChunks(ctx.pool, geo->pointCount(), [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                float v = fractal.sample((xs[i] + ox) * scale, (ys[i] + oy) * scale, phase);
                if (centre) v = v * 0.5f / 0.7f + 0.5f;
                out[i] = v * amplitude + bias;
            }
        });
        return geo;
    }
};

class WarpNode : public Node {
public:
    WarpNode() : Node("warp", 1) {
        declareString("mode", "noise");
        declareFloat("amount", 1.0f);
        declareFloat("scale", 0.1f);
        declareFloat("octaves", 3.0f);
        declareFloat("lacunarity", 2.0f);
        declareFloat("gain", 0.5f);
        declareFloat("iterations", 1.0f);
        declareFloat("phase", 0.0f);
        declareFloat("seed", 1.0f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const std::string& mode = getString("mode");
        if (mode != "noise" && mode != "curl") throw std::runtime_error("warp: mode must be 'noise' or 'curl', got '" + mode + "'");
        const bool curl = mode == "curl";
        const Fractal fractal = fractalFrom(*this, "fbm", "warp");
        const float amount = getFloat("amount"), scale = getFloat("scale"), phase = getFloat("phase");
        const int iterations = std::clamp(static_cast<int>(getFloat("iterations")), 1, 64);

        auto geo = std::make_shared<Geometry>(*in[0]);
        float* xs = geo->x();
        float* ys = geo->y();
        // Central differences over a small fraction of the noise wavelength.
        const float eps = 0.02f;
        parallelChunks(ctx.pool, geo->pointCount(), [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                float x = xs[i], y = ys[i];
                for (int it = 0; it < iterations; ++it) {
                    const float z = phase + static_cast<float>(it) * 5.2f;
                    const float nx = x * scale, ny = y * scale;
                    if (curl) {
                        const float dx = (fractal.sample(nx + eps, ny, z) - fractal.sample(nx - eps, ny, z)) / (2.0f * eps);
                        const float dy = (fractal.sample(nx, ny + eps, z) - fractal.sample(nx, ny - eps, z)) / (2.0f * eps);
                        x += dy * amount / static_cast<float>(iterations);
                        y -= dx * amount / static_cast<float>(iterations);
                    } else {
                        const float dx = fractal.sample(nx, ny, z);
                        const float dy = fractal.sample(nx + 17.3f, ny + 41.7f, z);
                        x += dx * amount / static_cast<float>(iterations);
                        y += dy * amount / static_cast<float>(iterations);
                    }
                }
                xs[i] = x;
                ys[i] = y;
            }
        });
        return geo;
    }
};

class ColorRampNode : public Node {
public:
    ColorRampNode() : Node("colorRamp", 1) {
        declareString("attribute", "noise");
        declareString("ramp", "0 #000000, 1 #ffffff");
        declareFloat("min", 0.0f);
        declareFloat("max", 1.0f);
        declareFloat("gamma", 1.0f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const std::string& name = getString("attribute");
        const AttribArray* source = in[0]->findAttrib(AttribClass::point, name);
        if (!source) throw std::runtime_error("colorRamp: no point attribute '" + name + "'");
        const std::vector<Rgb> table = rampTable(parseRamp(getString("ramp")));
        const float lo = getFloat("min"), hi = getFloat("max"), gamma = getFloat("gamma");
        const float span = hi != lo ? 1.0f / (hi - lo) : 0.0f;

        auto geo = std::make_shared<Geometry>(*in[0]);
        source = geo->findAttrib(AttribClass::point, name);
        float* red = geo->attrib(AttribClass::point, "Cd.x").floats().data();
        float* green = geo->attrib(AttribClass::point, "Cd.y").floats().data();
        float* blue = geo->attrib(AttribClass::point, "Cd.z").floats().data();
        parallelChunks(ctx.pool, geo->pointCount(), [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                float t = std::clamp((source->at(i) - lo) * span, 0.0f, 1.0f);
                if (gamma != 1.0f && t > 0.0f) t = std::pow(t, gamma);
                const float f = t * rampSteps;
                const size_t k = std::min<size_t>(static_cast<size_t>(f), rampSteps - 1);
                const float w = f - static_cast<float>(k);
                const Rgb& a = table[k];
                const Rgb& b = table[k + 1];
                red[i] = a.r + (b.r - a.r) * w;
                green[i] = a.g + (b.g - a.g) * w;
                blue[i] = a.b + (b.b - a.b) * w;
            }
        });
        return geo;
    }
};

// Passes the geometry through and records how it is turned into an image (see geo/raster.h); the editor viewport and `jitnodesCli --png`
// draw geometry that carries these values with the splat renderer.
class RenderNode : public Node {
public:
    RenderNode() : Node("render", 1) {
        declareFloat("additive", 1.0f);
        declareFloat("exposure", 1.0f);
        declareFloat("gamma", 2.2f);
        declareFloat("bloom", 0.0f);
        declareFloat("bloomRadius", 12.0f);
        declareString("background", "#000000");
        declareFloat("width", 1400.0f);
        declareFloat("height", 700.0f);
        declareFloat("viewCx", 0.0f);
        declareFloat("viewCy", 0.0f);
        declareFloat("viewWidth", 0.0f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        auto geo = std::make_shared<Geometry>(*in[0]);
        const Rgb bg = parseHex(getString("background"));
        geo->setDetail("render.enabled", 1.0f);
        geo->setDetail("render.additive", getFloat("additive") != 0.0f ? 1.0f : 0.0f);
        for (const char* name : {"exposure", "gamma", "bloom", "bloomRadius", "width", "height", "viewCx", "viewCy", "viewWidth"})
            geo->setDetail(std::string("render.") + name, getFloat(name));
        geo->setDetail("render.bgR", bg.r);
        geo->setDetail("render.bgG", bg.g);
        geo->setDetail("render.bgB", bg.b);
        return geo;
    }
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerFxNodes() {
    add<NoiseNode>("noise");
    add<WarpNode>("warp");
    add<ColorRampNode>("colorRamp");
    add<RenderNode>("render");
}

}  // namespace jitnodes
