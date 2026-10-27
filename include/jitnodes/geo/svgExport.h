#pragma once

#include <string>

#include "jitnodes/geo/geometry.h"

namespace jitnodes {

struct SvgOptions {
    int width = 800;          // pixel width; height follows the aspect ratio
    bool drawPoints = false;  // also draw every point as a small dot
    std::string stroke = "#1f2933";
    std::string pointFill = "#e4572e";
    std::string background;   // empty = transparent
    bool useColor = false;    // stroke each primitive with its first point's Cd/alpha; fill closed ones when render.fill is set
};

// Draws the primitives of a 2D geometry (x/y point channels) as polylines/polygons. +y is up.
std::string geometryToSvg(const Geometry& geo, const SvgOptions& options = {});

}  // namespace jitnodes
