#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "jitnodes/core/threadPool.h"
#include "jitnodes/geo/geometry.h"

namespace jitnodes {

// Pixels are 0xAARRGGBB, row 0 at the top.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
};

struct RenderSettings {
    int width = 1400;
    int height = 700;
    // World window mapped onto the image; the y axis points up.
    float minX = 0.0f, minY = 0.0f, maxX = 1.0f, maxY = 1.0f;
    bool additive = true;
    float exposure = 1.0f;
    float gamma = 2.2f;
    float bloom = 0.0f;
    float bloomRadius = 12.0f;  // pixels
    float background[3] = {0.0f, 0.0f, 0.0f};
};

// The `render` node stores its settings as geometry detail values ("render.*"); hasRenderSettings tells whether a geometry carries them.
bool hasRenderSettings(const Geometry& geo);
// Settings from the detail values for an image of width x height pixels (0 takes the node's size). The world window is the node's view
// window when it has one, else the bounds of the points; it is widened to the image's aspect so pixels stay square.
RenderSettings renderSettingsFromDetails(const Geometry& geo, int width = 0, int height = 0);

// Draws the points that have a `radius` attribute as sprites (Cd.x/y/z, alpha, softness, aspect, shape as in the editor viewport). Additive
// rendering sums the sprites in linear light and tone maps the result; otherwise sprites are composited in index order over the background.
Image renderSplats(const Geometry& geo, const RenderSettings& settings, ThreadPool* pool = nullptr);

// Writes an uncompressed PNG; throws when the file cannot be written.
void writePng(const std::string& path, const Image& image);

}  // namespace jitnodes
