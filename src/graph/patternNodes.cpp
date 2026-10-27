#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "jitnodes/graph/network.h"
#include "noise.h"
#include "nodeUtil.h"
#include "pointGrid.h"

namespace jitnodes {

namespace {

constexpr float twoPi = 6.28318530718f;
std::vector<std::string> splitNames(const std::string& text) {
    std::vector<std::string> names;
    std::string item;
    for (char c : text + ",") {
        if (c == ',' || c == ' ' || c == ';' || c == '\n') {
            if (!item.empty()) names.push_back(item);
            item.clear();
        } else {
            item.push_back(c);
        }
    }
    return names;
}

// Pushes points that are closer than `radius` apart until they are not (or the iterations run out): even, blue-noise-like spacing.
class RelaxNode : public Node {
public:
    RelaxNode() : Node("relax", 1) {
        declareFloat("radius", 0.5f);
        declareFloat("iterations", 20.0f);
        declareFloat("strength", 0.4f);
        declareFloat("keepInside", 1.0f);
        declareFloat("seed", 1.0f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const float radius = getFloat("radius"), strength = getFloat("strength");
        if (!(radius > 0.0f)) throw std::runtime_error("relax: radius must be greater than 0");
        const int iterations = std::clamp(static_cast<int>(getFloat("iterations")), 1, 500);
        const uint32_t seed = seedOf(getFloat("seed"));
        const bool keepInside = getFloat("keepInside") != 0.0f;

        auto geo = std::make_shared<Geometry>(*in[0]);
        const size_t n = geo->pointCount();
        if (n == 0) return geo;
        float* xs = geo->x();
        float* ys = geo->y();

        float minX = xs[0], maxX = xs[0], minY = ys[0], maxY = ys[0];
        for (size_t i = 1; i < n; ++i) {
            minX = std::min(minX, xs[i]);
            maxX = std::max(maxX, xs[i]);
            minY = std::min(minY, ys[i]);
            maxY = std::max(maxY, ys[i]);
        }

        std::vector<float> moveX(n), moveY(n);
        PointGrid grid;
        const float r2 = radius * radius;
        for (int it = 0; it < iterations; ++it) {
            grid.build(xs, ys, n, radius);
            parallelChunks(ctx.pool, n, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    float sx = 0.0f, sy = 0.0f;
                    grid.forEachNear(xs[i], ys[i], [&](uint32_t j) {
                        if (j == i) return;
                        float dx = xs[i] - xs[j], dy = ys[i] - ys[j];
                        const float d2 = dx * dx + dy * dy;
                        if (!(d2 < r2)) return;
                        float d = std::sqrt(d2);
                        if (d < 1e-6f) {
                            // Both points of a coincident pair use the same angle, in opposite senses.
                            const uint32_t lo = std::min<uint32_t>(j, static_cast<uint32_t>(i)), hi = std::max<uint32_t>(j, static_cast<uint32_t>(i));
                            const float a = twoPi * random01(seed, lo * 0x9E3779B1u ^ hi, static_cast<uint32_t>(it));
                            const float sign = i < j ? 1.0f : -1.0f;
                            dx = std::cos(a) * sign;
                            dy = std::sin(a) * sign;
                            d = 0.0f;
                        } else {
                            dx /= d;
                            dy /= d;
                        }
                        const float push = (radius - d) * 0.5f * strength;
                        sx += dx * push;
                        sy += dy * push;
                    });
                    moveX[i] = sx;
                    moveY[i] = sy;
                }
            });
            parallelChunks(ctx.pool, n, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    xs[i] += moveX[i];
                    ys[i] += moveY[i];
                    if (keepInside) {
                        xs[i] = std::clamp(xs[i], minX, maxX);
                        ys[i] = std::clamp(ys[i], minY, maxY);
                    }
                }
            });
        }
        return geo;
    }
};

