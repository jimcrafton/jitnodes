#pragma once

#include <cstddef>
#include <vector>

#include "jitnodes/geo/curveFit.h"

namespace jitnodes {

struct TrajectoryParams {
    double tolerance = 0.3;   // how far the fitted curve may stray from the input samples (document units)
    double flatness = 0.05;   // how far the polygon may stray from the fitted curve
    bool forward = false;     // CurveFit::Mode::forward: the start of the curve does not move when samples are added at the end (a stroke being drawn)
};

// The fitted trajectory of a stroke.
struct Trajectory {
    std::vector<CurveFit::Anchor> anchors;       // the cubics (3n+1 control points through CurveFit::controlPoints); Anchor::index is an input point
    std::vector<CurveFit::Point> vertices;       // the same curve as a polygon
    std::vector<double> arc;                     // per vertex: arc length from the start, normalised so the last is 1 (all 0 for a point)
    std::vector<std::vector<double>> attributes; // per attribute (in the order given), the value at each vertex
    std::vector<std::size_t> anchorVertex;       // per anchor: the vertex it is
    double length = 0.0;                         // arc length of the polygon in document units
};

// Fits a stroke's points to cubics and flattens the result, carrying point attributes (pressure, speed, tilt) onto the polygon by arc length: the
// value at an input point is placed at that point's normalised chord length, and each vertex takes the value interpolated at its own arc
// length (Pudet 1993, 3.4). Plain geometry, no graph, so brushes, scatter and skeletal strokes share it.
// @scriptable ignore=true
class TrajectoryFit {
public:
    // `attributes[a][i]` is attribute a at input point i; every list must have `points.size()` values. Repeated points are dropped by the fit.
    static Trajectory fit(const std::vector<CurveFit::Point>& points, const std::vector<std::vector<double>>& attributes, const TrajectoryParams& params = {},
                          const std::vector<unsigned>& breakpoints = {});
};

}  // namespace jitnodes
