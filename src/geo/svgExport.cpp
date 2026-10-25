#include "jitnodes/geo/svgExport.h"

#include <algorithm>
#include <cstdio>

namespace jitnodes {

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

    for (const Primitive& p : geo.primitives()) {
        if (p.count == 0) continue;
        if (const uint32_t segments = cubicSegments(p)) {
            std::snprintf(buf, sizeof(buf), "  <path d=\"M%.6g,%.6g", px(p.start), py(p.start));
            out += buf;
            for (uint32_t s = 0; s < segments; ++s) {
                const size_t i = p.start + 3 * s;
                std::snprintf(buf, sizeof(buf), " C%.6g,%.6g %.6g,%.6g %.6g,%.6g", px(i + 1), py(i + 1), px(i + 2), py(i + 2),
                              px(p.start + (3 * s + 3) % p.count), py(p.start + (3 * s + 3) % p.count));
                out += buf;
            }
            if (p.closed) out += " Z";
            std::snprintf(buf, sizeof(buf), "\" fill=\"none\" stroke=\"%s\" stroke-width=\"%.6g\" stroke-linejoin=\"round\"/>\n", options.stroke.c_str(), strokeWidth);
            out += buf;
            continue;
        }
        out += p.closed ? "  <polygon points=\"" : "  <polyline points=\"";
        for (uint32_t j = 0; j < p.count; ++j) {
            std::snprintf(buf, sizeof(buf), "%s%.6g,%.6g", j ? " " : "", px(p.start + j), py(p.start + j));
            out += buf;
        }
        std::snprintf(buf, sizeof(buf), "\" fill=\"none\" stroke=\"%s\" stroke-width=\"%.6g\" stroke-linejoin=\"round\"/>\n", options.stroke.c_str(), strokeWidth);
        out += buf;
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
