#pragma once

#include <cstddef>
#include <vector>

#include "jitnodes/geo/curveFit.h"
#include "jitnodes/geo/nibBrush.h"

namespace jitnodes {

// The outline of what a brush sweeps along a trajectory, as border polygons ready to be fitted to cubics.
struct BrushBorders {
    // "Left" is the side counter-clockwise of the direction of travel in a y-up plane, "right" the other, both listed in the direction of travel.
    // An open trajectory gives one left and one right border; a closed one gives two closed loops (one outer, one inner, which is which depends on
    // the direction of travel) and no caps.
    std::vector<CurveFit::Point> left;
    std::vector<CurveFit::Point> right;
    // Indices into left / right where the border must be cut and joined C0 only: the two points of a shortcut across a sharp inner turn.
    std::vector<std::size_t> leftBreaks;
    std::vector<std::size_t> rightBreaks;
    // The tip's own corners between the right and left border at the start (after the right border, in outline order) and between the left and right
    // border at the end, without the border points they join. Empty for a closed trajectory.
    std::vector<CurveFit::Point> startCap;
    std::vector<CurveFit::Point> endCap;
    bool closed = false;
    bool single = false;   // fewer than two distinct points: the outline is the tip itself, in startCap
};

// Pudet's walk (Pudet 1993, "Real time fitting of hand-sketched pressure brushes") for a convex tip: on each segment of the trajectory the tip
// touches each border with one fixed corner; at a vertex the border follows the tip round the outside of a turn, and across the inside of a turn
// takes the crossing of the two offset segments, or, where they do not cross (a turn sharper than the segments are long), jumps and reports the
// two points as breakpoints. One point per vertex on smooth stretches, so the cost is linear in the trajectory.
// @scriptable ignore=true
class BrushEnvelope {
public:
    // `scale[i]` is the tip's scale at `vertices[i]` (empty: 1). A closed trajectory is given without repeating its first vertex. Repeated vertices
    // are dropped.
    static BrushBorders sweep(const NibBrush& nib, const std::vector<CurveFit::Point>& vertices, const std::vector<double>& scale, bool closed);

    // One closed polygon of an open sweep: left border, end cap, right border backwards, start cap (clockwise in a y-up plane). For a `single`
    // result the tip; for a closed sweep both loops one after the other (not a single polygon).
    static std::vector<CurveFit::Point> polygon(const BrushBorders& borders);
};

}  // namespace jitnodes
