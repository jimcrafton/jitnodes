#include "jitnodes/geo/raster.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace jitnodes {

namespace {

struct Sprite {
    float sx, sy;  // centre in pixels
    float hw, hh;  // half extents in pixels
    float r, g, b, a;
    float softness;
    bool box;
};

constexpr float subPixelRadius = 0.75f;

float channel(const AttribArray* a, size_t i, float fallback) { return a ? a->at(i) : fallback; }

void blend(float* px, float r, float g, float b, float a, bool additive) {
    if (additive) {
        px[0] += r * a;
        px[1] += g * a;
        px[2] += b * a;
    } else {
        const float keep = 1.0f - a;
        px[0] = r * a + px[0] * keep;
        px[1] = g * a + px[1] * keep;
        px[2] = b * a + px[2] * keep;
        px[3] = a + px[3] * keep;
    }
}

float profile(float t, float softness) {
    if (softness < 0.01f) return t < 1.0f ? 1.0f : 0.0f;
    const float u = std::clamp((t - (1.0f - softness)) / softness, 0.0f, 1.0f);
    return (1.0f - u) * (1.0f - u) * (1.0f + 2.0f * u);
}

void splatBand(const std::vector<Sprite>& sprites, std::vector<float>& buf, int w, int y0, int y1, bool additive) {
    for (const Sprite& s : sprites) {
        if (s.sy + s.hh < static_cast<float>(y0) || s.sy - s.hh >= static_cast<float>(y1)) continue;
        if (s.hw < subPixelRadius && s.hh < subPixelRadius) {
            // A sprite smaller than a pixel keeps its energy (area times alpha) by spreading over the four nearest pixels.
            const float weight = s.a * (s.hw * s.hh) / (subPixelRadius * subPixelRadius);
            const float fx = s.sx - 0.5f, fy = s.sy - 0.5f;
            const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
            const float tx = fx - static_cast<float>(ix), ty = fy - static_cast<float>(iy);
            for (int dy = 0; dy < 2; ++dy) {
                const int y = iy + dy;
                if (y < y0 || y >= y1) continue;
                for (int dx = 0; dx < 2; ++dx) {
                    const int x = ix + dx;
                    if (x < 0 || x >= w) continue;
                    const float wgt = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty) * weight;
                    blend(&buf[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4], s.r, s.g, s.b, wgt, additive);
                }
            }
            continue;
        }
        const int xa = std::max(0, static_cast<int>(std::floor(s.sx - s.hw)));
        const int xb = std::min(w - 1, static_cast<int>(std::ceil(s.sx + s.hw)));
        const int ya = std::max(y0, static_cast<int>(std::floor(s.sy - s.hh)));
        const int yb = std::min(y1 - 1, static_cast<int>(std::ceil(s.sy + s.hh)));
        const float invW = 1.0f / s.hw, invH = 1.0f / s.hh;
        for (int y = ya; y <= yb; ++y) {
            const float dy = (static_cast<float>(y) + 0.5f - s.sy) * invH;
            for (int x = xa; x <= xb; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f - s.sx) * invW;
                float f;
                if (s.box) {
                    if (std::fabs(dx) > 1.0f || std::fabs(dy) > 1.0f) continue;
                    f = 1.0f;
                } else {
                    const float t2 = dx * dx + dy * dy;
                    if (t2 >= 1.0f) continue;
                    f = profile(std::sqrt(t2), s.softness);
                }
                blend(&buf[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4], s.r, s.g, s.b, s.a * f, additive);
            }
        }
    }
}