// Replaces attributes with the average over the points within `radius`, repeated `iterations` times.
class AttribBlurNode : public Node {
public:
    AttribBlurNode() : Node("attribBlur", 1) {
        declareString("attribute", "rand");
        declareString("output", "");
        declareString("falloff", "linear");
        declareFloat("radius", 0.5f);
        declareFloat("iterations", 1.0f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const std::vector<std::string> names = splitNames(getString("attribute"));
        std::vector<std::string> outNames = splitNames(getString("output"));
        if (names.empty()) throw std::runtime_error("attribBlur: no attribute named");
        if (outNames.empty()) outNames = names;
        if (outNames.size() != names.size()) throw std::runtime_error("attribBlur: output needs one name per attribute");
        const std::string& falloffName = getString("falloff");
        if (falloffName != "linear" && falloffName != "box" && falloffName != "gaussian")
            throw std::runtime_error("attribBlur: falloff must be 'linear', 'box' or 'gaussian', got '" + falloffName + "'");
        const int falloff = falloffName == "linear" ? 0 : falloffName == "box" ? 1 : 2;
        const float radius = getFloat("radius");
        if (!(radius > 0.0f)) throw std::runtime_error("attribBlur: radius must be greater than 0");
        const int iterations = std::clamp(static_cast<int>(getFloat("iterations")), 1, 64);

        auto geo = std::make_shared<Geometry>(*in[0]);
        const size_t n = geo->pointCount();
        const size_t channels = names.size();
        std::vector<std::vector<float>> current(channels, std::vector<float>(n)), next(channels, std::vector<float>(n));
        for (size_t c = 0; c < channels; ++c) {
            const AttribArray* source = geo->findAttrib(AttribClass::point, names[c]);
            if (!source) throw std::runtime_error("attribBlur: no point attribute '" + names[c] + "'");
            for (size_t i = 0; i < n; ++i) current[c][i] = source->at(i);
        }
        if (n == 0) return geo;

        const float* xs = std::as_const(*geo).x();
        const float* ys = std::as_const(*geo).y();
        PointGrid grid;
        grid.build(xs, ys, n, radius);
        const float r2 = radius * radius;
        for (int it = 0; it < iterations; ++it) {
            parallelChunks(ctx.pool, n, [&](size_t begin, size_t end) {
                std::vector<float> sum(channels);
                for (size_t i = begin; i < end; ++i) {
                    float weightSum = 1.0f;
                    for (size_t c = 0; c < channels; ++c) sum[c] = current[c][i];
                    grid.forEachNear(xs[i], ys[i], [&](uint32_t j) {
                        if (j == i) return;
                        const float dx = xs[i] - xs[j], dy = ys[i] - ys[j];
                        const float d2 = dx * dx + dy * dy;
                        if (!(d2 < r2)) return;
                        const float w = falloff == 0 ? 1.0f - std::sqrt(d2) / radius : falloff == 1 ? 1.0f : std::exp(-4.0f * d2 / r2);
                        weightSum += w;
                        for (size_t c = 0; c < channels; ++c) sum[c] += w * current[c][j];
                    });
                    for (size_t c = 0; c < channels; ++c) next[c][i] = sum[c] / weightSum;
                }
            });
            std::swap(current, next);
        }
        for (size_t c = 0; c < channels; ++c) {
            float* out = geo->attrib(AttribClass::point, outNames[c]).floats().data();
            std::copy(current[c].begin(), current[c].end(), out);
        }
        return geo;
    }
};

// Writes a travelling sine wave into an attribute, and can displace the points by it. Radial waves come from one source or from several
// spaced along a segment (two sources give interference fringes); plane waves run in the direction `angle`.
class WaveNode : public Node {
public:
    WaveNode() : Node("wave", 1) {
        declareString("name", "wave");
        declareString("mode", "radial");
        declareFloat("cx", 0.0f);
        declareFloat("cy", 0.0f);
        declareFloat("cx2", 0.0f);
        declareFloat("cy2", 0.0f);
        declareFloat("sourceCount", 1.0f);
        declareFloat("angle", 0.0f);
        declareFloat("wavelength", 2.0f);
        declareFloat("phase", 0.0f);
        declareFloat("amplitude", 1.0f);
        declareFloat("falloff", 0.0f);
        declareFloat("displaceX", 0.0f);
        declareFloat("displaceY", 0.0f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const std::string& mode = getString("mode");
        if (mode != "radial" && mode != "plane") throw std::runtime_error("wave: mode must be 'radial' or 'plane', got '" + mode + "'");
        const bool radial = mode == "radial";
        const float wavelength = getFloat("wavelength");
        if (!(wavelength != 0.0f)) throw std::runtime_error("wave: wavelength must not be 0");
        const float k = twoPi / wavelength, phase = getFloat("phase"), amplitude = getFloat("amplitude"), falloff = getFloat("falloff");
        const float displaceX = getFloat("displaceX"), displaceY = getFloat("displaceY");
        const int sources = std::clamp(static_cast<int>(getFloat("sourceCount")), 1, 256);
        const float cx = getFloat("cx"), cy = getFloat("cy"), cx2 = getFloat("cx2"), cy2 = getFloat("cy2");
        const float angle = getFloat("angle") * (twoPi / 360.0f);
        const float dirX = std::cos(angle), dirY = std::sin(angle);

        auto geo = std::make_shared<Geometry>(*in[0]);
        const size_t n = geo->pointCount();
        float* out = geo->attrib(AttribClass::point, getString("name")).floats().data();
        float* xs = geo->x();
        float* ys = geo->y();
        parallelChunks(ctx.pool, n, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                float sum = 0.0f;
                if (radial) {
                    for (int s = 0; s < sources; ++s) {
                        const float t = sources > 1 ? static_cast<float>(s) / static_cast<float>(sources - 1) : 0.0f;
                        const float r = std::hypot(xs[i] - (cx + (cx2 - cx) * t), ys[i] - (cy + (cy2 - cy) * t));
                        sum += std::sin(k * r - phase) * (falloff != 0.0f ? std::exp(-falloff * r) : 1.0f);
                    }
                    sum /= static_cast<float>(sources);
                } else {
                    const float r = (xs[i] - cx) * dirX + (ys[i] - cy) * dirY;
                    sum = std::sin(k * r - phase) * (falloff != 0.0f ? std::exp(-falloff * std::fabs(r)) : 1.0f);
                }
                const float v = sum * amplitude;
                out[i] = v;
                xs[i] += v * displaceX;
                ys[i] += v * displaceY;
            }
        });
        return geo;
    }
};

