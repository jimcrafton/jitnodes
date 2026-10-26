#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "jitnodes/geo/topology.h"
#include "jitnodes/graph/network.h"
#include "nodeUtil.h"

namespace jitnodes {

namespace {

constexpr float degToRad = 3.14159265359f / 180.0f;
constexpr size_t maxSymbols = 50'000'000;
constexpr size_t maxDrawnPoints = 20'000'000;

std::string trimmed(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// "X=F[+X]F; F=FF" (separated by ';' or line breaks): every symbol may have several rules, one of which is picked at random.
std::map<char, std::vector<std::string>> parseRules(const std::string& text) {
    std::map<char, std::vector<std::string>> rules;
    std::string item;
    for (char c : text + ";") {
        if (c != ';' && c != '\n' && c != '\r') {
            item.push_back(c);
            continue;
        }
        const std::string rule = trimmed(item);
        item.clear();
        if (rule.empty()) continue;
        const size_t eq = rule.find('=');
        const std::string key = eq == std::string::npos ? std::string() : trimmed(rule.substr(0, eq));
        if (key.size() != 1) throw std::runtime_error("lsystem: a rule must look like 'F=FF', got '" + rule + "'");
        std::string body;
        for (char b : rule.substr(eq + 1))
            if (!std::isspace(static_cast<unsigned char>(b))) body.push_back(b);
        rules[key[0]].push_back(body);
    }
    return rules;
}

// A Lindenmayer system drawn by a turtle: F and G draw a line, f moves without drawing, + and - turn left and right, | turns around,
// [ and ] save and restore the turtle (a branch), and every other symbol only takes part in the rewriting.
class LsystemNode : public Node {
public:
    LsystemNode() : Node("lsystem", 0) {
        declareString("axiom", "X");
        declareString("rules", "X=F[+X][-X]FX; F=FF");
        declareFloat("iterations", 5.0f);
        declareFloat("angle", 25.0f);
        declareFloat("length", 0.1f);
        declareFloat("branchScale", 1.0f);
        declareFloat("angleJitter", 0.0f);
        declareFloat("lengthJitter", 0.0f);
        declareFloat("seed", 1.0f);
        declareFloat("startX", 0.0f);
        declareFloat("startY", 0.0f);
        declareFloat("startAngle", 90.0f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>&) override {
        std::string current;
        for (char c : getString("axiom"))
            if (!std::isspace(static_cast<unsigned char>(c))) current.push_back(c);
        if (current.empty()) throw std::runtime_error("lsystem: the axiom is empty");
        const auto rules = parseRules(getString("rules"));
        const int iterations = std::clamp(static_cast<int>(getFloat("iterations")), 0, 30);
        const uint32_t seed = seedOf(getFloat("seed"));

        for (int it = 0; it < iterations; ++it) {
            std::string next;
            next.reserve(current.size() * 2);
            for (size_t i = 0; i < current.size(); ++i) {
                const auto found = rules.find(current[i]);
                if (found == rules.end()) {
                    next.push_back(current[i]);
                } else {
                    const std::vector<std::string>& options = found->second;
                    const size_t pick = options.size() == 1 ? 0 : hash32(seed, static_cast<uint32_t>(i), 1000 + static_cast<uint32_t>(it)) % options.size();
                    next += options[pick];
                }
                if (next.size() > maxSymbols) throw std::runtime_error("lsystem: more than 50 million symbols; lower iterations");
            }
            current.swap(next);
        }

        struct Turtle {
            float x, y, heading, length;
            int depth;
        };
        const float angle = getFloat("angle") * degToRad;
        const float angleJitter = getFloat("angleJitter") * degToRad, lengthJitter = getFloat("lengthJitter");
        const float branchScale = getFloat("branchScale");
        Turtle t{getFloat("startX"), getFloat("startY"), getFloat("startAngle") * degToRad, getFloat("length"), 0};
        std::vector<Turtle> stack;
        std::vector<float> xs, ys;
        std::vector<int32_t> depth, stroke;
        std::vector<Primitive> prims;
        bool open = false;
        size_t strokeStart = 0;
        int32_t strokeIndex = -1;
        auto closeStroke = [&] {
            if (open && xs.size() - strokeStart >= 2) prims.push_back({static_cast<uint32_t>(strokeStart), static_cast<uint32_t>(xs.size() - strokeStart), false, PrimKind::polyline});
            open = false;
        };
        auto addPoint = [&] {
            xs.push_back(t.x);
            ys.push_back(t.y);
            depth.push_back(t.depth);
            stroke.push_back(strokeIndex);
            if (xs.size() > maxDrawnPoints) throw std::runtime_error("lsystem: more than 20 million points; lower iterations");
        };
        auto jittered = [&](float base, float amount, size_t i, uint32_t stream) { return amount == 0.0f ? base : base + amount * (random01(seed, static_cast<uint32_t>(i), stream) * 2.0f - 1.0f); };

        for (size_t i = 0; i < current.size(); ++i) {
            switch (current[i]) {
            case 'F':
            case 'G': {
                if (!open) {
                    open = true;
                    strokeStart = xs.size();
                    ++strokeIndex;
                    addPoint();
                }
                const float step = t.length * (lengthJitter == 0.0f ? 1.0f : 1.0f + lengthJitter * (random01(seed, static_cast<uint32_t>(i), 2) * 2.0f - 1.0f));
                t.x += std::cos(t.heading) * step;
                t.y += std::sin(t.heading) * step;
                addPoint();
                break;
            }
            case 'f':
                closeStroke();
                t.x += std::cos(t.heading) * t.length;
                t.y += std::sin(t.heading) * t.length;
                break;
            case '+':
                t.heading += jittered(angle, angleJitter, i, 3);
                break;
            case '-':
                t.heading -= jittered(angle, angleJitter, i, 4);
                break;
            case '|':
                t.heading += 3.14159265359f;
                break;
            case '[':
                closeStroke();
                stack.push_back(t);
                ++t.depth;
                t.length *= branchScale;
                break;
            case ']':
                closeStroke();
                if (stack.empty()) throw std::runtime_error("lsystem: ']' without a matching '['");
                t = stack.back();
                stack.pop_back();
                break;
            default:
                break;
            }
        }
        closeStroke();

        auto geo = std::make_shared<Geometry>();
        const size_t n = xs.size();
        geo->resizePoints(n);
        std::copy(xs.begin(), xs.end(), geo->x());
        std::copy(ys.begin(), ys.end(), geo->y());
        if (n > 0) {
            int32_t* d = geo->attrib(AttribClass::point, "depth", AttribType::int32).ints().data();
            int32_t* s = geo->attrib(AttribClass::point, "stroke", AttribType::int32).ints().data();
            float* u = geo->attrib(AttribClass::point, "u").floats().data();
            std::copy(depth.begin(), depth.end(), d);
            std::copy(stroke.begin(), stroke.end(), s);
            for (size_t i = 0; i < n; ++i) u[i] = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
        }
        for (const Primitive& p : prims) geo->addPrimitive(p);
        return geo;
    }
};

struct CurveSample {
    uint32_t a, b;
    float f;
    float tx, ty;
    int32_t curve;
    float u;
};

struct Curve {
    uint32_t start, count;
    bool closed;
    int32_t index;
    std::vector<float> along;
    float total = 0.0f;
    size_t first = 0, samples = 0;
};

// Places points along the input curves, evenly or at random, and gives each one its tangent, its normal and every attribute of the curve
// interpolated at that spot (unlike scatter, which only keeps u and srcPrim). Open curves get a point at each end.
class CurveSampleNode : public Node {
public:
    CurveSampleNode() : Node("curveSample", 1) {
        declareFloat("count", 100.0f);
        declareFloat("spacing", 0.0f);
        declareFloat("random", 0.0f);
        declareFloat("offset", 0.0f);
        declareFloat("seed", 1.0f);
        declareFloat("tolerance", 0.1f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const float spacing = std::max(getFloat("spacing"), 0.0f);
        const size_t perCurve = static_cast<size_t>(std::clamp(getFloat("count"), 0.0f, 5.0e6f));
        const bool random = getFloat("random") != 0.0f;
        const float offset = getFloat("offset");
        const uint32_t seed = seedOf(getFloat("seed"));

        Geometry flat;
        const Geometry* shape = in[0].get();
        if (hasCurves(*shape)) {
            flat = flattenCurves(*shape, getFloat("tolerance"));
            shape = &flat;
        }
        const float* sx = shape->pointCount() ? std::as_const(*shape).x() : nullptr;
        const float* sy = shape->pointCount() ? std::as_const(*shape).y() : nullptr;

        std::vector<Curve> curves;
        int32_t index = 0;
        size_t total = 0;
        for (const Primitive& p : shape->primitives()) {
            Curve c{p.start, p.count, p.closed, index++, {}, 0.0f, 0, 0};
            if (p.count < 2) continue;
            c.along.assign(p.count + (p.closed ? 1 : 0), 0.0f);
            const size_t segments = p.count - 1 + (p.closed ? 1 : 0);
            for (size_t k = 0; k < segments; ++k) {
                const size_t a = p.start + k, b = p.start + (k + 1) % p.count;
                c.along[k + 1] = c.along[k] + std::hypot(sx[b] - sx[a], sy[b] - sy[a]);
            }
            c.total = c.along.back();
            if (!(c.total > 1e-12f)) continue;
            if (spacing > 0.0f) c.samples = std::max<size_t>(1, static_cast<size_t>(std::floor(c.total / spacing + 1e-4f)) + (p.closed ? 0 : 1));
            else c.samples = perCurve;
            c.first = total;
            total += c.samples;
            if (total > 100'000'000) throw std::runtime_error("curveSample: more than 100 million points; raise spacing or lower count");
            curves.push_back(std::move(c));
        }

        std::vector<CurveSample> samples(total);
        auto sampleCurve = [&](size_t ci) {
            const Curve& c = curves[ci];
            const size_t segments = c.along.size() - 1;
            for (size_t j = 0; j < c.samples; ++j) {
                float d;
                if (random) d = random01(seed, static_cast<uint32_t>(c.first + j), 10) * c.total;
                else if (spacing > 0.0f) d = static_cast<float>(j) * spacing;
                else if (c.samples == 1) d = 0.0f;
                else d = c.closed ? c.total * static_cast<float>(j) / static_cast<float>(c.samples) : c.total * static_cast<float>(j) / static_cast<float>(c.samples - 1);
                d = std::clamp(d, 0.0f, c.total);
                size_t seg = static_cast<size_t>(std::upper_bound(c.along.begin(), c.along.end(), d) - c.along.begin());
                seg = std::clamp<size_t>(seg == 0 ? 0 : seg - 1, 0, segments - 1);
                const uint32_t a = c.start + static_cast<uint32_t>(seg), b = c.start + static_cast<uint32_t>((seg + 1) % c.count);
                const float len = c.along[seg + 1] - c.along[seg];
                CurveSample& s = samples[c.first + j];
                s.a = a;
                s.b = b;
                s.f = len > 1e-20f ? std::clamp((d - c.along[seg]) / len, 0.0f, 1.0f) : 0.0f;
                s.tx = len > 1e-20f ? (sx[b] - sx[a]) / len : 1.0f;
                s.ty = len > 1e-20f ? (sy[b] - sy[a]) / len : 0.0f;
                s.curve = c.index;
                s.u = d / c.total;
            }
        };
        if (ctx.pool && curves.size() > 1) ctx.pool->parallelFor(curves.size(), sampleCurve);
        else for (size_t ci = 0; ci < curves.size(); ++ci) sampleCurve(ci);

        auto geo = std::make_shared<Geometry>();
        geo->resizePoints(total);
        for (const auto& entry : shape->attribs(AttribClass::point)) {
            const AttribArray& source = entry.second;
            AttribArray& target = geo->attrib(AttribClass::point, entry.first, source.type());
            if (source.type() == AttribType::float32) {
                float* dst = target.floats().data();
                const float* src = source.floats().data();
                parallelChunks(ctx.pool, total, [&](size_t begin, size_t end) {
                    for (size_t i = begin; i < end; ++i) dst[i] = src[samples[i].a] + (src[samples[i].b] - src[samples[i].a]) * samples[i].f;
                });
            } else {
                int32_t* dst = target.ints().data();
                const int32_t* src = source.ints().data();
                parallelChunks(ctx.pool, total, [&](size_t begin, size_t end) {
                    for (size_t i = begin; i < end; ++i) dst[i] = samples[i].f < 0.5f ? src[samples[i].a] : src[samples[i].b];
                });
            }
        }
        for (const auto& entry : shape->details()) geo->setDetail(entry.first, entry.second);
        if (total == 0) return geo;
        float* xs = geo->x();
        float* ys = geo->y();
        float* u = geo->attrib(AttribClass::point, "u").floats().data();
        float* tx = geo->attrib(AttribClass::point, "tx").floats().data();
        float* ty = geo->attrib(AttribClass::point, "ty").floats().data();
        float* nx = geo->attrib(AttribClass::point, "nx").floats().data();
        float* ny = geo->attrib(AttribClass::point, "ny").floats().data();
        int32_t* curve = geo->attrib(AttribClass::point, "curve", AttribType::int32).ints().data();
        parallelChunks(ctx.pool, total, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const CurveSample& s = samples[i];
                xs[i] = sx[s.a] + (sx[s.b] - sx[s.a]) * s.f - s.ty * offset;
                ys[i] = sy[s.a] + (sy[s.b] - sy[s.a]) * s.f + s.tx * offset;
                u[i] = s.u;
                tx[i] = s.tx;
                ty[i] = s.ty;
                nx[i] = -s.ty;
                ny[i] = s.tx;
                curve[i] = s.curve;
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

void registerCurveNodes() {
    add<LsystemNode>("lsystem");
    add<CurveSampleNode>("curveSample");
}

}  // namespace jitnodes