// Three box blur passes approximate a Gaussian; each pass is a running sum, so the cost does not depend on the radius.
void boxBlur(std::vector<float>& rgb, int w, int h, int radius, ThreadPool* pool) {
    if (radius < 1) return;
    std::vector<float> tmp(rgb.size());
    const float norm = 1.0f / static_cast<float>(2 * radius + 1);
    auto run = [&](size_t lines, size_t lineStride, size_t step, size_t length, std::vector<float>& src, std::vector<float>& dst) {
        auto one = [&](size_t line) {
            const size_t base = line * lineStride;
            for (size_t c = 0; c < 3; ++c) {
                float sum = 0.0f;
                auto at = [&](long i) { return src[base + static_cast<size_t>(std::clamp<long>(i, 0, static_cast<long>(length) - 1)) * step + c]; };
                for (long i = -radius; i <= radius; ++i) sum += at(i);
                for (size_t i = 0; i < length; ++i) {
                    dst[base + i * step + c] = sum * norm;
                    sum += at(static_cast<long>(i) + radius + 1) - at(static_cast<long>(i) - radius);
                }
            }
        };
        if (pool) pool->parallelFor(lines, one);
        else for (size_t l = 0; l < lines; ++l) one(l);
    };
    for (int pass = 0; pass < 3; ++pass) {
        run(static_cast<size_t>(h), static_cast<size_t>(w) * 3, 3, static_cast<size_t>(w), rgb, tmp);
        run(static_cast<size_t>(w), 3, static_cast<size_t>(w) * 3, static_cast<size_t>(h), tmp, rgb);
    }
}

uint32_t pixelHash(uint32_t x, uint32_t y) {
    uint32_t h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

void putU32(std::string& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>((v >> shift) & 255u));
}

uint32_t crc32(const char* data, size_t n, uint32_t crc = 0) {
    static const auto table = [] {
        std::unique_ptr<uint32_t[]> t(new uint32_t[256]);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ static_cast<uint8_t>(data[i])) & 255u] ^ (crc >> 8);
    return ~crc;
}

void putChunk(std::string& out, const char* type, const std::string& body) {
    putU32(out, static_cast<uint32_t>(body.size()));
    std::string typed(type, 4);
    typed += body;
    out += typed;
    putU32(out, crc32(typed.data(), typed.size()));
}

}  // namespace

bool hasRenderSettings(const Geometry& geo) { return geo.details().count("render.enabled") != 0; }

RenderSettings renderSettingsFromDetails(const Geometry& geo, int width, int height) {
    RenderSettings s;
    s.width = width > 0 ? width : static_cast<int>(geo.detail("render.width", 1400.0f));
    s.height = height > 0 ? height : static_cast<int>(geo.detail("render.height", 700.0f));
    s.width = std::clamp(s.width, 1, 16384);
    s.height = std::clamp(s.height, 1, 16384);
    s.additive = geo.detail("render.additive", 1.0f) >= 0.5f;
    s.exposure = geo.detail("render.exposure", 1.0f);
    s.gamma = std::max(geo.detail("render.gamma", 2.2f), 0.1f);
    s.bloom = geo.detail("render.bloom", 0.0f);
    s.bloomRadius = geo.detail("render.bloomRadius", 12.0f);
    s.background[0] = geo.detail("render.bgR", 0.0f);
    s.background[1] = geo.detail("render.bgG", 0.0f);
    s.background[2] = geo.detail("render.bgB", 0.0f);

    float cx = 0.0f, cy = 0.0f, viewW = 0.0f, viewH = 0.0f;
    const float requested = geo.detail("render.viewWidth", 0.0f);
    if (requested > 0.0f) {
        cx = geo.detail("render.viewCx", 0.0f);
        cy = geo.detail("render.viewCy", 0.0f);
        viewW = requested;
        viewH = requested * static_cast<float>(s.height) / static_cast<float>(s.width);
    } else if (geo.pointCount() > 0) {
        const float* x = geo.x();
        const float* y = geo.y();
        float minX = x[0], maxX = x[0], minY = y[0], maxY = y[0];
        for (size_t i = 1; i < geo.pointCount(); ++i) {
            minX = std::min(minX, x[i]);
            maxX = std::max(maxX, x[i]);
            minY = std::min(minY, y[i]);
            maxY = std::max(maxY, y[i]);
        }
        cx = (minX + maxX) * 0.5f;
        cy = (minY + maxY) * 0.5f;
        viewW = std::max(maxX - minX, 1e-6f);
        viewH = std::max(maxY - minY, 1e-6f);
        const float aspect = static_cast<float>(s.width) / static_cast<float>(s.height);
        if (viewW / viewH < aspect) viewW = viewH * aspect;
        else viewH = viewW / aspect;
    } else {
        viewW = 1.0f;
        viewH = static_cast<float>(s.height) / static_cast<float>(s.width);
    }
    s.minX = cx - viewW * 0.5f;
    s.maxX = cx + viewW * 0.5f;
    s.minY = cy - viewH * 0.5f;
    s.maxY = cy + viewH * 0.5f;
    return s;
}

