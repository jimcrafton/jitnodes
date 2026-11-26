#pragma once

#include <cstddef>
#include <vector>

#include "jitnodes/geo/brushEnvelope.h"
#include "jitnodes/geo/curveFit.h"

namespace jitnodes {

// Turns the borders of a brush sweep into closed cubic outlines.
// @scriptable ignore=true
class OutlineFit {
public:
    // One closed cubic from a closed polygon: 3n control points, anchor, out handle, in handle, ..., the last segment running back to the first
    // point (the layout of a closed PrimKind::cubic). `breaks` are polygon indices where the outline has a corner. The line is cut at the first
    // point as well, so the join there is C0.
    static std::vector<CurveFit::Point> closedCubic(const std::vector<CurveFit::Point>& polygon, const std::vector<std::size_t>& breaks, double tolerance);

    // The closed cubic of an open sweep, fitted so that growing the trajectory at its end does not move the cubics near its start: seamed in the middle
    // of the start cap, with the left and the right border each fitted forward in stroke order (CurveFit::Mode::forward), joined at the end cap.
    // `breaks` are indices into BrushEnvelope::polygon(borders). Empty when the sweep has no caps to seam on (the caller falls back to closedCubic).
    static std::vector<CurveFit::Point> openStrokeCubic(const BrushBorders& borders, const std::vector<std::size_t>& breaks, double tolerance);

    // The closed cubics of a sweep: one for an open trajectory (left border, end cap, right border backwards, start cap) or for a single point
    // (the tip); two for a closed trajectory (the left loop and the right loop reversed, so filling both non-zero leaves the inside open).
    static std::vector<std::vector<CurveFit::Point>> outlines(const BrushBorders& borders, double tolerance);
};

}  // namespace jitnodes