enum class AttractorKind { clifford, deJong, svensson };

// Iterates every input point (taken as a start position in the map's own units) through a chaotic 2D map, then scales the result into the
// scene. With many seeds the points trace the attractor, and rendered additively their density draws it.
class AttractorNode : public Node {
public:
    AttractorNode() : Node("attractor", 1) {
        declareString("type", "clifford");
        declareFloat("a", -1.4f);
        declareFloat("b", 1.6f);
        declareFloat("c", 1.0f);
        declareFloat("d", 0.7f);
        declareFloat("iterations", 30.0f);
        declareFloat("scale", 3.0f);
        declareFloat("cx", 0.0f);
        declareFloat("cy", 0.0f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const std::string& type = getString("type");
        AttractorKind kind;
        if (type == "clifford") kind = AttractorKind::clifford;
        else if (type == "dejong") kind = AttractorKind::deJong;
        else if (type == "svensson") kind = AttractorKind::svensson;
        else throw std::runtime_error("attractor: type must be 'clifford', 'dejong' or 'svensson', got '" + type + "'");
        const float a = getFloat("a"), b = getFloat("b"), c = getFloat("c"), d = getFloat("d");
        const int iterations = std::clamp(static_cast<int>(getFloat("iterations")), 1, 10000);
        const float scale = getFloat("scale"), cx = getFloat("cx"), cy = getFloat("cy");

        auto geo = std::make_shared<Geometry>(*in[0]);
        float* speed = geo->attrib(AttribClass::point, "speed").floats().data();
        float* dir = geo->attrib(AttribClass::point, "dir").floats().data();
        float* xs = geo->x();
        float* ys = geo->y();
        parallelChunks(ctx.pool, geo->pointCount(), [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                float x = xs[i], y = ys[i], stepX = 0.0f, stepY = 0.0f;
                for (int it = 0; it < iterations; ++it) {
                    float nx, ny;
                    switch (kind) {
                        case AttractorKind::clifford:
                            nx = std::sin(a * y) + c * std::cos(a * x);
                            ny = std::sin(b * x) + d * std::cos(b * y);
                            break;
                        case AttractorKind::deJong:
                            nx = std::sin(a * y) - std::cos(b * x);
                            ny = std::sin(c * x) - std::cos(d * y);
                            break;
                        default:
                            nx = d * std::sin(a * x) - std::sin(b * y);
                            ny = c * std::cos(a * x) + std::cos(b * y);
                            break;
                    }
                    stepX = nx - x;
                    stepY = ny - y;
                    x = nx;
                    y = ny;
                }
                speed[i] = std::hypot(stepX, stepY);
                dir[i] = std::atan2(stepY, stepX) / twoPi + 0.5f;
                xs[i] = cx + x * scale;
                ys[i] = cy + y * scale;
            }
        });
        return geo;
    }
};