Image renderSplats(const Geometry& geo, const RenderSettings& settings, ThreadPool* pool) {
    const int w = settings.width, h = settings.height;
    const float scaleX = static_cast<float>(w) / (settings.maxX - settings.minX);
    const float scaleY = static_cast<float>(h) / (settings.maxY - settings.minY);

    std::vector<Sprite> sprites;
    if (const AttribArray* radius = geo.findAttrib(AttribClass::point, "radius")) {
        const AttribArray* red = geo.findAttrib(AttribClass::point, "Cd.x");
        const AttribArray* green = geo.findAttrib(AttribClass::point, "Cd.y");
        const AttribArray* blue = geo.findAttrib(AttribClass::point, "Cd.z");
        const AttribArray* alpha = geo.findAttrib(AttribClass::point, "alpha");
        const AttribArray* softness = geo.findAttrib(AttribClass::point, "softness");
        const AttribArray* aspect = geo.findAttrib(AttribClass::point, "aspect");
        const AttribArray* shape = geo.findAttrib(AttribClass::point, "shape");
        const float* xs = geo.x();
        const float* ys = geo.y();
        sprites.reserve(geo.pointCount());
        for (size_t i = 0; i < geo.pointCount(); ++i) {
            const float r = radius->at(i) * scaleX;
            const float a = std::clamp(channel(alpha, i, 1.0f), 0.0f, 1.0f);
            if (!(r > 0.0f) || !(a > 0.0f)) continue;
            const float stretch = channel(aspect, i, 1.0f);
            Sprite s;
            s.sx = (xs[i] - settings.minX) * scaleX;
            s.sy = (settings.maxY - ys[i]) * scaleY;
            s.hw = r * (stretch > 0.0f ? stretch : 1.0f);
            s.hh = r;
            if (!(s.sx + s.hw >= 0.0f && s.sx - s.hw < static_cast<float>(w) && s.sy + s.hh >= 0.0f && s.sy - s.hh < static_cast<float>(h))) continue;
            s.r = channel(red, i, 1.0f);
            s.g = channel(green, i, 1.0f);
            s.b = channel(blue, i, 1.0f);
            s.a = a;
            s.softness = std::clamp(channel(softness, i, 1.0f), 0.0f, 1.0f);
            s.box = channel(shape, i, 0.0f) >= 0.5f;
            sprites.push_back(s);
        }
    }

    std::vector<float> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0.0f);
    const int bands = pool ? std::clamp(static_cast<int>(pool->concurrency()) * 4, 1, h) : 1;
    auto band = [&](size_t b) {
        const int y0 = static_cast<int>(static_cast<size_t>(h) * b / static_cast<size_t>(bands));
        const int y1 = static_cast<int>(static_cast<size_t>(h) * (b + 1) / static_cast<size_t>(bands));
        splatBand(sprites, buf, w, y0, y1, settings.additive);
    };
    if (pool && bands > 1) pool->parallelFor(static_cast<size_t>(bands), band);
    else for (int b = 0; b < bands; ++b) band(static_cast<size_t>(b));

    const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<float> glow;
    if (settings.additive && settings.bloom > 0.0f) {
        glow.resize(pixelCount * 3);
        for (size_t i = 0; i < pixelCount; ++i)
            for (size_t c = 0; c < 3; ++c) glow[i * 3 + c] = buf[i * 4 + c];
        boxBlur(glow, w, h, static_cast<int>(std::lround(settings.bloomRadius / 3.0f)), pool);
    }

    Image image;
    image.width = w;
    image.height = h;
    image.pixels.resize(pixelCount);
    const float invGamma = 1.0f / settings.gamma;
    auto finish = [&](size_t row) {
        for (int x = 0; x < w; ++x) {
            const size_t i = row * static_cast<size_t>(w) + static_cast<size_t>(x);
            float rgb[3];
            for (size_t c = 0; c < 3; ++c) {
                float v = buf[i * 4 + c];
                if (settings.additive) {
                    if (!glow.empty()) v += settings.bloom * glow[i * 3 + c];
                    const float lit = 1.0f - std::exp(-settings.exposure * v);
                    v = settings.background[c] + lit * (1.0f - settings.background[c]);
                    v = std::pow(std::clamp(v, 0.0f, 1.0f), invGamma);
                } else {
                    v += settings.background[c] * (1.0f - buf[i * 4 + 3]);
                }
                rgb[c] = v;
            }
            // Dithering hides the banding of slow dark gradients.
            const float dither = (static_cast<float>(pixelHash(static_cast<uint32_t>(x), static_cast<uint32_t>(row)) & 255u) / 255.0f - 0.5f) / 255.0f;
            uint32_t out = 0xFF000000u;
            for (size_t c = 0; c < 3; ++c) {
                const uint32_t byte = static_cast<uint32_t>(std::lround(std::clamp(rgb[c] + dither, 0.0f, 1.0f) * 255.0f));
                out |= byte << (16 - 8 * c);
            }
            image.pixels[i] = out;
        }
    };
    if (pool) pool->parallelFor(static_cast<size_t>(h), finish);
    else for (int y = 0; y < h; ++y) finish(static_cast<size_t>(y));
    return image;
}

