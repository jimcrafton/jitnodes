#pragma once

#include <cstdint>
#include <vector>

namespace jitnodes {

// Turns a polyline (a hand drawn stroke, the border of a brush sweep) into a few cubic Beziers. Plain geometry with no document or graph, so
// nodes, the editor and scripts share one implementation.
// @scriptable ignore=true
class CurveFit {
public:
    struct Point {
        double x = 0.0;
        double y = 0.0;
    };

    // One anchor with its two handles (absolute positions). An anchor without handles is a corner.
    struct Anchor {
        Point point;
        Point in;                // toward the previous anchor
        Point out;               // toward the next anchor
        bool hasIn = false;
        bool hasOut = false;
        uint32_t index = 0;      // the input point this anchor sits on, so curve k covers the input points from anchors[k].index to anchors[k + 1].index
    };

    // Schneider's "automatically fitting digitized curves": as few segments as keep the curve within `tolerance` of every sample, smooth joins
    // except at sharp corners (turns over about 60 degrees), where the line is cut and each side fitted separately. `breakpoints` are input point
    // indices where the line must be cut as well (the join there is C0 only); ones at the ends or out of range are ignored. Repeated samples are
    // dropped; fewer than three distinct points give what there is.
    //
    // `Mode::forward` fits from the first sample onward: each cubic is grown as far as it stays within tolerance, then kept, and the next starts at
    // its end with the tangent read from the samples around the join. A curve therefore depends only on the samples up to a short reach past its
    // end, so a stroke that is still being drawn does not rearrange its beginning when samples are added (the global mode splits at the worst
    // error of the whole run and takes its end tangents from reaches proportional to the whole length, so every new sample can move the start).
    enum class Mode { global, forward };

    //
    // `startAlong` / `endAlong` (unit vectors, either may be null) fix the direction of travel at the first and the last sample, for a caller
    // that knows the curve continues past an end (the halves of a closed outline sharing one seam tangent, or a loop meeting itself).
    // `rounded` (ascending input indices) names points the caller knows lie on a curve however fast it turns there, such as an arc of a brush
    // tip: no corner is found at them, and where a straight run meets a long enough one the two are fitted apart,
    // joined along the straight.
    static std::vector<Anchor> fit(const std::vector<Point>& points, double tolerance, const std::vector<uint32_t>& breakpoints = {},
                                   Mode mode = Mode::global, const Point* startAlong = nullptr, const Point* endAlong = nullptr,
                                   const std::vector<uint32_t>& rounded = {});

    // Each pass replaces every point but the two ends with (previous + 2 * itself + next) / 4.
    static std::vector<Point> smooth(const std::vector<Point>& points, int passes);

    // The anchors as control points in SVG order (anchor, out handle, in handle, anchor, ...): 3n+1 points for n+1 anchors, a missing handle sits
    // on its anchor.
    static std::vector<Point> controlPoints(const std::vector<Anchor>& anchors);
};

}  // namespace jitnodes