// Gives every output point the attribute values of the input point it came from: output points first[i] .. first[i] + count[i] - 1 belong to
// input point i.
void replicateAttributes(const Geometry& in, Geometry& out, const std::vector<size_t>& first, const std::vector<uint32_t>& count, ThreadPool* pool) {
    const size_t n = in.pointCount();
    for (const auto& entry : in.attribs(AttribClass::point)) {
        const AttribArray& source = entry.second;
        AttribArray& target = out.attribLike(AttribClass::point, entry.first, source);
        if (source.type() == AttribType::float32) {
            float* dst = target.floats().data();
            const float* src = source.floats().data();
            parallelChunks(pool, n, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) std::fill_n(dst + first[i], count[i], src[i]);
            });
        } else {
            int32_t* dst = target.ints().data();
            const int32_t* src = source.ints().data();
            parallelChunks(pool, n, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) std::fill_n(dst + first[i], count[i], src[i]);
            });
        }
    }
    for (const auto& entry : in.details()) out.setDetail(entry.first, entry.second);
}

constexpr size_t maxFlowPoints = 50'000'000;

// Follows each input point through a noise-driven vector field and outputs the path as a polyline: streamlines of a flow field.
class FlowNode : public Node {
public:
    FlowNode() : Node("flow", 1) {
        declareString("mode", "curl");
        declareFloat("steps", 64.0f);
        declareFloat("stepLength", 0.1f);
        declareFloat("bothWays", 0.0f);
        declareFloat("normalize", 1.0f);
        declareFloat("strength", 1.0f);
        declareFloat("turns", 1.0f);
        declareFloat("scale", 0.1f);
        declareFloat("octaves", 3.0f);
        declareFloat("lacunarity", 2.0f);
        declareFloat("gain", 0.5f);
        declareFloat("seed", 1.0f);
        declareFloat("phase", 0.0f);
        declareFloat("drift", 0.0f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const std::string& mode = getString("mode");
        if (mode != "curl" && mode != "angle") throw std::runtime_error("flow: mode must be 'curl' or 'angle', got '" + mode + "'");
        const bool curl = mode == "curl";
        const int steps = std::clamp(static_cast<int>(getFloat("steps")), 1, 4096);
        const bool both = getFloat("bothWays") != 0.0f;
        const bool unit = getFloat("normalize") != 0.0f;
        const float stepLength = getFloat("stepLength"), strength = getFloat("strength"), turns = getFloat("turns"), scale = getFloat("scale");
        const float phase = getFloat("phase"), drift = getFloat("drift");
        Fractal field;
        field.octaves = std::clamp(static_cast<int>(getFloat("octaves")), 1, 12);
        field.lacunarity = getFloat("lacunarity");
        field.gain = getFloat("gain");
        field.seed = seedOf(getFloat("seed"));

        const Geometry& seeds = *in[0];
        const size_t n = seeds.pointCount();
        const size_t perTrail = static_cast<size_t>(steps) * (both ? 2 : 1) + 1;
        if (n * perTrail > maxFlowPoints) throw std::runtime_error("flow: " + std::to_string(n) + " trails of " + std::to_string(perTrail) + " points exceed " + std::to_string(maxFlowPoints) + " points");

        auto geo = std::make_shared<Geometry>();
        geo->resizePoints(n * perTrail);
        std::vector<size_t> first(n);
        std::vector<uint32_t> count(n, static_cast<uint32_t>(perTrail));
        for (size_t i = 0; i < n; ++i) first[i] = i * perTrail;
        replicateAttributes(seeds, *geo, first, count, ctx.pool);
        float* u = geo->attrib(AttribClass::point, "u").floats().data();
        int32_t* trail = geo->attrib(AttribClass::point, "trail", AttribType::int32).ints().data();
        float* xs = geo->x();
        float* ys = geo->y();
        const float* sx = n ? std::as_const(seeds).x() : nullptr;
        const float* sy = n ? std::as_const(seeds).y() : nullptr;

        auto direction = [&](float x, float y, float z, float& dx, float& dy) {
            if (curl) {
                curlOf(field, x * scale, y * scale, z, 0.02f, dx, dy);
            } else {
                const float a = field.sample(x * scale, y * scale, z) * twoPi * turns;
                dx = std::cos(a);
                dy = std::sin(a);
            }
            if (unit) {
                const float len = std::hypot(dx, dy);
                if (len > 1e-12f) {
                    dx /= len;
                    dy /= len;
                } else {
                    dx = dy = 0.0f;
                }
            } else {
                dx *= strength;
                dy *= strength;
            }
        };

        parallelChunks(ctx.pool, n, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const size_t base = i * perTrail, seedAt = both ? static_cast<size_t>(steps) : 0;
                xs[base + seedAt] = sx[i];
                ys[base + seedAt] = sy[i];
                float x = sx[i], y = sy[i], dx, dy;
                for (int k = 1; k <= steps; ++k) {
                    direction(x, y, phase + drift * static_cast<float>(k - 1), dx, dy);
                    x += dx * stepLength;
                    y += dy * stepLength;
                    xs[base + seedAt + static_cast<size_t>(k)] = x;
                    ys[base + seedAt + static_cast<size_t>(k)] = y;
                }
                if (both) {
                    x = sx[i];
                    y = sy[i];
                    for (int k = 1; k <= steps; ++k) {
                        direction(x, y, phase - drift * static_cast<float>(k - 1), dx, dy);
                        x -= dx * stepLength;
                        y -= dy * stepLength;
                        xs[base + seedAt - static_cast<size_t>(k)] = x;
                        ys[base + seedAt - static_cast<size_t>(k)] = y;
                    }
                }
                for (size_t k = 0; k < perTrail; ++k) {
                    u[base + k] = perTrail > 1 ? static_cast<float>(k) / static_cast<float>(perTrail - 1) : 0.0f;
                    trail[base + k] = static_cast<int32_t>(i);
                }
            }
        });
        for (size_t i = 0; i < n; ++i) geo->addPrimitive({static_cast<uint32_t>(i * perTrail), static_cast<uint32_t>(perTrail), false, PrimKind::polyline});
        return geo;
    }
};