void writePng(const std::string& path, const Image& image) {
    std::string raw;
    raw.reserve(static_cast<size_t>(image.height) * (static_cast<size_t>(image.width) * 3 + 1));
    for (int y = 0; y < image.height; ++y) {
        raw.push_back(0);
        for (int x = 0; x < image.width; ++x) {
            const uint32_t px = image.pixels[static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)];
            raw.push_back(static_cast<char>((px >> 16) & 255u));
            raw.push_back(static_cast<char>((px >> 8) & 255u));
            raw.push_back(static_cast<char>(px & 255u));
        }
    }

    // zlib stream of stored (uncompressed) deflate blocks.
    std::string z = {0x78, 0x01};
    uint32_t s1 = 1, s2 = 0;
    for (char c : raw) {
        s1 = (s1 + static_cast<uint8_t>(c)) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    for (size_t pos = 0; pos < raw.size() || pos == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n >= raw.size() ? 1 : 0);
        z.push_back(static_cast<char>(n & 255u));
        z.push_back(static_cast<char>(n >> 8));
        z.push_back(static_cast<char>(~n & 255u));
        z.push_back(static_cast<char>((~n >> 8) & 255u));
        z.append(raw, pos, n);
        pos += n;
        if (n == 0) break;
    }
    putU32(z, (s2 << 16) | s1);

    std::string out = "\x89PNG\r\n\x1a\n";
    std::string header;
    putU32(header, static_cast<uint32_t>(image.width));
    putU32(header, static_cast<uint32_t>(image.height));
    header += {8, 2, 0, 0, 0};
    putChunk(out, "IHDR", header);
    putChunk(out, "IDAT", z);
    putChunk(out, "IEND", "");

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot open '" + path + "' for writing");
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (!ok) throw std::runtime_error("failed writing '" + path + "'");
}

}  // namespace jitnodes
