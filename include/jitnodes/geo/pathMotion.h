#pragma once

#include <string>

#include "jitnodes/geo/geometry.h"

namespace jitnodes {

// How PathMotion::compute reads and smooths a stroke.
struct PathMotionParams {
    std::string timeAttr = "t";   // point attribute holding the sample time in seconds
    float smoothing = 0.3f;       // exponential moving average weight of the newest velocity, 0 < smoothing <= 1 (1: no smoothing)
};

// Derives the motion of a stroke from its positions and sample times, one open or closed polyline at a time: `s` (arc length from the primitive's
// start), `dt`, the smoothed velocity `vx`, `vy`, `speed`, its change `ax`, `ay`, `accel` (the signed rate of change of speed) and `heading`
// (radians, direction of the smoothed velocity). The first point of a primitive takes the values of the second, so a stroke does not start at
// rest; samples with no time between them (dt <= 0) repeat the previous values. Used by the pathMotion node and by the editor's stroke recorder,
// so there is one implementation.
// @scriptable ignore=true
class PathMotion {
public:
    // Adds or overwrites the attributes above on `geo`. Throws std::runtime_error when the time attribute is missing or `smoothing` is out of range.
    static void compute(Geometry& geo, const PathMotionParams& params = {});
};

}  // namespace jitnodes