// Clips the convex polygon (interleaved x, y) to the side of the perpendicular bisector between the site (sx, sy) and the point at (sx + dx, sy + dy)
// that contains the site.
void clipToBisector(std::vector<float>& poly, std::vector<float>& scratch, float sx, float sy, float dx, float dy) {
    const float half = 0.5f * (dx * dx + dy * dy);
    const size_t m = poly.size() / 2;
    scratch.clear();
    auto side = [&](size_t k) { return (poly[2 * k] - sx) * dx + (poly[2 * k + 1] - sy) * dy - half; };
    for (size_t k = 0; k < m; ++k) {
        const size_t next = k + 1 == m ? 0 : k + 1;
        const float fa = side(k), fb = side(next);
        if (fa <= 0.0f) {
            scratch.push_back(poly[2 * k]);
            scratch.push_back(poly[2 * k + 1]);
        }
        if ((fa < 0.0f && fb > 0.0f) || (fa > 0.0f && fb < 0.0f)) {
            const float t = fa / (fa - fb);
            scratch.push_back(poly[2 * k] + (poly[2 * next] - poly[2 * k]) * t);
            scratch.push_back(poly[2 * k + 1] + (poly[2 * next + 1] - poly[2 * k + 1]) * t);
        }
    }
    poly.swap(scratch);
}

