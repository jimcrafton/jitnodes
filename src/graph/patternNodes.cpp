#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "jitnodes/graph/network.h"
#include "nodeUtil.h"

namespace jitnodes {

namespace {

constexpr float twoPi = 6.28318530718f;
constexpr float maxGridCells = 4.0e6f;
constexpr float maxGridSide = 8192.0f;

// A uniform grid whose cells are at least `radius` wide, so every neighbour closer than `radius` is in the 3 x 3 cells around a point.
class PointGrid {
public:
    void build(const float* xs, const float* ys, size_t n, float radius) {
        float minX = std::numeric_limits<float>::max(), minY = minX, maxX = std::numeric_limits<float>::lowest(), maxY = maxX;
        for (size_t i = 0; i < n; ++i) {
            if (xs[i] < minX) minX = xs[i];
            if (xs[i] > maxX) maxX = xs[i];
            if (ys[i] < minY) minY = ys[i];
            if (ys[i] > maxY) maxY = ys[i];
        }
        if (n == 0 || minX > maxX || minY > maxY) minX = minY = maxX = maxY = 0.0f;
        const float w = maxX - minX, h = maxY - minY;
        cell_ = std::max({radius, std::sqrt(w * h / maxGridCells), std::max(w, h) / maxGridSide});
        if (!std::isfinite(cell_)) throw std::runtime_error("point positions are not finite");
        minX_ = minX;
        minY_ = minY;
        cols_ = static_cast<int>(w / cell_) + 1;
        rows_ = static_cast<int>(h / cell_) + 1;
        start_.assign(static_cast<size_t>(cols_) * static_cast<size_t>(rows_) + 1, 0);
        for (size_t i = 0; i < n; ++i) ++start_[cellOf(xs[i], ys[i]) + 1];
        for (size_t c = 1; c < start_.size(); ++c) start_[c] += start_[c - 1];
        items_.resize(n);
        std::vector<uint32_t> fill(start_.begin(), start_.end() - 1);
        for (size_t i = 0; i < n; ++i) items_[fill[cellOf(xs[i], ys[i])]++] = static_cast<uint32_t>(i);
    }

    template <class F>
    void forEachNear(float x, float y, F&& fn) const {
        const int cx = column(x), cy = row(y);
        for (int r = std::max(cy - 1, 0); r <= std::min(cy + 1, rows_ - 1); ++r) {
            for (int c = std::max(cx - 1, 0); c <= std::min(cx + 1, cols_ - 1); ++c) {
                const size_t cell = static_cast<size_t>(r) * static_cast<size_t>(cols_) + static_cast<size_t>(c);
                for (uint32_t k = start_[cell]; k < start_[cell + 1]; ++k) fn(items_[k]);
            }
        }
    }

private:
    // NaN and out-of-range positions land in the nearest edge cell.
    int column(float x) const {
        const float v = (x - minX_) / cell_;
        return static_cast<int>(std::min(v > 0.0f ? v : 0.0f, static_cast<float>(cols_ - 1)));
    }
    int row(float y) const {
        const float v = (y - minY_) / cell_;
        return static_cast<int>(std::min(v > 0.0f ? v : 0.0f, static_cast<float>(rows_ - 1)));
    }
    size_t cellOf(float x, float y) const { return static_cast<size_t>(row(y)) * static_cast<size_t>(cols_) + static_cast<size_t>(column(x)); }

    float minX_ = 0.0f, minY_ = 0.0f, cell_ = 1.0f;
    int cols_ = 1, rows_ = 1;
    std::vector<uint32_t> start_;
    std::vector<uint32_t> items_;
};

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
}

}  // namespace jitnodes
