#pragma once

#include <string>

#include "jitnodes/geo/raster.h"

namespace jitnodes::editor {

// Writes the image as PNG (.png) or JPEG (.jpg, .jpeg) chosen by the extension of `path` (UTF-8), through the Windows imaging
// component. `jpegQuality` is 1..100. Returns an error message; empty on success.
std::string exportImage(const Image& image, const std::string& path, int jpegQuality = 92);

}  // namespace jitnodes::editor