struct Bounds {
    float minX, maxX, minY, maxY;
};

Bounds boundsOf(const float* xs, const float* ys, size_t n, float margin, const char* who) {
    Bounds b{xs[0], xs[0], ys[0], ys[0]};
    for (size_t i = 1; i < n; ++i) {
        b.minX = std::min(b.minX, xs[i]);
        b.maxX = std::max(b.maxX, xs[i]);
        b.minY = std::min(b.minY, ys[i]);
        b.maxY = std::max(b.maxY, ys[i]);
    }
    if (!std::isfinite(b.minX + b.maxX + b.minY + b.maxY)) throw std::runtime_error(std::string(who) + ": point positions are not finite");
    b.minX -= margin;
    b.maxX += margin;
    b.minY -= margin;
    b.maxY += margin;
    if (b.maxX - b.minX < 1e-6f) {
        b.minX -= 1.0f;
        b.maxX += 1.0f;
    }
    if (b.maxY - b.minY < 1e-6f) {
        b.minY -= 1.0f;
        b.maxY += 1.0f;
    }
    return b;
}

// One convex polygon (interleaved x, y) per point, clipped to `b`. Each cell is built by cutting the rectangle with the bisectors of the nearest
// neighbours, found ring by ring in a grid. A cell that is cut away entirely (a duplicate point) is empty.
std::vector<std::vector<float>> voronoiCells(const float* xs, const float* ys, size_t n, const Bounds& b, ThreadPool* pool) {
    PointGrid grid;
    grid.build(xs, ys, n, std::max(1.5f * std::sqrt((b.maxX - b.minX) * (b.maxY - b.minY) / static_cast<float>(n)), 1e-6f));
    const float cell = grid.cellSize();
    std::vector<std::vector<float>> cells(n);
    parallelChunks(pool, n, [&](size_t begin, size_t end) {
        std::vector<float> scratch;
        for (size_t i = begin; i < end; ++i) {
            std::vector<float>& poly = cells[i];
            poly = {b.minX, b.minY, b.maxX, b.minY, b.maxX, b.maxY, b.minX, b.maxY};
            const float sx = xs[i], sy = ys[i];
            for (int ring = 0; ring <= grid.maxRing(); ++ring) {
                grid.forEachInRing(sx, sy, ring, [&](uint32_t j) {
                    if (j == i || poly.empty()) return;
                    const float dx = xs[j] - sx, dy = ys[j] - sy;
                    if (dx * dx + dy * dy < 1e-18f) return;
                    clipToBisector(poly, scratch, sx, sy, dx, dy);
                });
                if (poly.size() < 6) {
                    poly.clear();
                    break;
                }
                float reach2 = 0.0f;
                for (size_t k = 0; k < poly.size(); k += 2) reach2 = std::max(reach2, (poly[k] - sx) * (poly[k] - sx) + (poly[k + 1] - sy) * (poly[k + 1] - sy));
                if (2.0f * std::sqrt(reach2) <= static_cast<float>(ring) * cell) break;
            }
        }
    });
    return cells;
}

