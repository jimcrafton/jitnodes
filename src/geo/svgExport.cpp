#include "jitnodes/geo/svgExport.h"

#include <algorithm>
#include <cstdio>

namespace jitnodes {

namespace {

std::string escapeXml(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\n': out += "&#10;"; break;
            case '\t': out += "&#9;"; break;
            case '\r': out += "&#13;"; break;
            default:
                if (static_cast<unsigned char>(c) >= 0x20 || c < 0) out += c;  // other control characters are not allowed in XML
        }
    }
    return out;
}

// ` id="..." class="..."` from the primitive string attributes of those names; empty text is left out.
std::string identity(const Geometry& geo, size_t prim) {
    std::string out;
    for (const char* name : {"id", "class"}) {
        const AttribArray* a = geo.findAttrib(AttribClass::primitive, name);
        if (a == nullptr || a->type() != AttribType::string || prim >= a->size()) continue;
        const std::string& text = a->stringAt(prim);
        if (!text.empty()) out += std::string(" ") + name + "=\"" + escapeXml(text) + "\"";
    }
    return out;
}

float channelAt(const AttribArray* a, size_t i, float fallback) { return a != nullptr && i < a->size() ? a->at(i) : fallback; }

// Paint attributes for one primitive: `fill`/`stroke` plus opacity, taken from the first point's Cd and alpha.
struct Paint {
    const AttribArray* red;
    const AttribArray* green;
    const AttribArray* blue;
    const AttribArray* alpha;
    bool fillClosed;
    std::string stroke;

    std::string color(size_t point) const {
        auto byte = [](float v) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        char buf[32];
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", byte(channelAt(red, point, 1.0f)), byte(channelAt(green, point, 1.0f)), byte(channelAt(blue, point, 1.0f)));
        return buf;
    }

    std::string attributes(const Primitive& p, float strokeWidth, bool closedShape) const {
        char buf[256];
        if (red == nullptr && green == nullptr && blue == nullptr && alpha == nullptr && !fillClosed) {
            std::snprintf(buf, sizeof(buf), " fill=\"none\" stroke=\"%s\" stroke-width=\"%.6g\" stroke-linejoin=\"round\"", stroke.c_str(), strokeWidth);
            return buf;
        }
        const std::string rgb = color(p.start);
        const float a = std::clamp(channelAt(alpha, p.start, 1.0f), 0.0f, 1.0f);
        if (fillClosed && closedShape) std::snprintf(buf, sizeof(buf), " fill=\"%s\" fill-opacity=\"%.3g\" stroke=\"none\"", rgb.c_str(), a);
        else std::snprintf(buf, sizeof(buf), " fill=\"none\" stroke=\"%s\" stroke-opacity=\"%.3g\" stroke-width=\"%.6g\" stroke-linejoin=\"round\"", rgb.c_str(), a, strokeWidth);
        return buf;
    }
};

}  // namespace

std::string geometryToSvg(const Geometry& geo, const SvgOptions& options) {
    const float* x = geo.x();
    const float* y = geo.y();
    float minX = 0.0f, maxX = 1.0f, minY = 0.0f, maxY = 1.0f;
    if (geo.pointCount()) {
        minX = maxX = x[0];
        minY = maxY = y[0];
        for (size_t i = 1; i < geo.pointCount(); ++i) {
            minX = std::min(minX, x[i]);
            maxX = std::max(maxX, x[i]);
            minY = std::min(minY, y[i]);
            maxY = std::max(maxY, y[i]);
        }
    }
    const float extent = std::max({maxX - minX, maxY - minY, 1e-6f});
    const float pad = extent * 0.05f;
    const float width = (maxX - minX) + 2 * pad;
    const float height = (maxY - minY) + 2 * pad;
    const float strokeWidth = extent * 0.004f;
    const int pixelHeight = std::max(1, static_cast<int>(static_cast<float>(options.width) * height / width));

    auto px = [&](size_t i) { return x[i] - minX + pad; };
    auto py = [&](size_t i) { return maxY - y[i] + pad; };

    std::string out;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %.6g %.6g\">\n",
                  options.width, pixelHeight, width, height);
    out += buf;
    if (!options.background.empty()) out += "  <rect width=\"100%\" height=\"100%\" fill=\"" + options.background + "\"/>\n";

    Paint paint{nullptr, nullptr, nullptr, nullptr, false, options.stroke};
    if (options.useColor) {
        paint.red = geo.findAttrib(AttribClass::point, "Cd.x");
        paint.green = geo.findAttrib(AttribClass::point, "Cd.y");
        paint.blue = geo.findAttrib(AttribClass::point, "Cd.z");
        paint.alpha = geo.findAttrib(AttribClass::point, "alpha");
        paint.fillClosed = geo.detail("render.fill", 0.0f) >= 0.5f;
    }

    for (size_t primIndex = 0; primIndex < geo.primitiveCount(); ++primIndex) {
        const Primitive& p = geo.primitives()[primIndex];
        if (p.count == 0) continue;
        const std::string ident = identity(geo, primIndex);
        if (const uint32_t segments = cubicSegments(p)) {
            out += "  <path" + ident;
            std::snprintf(buf, sizeof(buf), " d=\"M%.6g,%.6g", px(p.start), py(p.start));
            out += buf;
            for (uint32_t s = 0; s < segments; ++s) {
                const size_t i = p.start + 3 * s;
                std::snprintf(buf, sizeof(buf), " C%.6g,%.6g %.6g,%.6g %.6g,%.6g", px(i + 1), py(i + 1), px(i + 2), py(i + 2),
                              px(p.start + (3 * s + 3) % p.count), py(p.start + (3 * s + 3) % p.count));
                out += buf;
            }
            if (p.closed) out += " Z";
            out += "\"" + paint.attributes(p, strokeWidth, p.closed) + "/>\n";
            continue;
        }
        out += (p.closed ? "  <polygon" : "  <polyline") + ident + " points=\"";
        for (uint32_t j = 0; j < p.count; ++j) {
            std::snprintf(buf, sizeof(buf), "%s%.6g,%.6g", j ? " " : "", px(p.start + j), py(p.start + j));
            out += buf;
        }
        out += "\"" + paint.attributes(p, strokeWidth, p.closed && p.count >= 3) + "/>\n";
    }
    if (options.drawPoints) {
        for (size_t i = 0; i < geo.pointCount(); ++i) {
            std::snprintf(buf, sizeof(buf), "  <circle cx=\"%.6g\" cy=\"%.6g\" r=\"%.6g\" fill=\"%s\"/>\n", px(i), py(i), strokeWidth * 1.5f, options.pointFill.c_str());
            out += buf;
        }
    }
    out += "</svg>\n";
    return out;
}

}  // namespace jitnodes
