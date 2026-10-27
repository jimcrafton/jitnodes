#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "jitnodes/geo/geometry.h"

namespace jitnodes {

// The .jngeo binary geometry format; see "Binary geometry files" in docs/design.md and docs/geometryFiles.md.
// Loading throws std::runtime_error for anything that is not a complete, checksummed, in-bounds file.
std::vector<uint8_t> saveGeometry(const Geometry& geo);
Geometry loadGeometry(const uint8_t* data, size_t size);

void saveGeometryToFile(const Geometry& geo, const std::string& path);
Geometry loadGeometryFromFile(const std::string& path);

}  // namespace jitnodes