// The Voronoi diagram of the input points: one polygon per point, the region closer to it than to any other point, clipped to the bounding box of
// the points plus `margin`.
class VoronoiNode : public Node {
public:
    VoronoiNode() : Node("voronoi", 1) {
        declareFloat("margin", 0.0f);
        declareFloat("closed", 1.0f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const Geometry& sites = *in[0];
        const size_t n = sites.pointCount();
        auto geo = std::make_shared<Geometry>();
        if (n == 0) return geo;
        const bool closed = getFloat("closed") != 0.0f;
        const float margin = std::max(getFloat("margin"), 0.0f);
        const float* xs = std::as_const(sites).x();
        const float* ys = std::as_const(sites).y();

        const std::vector<std::vector<float>> cells = voronoiCells(xs, ys, n, boundsOf(xs, ys, n, margin, "voronoi"), ctx.pool);

        std::vector<size_t> first(n);
        std::vector<uint32_t> count(n);
        size_t total = 0;
        for (size_t i = 0; i < n; ++i) {
            first[i] = total;
            count[i] = static_cast<uint32_t>(cells[i].size() / 2 + (!closed && !cells[i].empty() ? 1 : 0));
            total += count[i];
        }
        geo->resizePoints(total);
        replicateAttributes(sites, *geo, first, count, ctx.pool);
        float* area = geo->attrib(AttribClass::point, "area").floats().data();
        int32_t* site = geo->attrib(AttribClass::point, "site", AttribType::int32).ints().data();
        float* ox = geo->x();
        float* oy = geo->y();
        parallelChunks(ctx.pool, n, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const std::vector<float>& poly = cells[i];
                const size_t m = poly.size() / 2;
                double twice = 0.0;
                for (size_t k = 0, prev = m ? m - 1 : 0; k < m; prev = k++) twice += static_cast<double>(poly[2 * prev]) * poly[2 * k + 1] - static_cast<double>(poly[2 * k]) * poly[2 * prev + 1];
                for (size_t k = 0; k < count[i]; ++k) {
                    const size_t from = k % m;
                    ox[first[i] + k] = poly[2 * from];
                    oy[first[i] + k] = poly[2 * from + 1];
                    area[first[i] + k] = static_cast<float>(std::fabs(twice) * 0.5);
                    site[first[i] + k] = static_cast<int32_t>(i);
                }
            }
        });
        for (size_t i = 0; i < n; ++i)
            if (count[i] > 0) geo->addPrimitive({static_cast<uint32_t>(first[i]), count[i], closed, PrimKind::polyline});
        return geo;
    }
};

// Lloyd relaxation: moves every point towards the centroid of its Voronoi cell, repeated `iterations` times. The cells are clipped to the bounding
// box of the input points (plus `margin`), which stays fixed, so the points spread out evenly inside it.
class LloydNode : public Node {
public:
    LloydNode() : Node("lloyd", 1) {
        declareFloat("iterations", 5.0f);
        declareFloat("strength", 1.0f);
        declareFloat("margin", 0.0f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        const int iterations = std::clamp(static_cast<int>(getFloat("iterations")), 1, 200);
        const float strength = std::clamp(getFloat("strength"), 0.0f, 1.0f);
        const float margin = std::max(getFloat("margin"), 0.0f);
        auto geo = std::make_shared<Geometry>(*in[0]);
        const size_t n = geo->pointCount();
        if (n == 0) return geo;
        float* xs = geo->x();
        float* ys = geo->y();
        const Bounds bounds = boundsOf(xs, ys, n, margin, "lloyd");

        for (int it = 0; it < iterations; ++it) {
            const std::vector<std::vector<float>> cells = voronoiCells(xs, ys, n, bounds, ctx.pool);
            parallelChunks(ctx.pool, n, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    const std::vector<float>& poly = cells[i];
                    const size_t m = poly.size() / 2;
                    if (m < 3) continue;
                    double twice = 0.0, cx = 0.0, cy = 0.0;
                    for (size_t k = 0, prev = m - 1; k < m; prev = k++) {
                        const double cross = static_cast<double>(poly[2 * prev]) * poly[2 * k + 1] - static_cast<double>(poly[2 * k]) * poly[2 * prev + 1];
                        twice += cross;
                        cx += (poly[2 * prev] + poly[2 * k]) * cross;
                        cy += (poly[2 * prev + 1] + poly[2 * k + 1]) * cross;
                    }
                    if (std::fabs(twice) < 1e-20) continue;
                    cx /= 3.0 * twice;
                    cy /= 3.0 * twice;
                    xs[i] += static_cast<float>(cx - xs[i]) * strength;
                    ys[i] += static_cast<float>(cy - ys[i]) * strength;
                }
            });
        }
        return geo;
    }
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerPatternNodes() {
    add<RelaxNode>("relax");
    add<AttribBlurNode>("attribBlur");
    add<WaveNode>("wave");
    add<AttractorNode>("attractor");
    add<FlowNode>("flow");
    add<VoronoiNode>("voronoi");
    add<LloydNode>("lloyd");
}

}  // namespace jitnodes
