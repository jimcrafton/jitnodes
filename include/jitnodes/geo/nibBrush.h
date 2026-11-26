#pragma once

#include <cstddef>
#include <vector>

#include "jitnodes/geo/curveFit.h"

namespace jitnodes {

// A brush tip: a convex polygon centred on the origin, corners counter-clockwise (y up), that BrushEnvelope sweeps along a trajectory. The tip is
// scaled about its centre by the pressure at each place. A round pen, a slanted oval, a flat chisel are all this; any convex polygon works.
// @scriptable ignore=true
class NibBrush {
public:
    std::vector<CurveFit::Point> corners;

    // A slanted oval: `width` is its long diameter, `angleDegrees` its tilt counter-clockwise from horizontal, `roundness` its thickness as a
    // percentage of its length (100 is a circle, small is a chisel edge).
    static NibBrush oval(double width, double angleDegrees, double roundness, int sides = 16);

    std::size_t size() const { return corners.size(); }
    // The corner furthest along `direction` (the first of equals): where a sweep in a direction at right angles to it touches the tip.
    std::size_t support(double dx, double dy) const;
};

}  // namespace jitnodes
