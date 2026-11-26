#include "jitnodes/geo/outlineFit.h"

#include <algorithm>
#include <cmath>

namespace jitnodes {

namespace {
using Point = CurveFit::Point;

// The fitter estimates a tangent from the samples near each end, which needs samples: a long straight edge between two vertices (the flattened
// straight part of a trajectory) is split so that no edge is longer than a few tolerances, or a curve spanning one edge takes its tangent from
// the far side of a corner and bulges. `newIndex[i]` is where polygon point i went; the closing edge is split too, the first point is not repeated.
std::vector<Point> subdivided(const std::vector<Point>& polygon, double tolerance, std::vector<uint32_t>& newIndex) {
    const double longest = std::max(tolerance * 8.0, 1e-3);
    std::vector<Point> ring;
    newIndex.assign(polygon.size() + 1, 0);
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        newIndex[i] = static_cast<uint32_t>(ring.size());
        ring.push_back(polygon[i]);
        const Point& a = polygon[i];
        const Point& b = polygon[(i + 1) % polygon.size()];
        const int pieces = static_cast<int>(std::min(std::ceil(std::hypot(b.x - a.x, b.y - a.y) / longest), 4096.0));
        for (int k = 1; k < pieces; ++k) {
            const double t = double(k) / pieces;
            ring.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
        }
    }
    return ring;
}

// The direction of the ring at index `at`, from the points `reach` of path length before and after it (so a symmetric cap gives the true tangent).
Point ringDirection(const std::vector<Point>& ring, std::size_t at, double reach) {
    const std::size_t n = ring.size();
    std::size_t forward = at, backward = at;
    double run = 0.0;
    for (std::size_t k = 0; k < n / 2 && run < reach; ++k) {
        const std::size_t next = (forward + 1) % n;
        run += std::hypot(ring[next].x - ring[forward].x, ring[next].y - ring[forward].y);
        forward = next;
    }
    run = 0.0;
    for (std::size_t k = 0; k < n / 2 && run < reach; ++k) {
        const std::size_t previous = (backward + n - 1) % n;
        run += std::hypot(ring[backward].x - ring[previous].x, ring[backward].y - ring[previous].y);
        backward = previous;
    }
    const double dx = ring[forward].x - ring[backward].x, dy = ring[forward].y - ring[backward].y;
    const double length = std::hypot(dx, dy);
    return length > 1e-12 ? Point{dx / length, dy / length} : Point{1.0, 0.0};
}
}  // namespace

std::vector<CurveFit::Point> OutlineFit::openStrokeCubic(const BrushBorders& borders, const std::vector<std::size_t>& breaks, double tolerance) {
    const std::vector<Point> polygon = BrushEnvelope::polygon(borders);
    const std::size_t cap = borders.startCap.size();
    if (polygon.size() < 8 || cap == 0 || borders.endCap.empty()) {
        return {};
    }
    // The ring is left border, end cap, right border backwards, start cap. Its seam sits in the middle of the start cap, which depends only on the
    // first stretch of the stroke; from there the left and the right border are fitted forward in stroke order, so neither changes where it has
    // been fitted when the stroke grows. They meet in the middle of the end cap, the live tip.
    const std::size_t seam = polygon.size() - cap + cap / 2;
    const std::size_t tip = borders.left.size() + borders.endCap.size() / 2;
    std::vector<uint32_t> newIndex;
    const std::vector<Point> ring = subdivided(polygon, tolerance, newIndex);
    const std::size_t n = ring.size();
    const std::size_t seamAt = newIndex[seam], tipAt = newIndex[tip];
    const double reach = 6.0 * tolerance;
    const Point seamDirection = ringDirection(ring, seamAt, reach), tipDirection = ringDirection(ring, tipAt, reach);

    std::vector<Point> forwardChain, backwardChain;
    for (std::size_t i = seamAt;; i = (i + 1) % n) {
        forwardChain.push_back(ring[i]);
        if (i == tipAt) {
            break;
        }
    }
    for (std::size_t i = seamAt;; i = (i + n - 1) % n) {
        backwardChain.push_back(ring[i]);
        if (i == tipAt) {
            break;
        }
    }
    std::vector<uint32_t> forwardBreaks, backwardBreaks;
    for (const std::size_t b : breaks) {
        if (b >= polygon.size()) {
            continue;
        }
        const std::size_t at = newIndex[b];
        const std::size_t f = (at + n - seamAt) % n, g = (seamAt + n - at) % n;
        if (f > 0 && f + 1 < forwardChain.size() && at != tipAt) {
            forwardBreaks.push_back(static_cast<uint32_t>(f));
        }
        if (g > 0 && g + 1 < backwardChain.size() && at != tipAt) {
            backwardBreaks.push_back(static_cast<uint32_t>(g));
        }
    }
    const Point backSeam{-seamDirection.x, -seamDirection.y}, backTip{-tipDirection.x, -tipDirection.y};
    const std::vector<CurveFit::Anchor> forwardAnchors =
        CurveFit::fit(forwardChain, tolerance, forwardBreaks, CurveFit::Mode::forward, &seamDirection, &tipDirection);
    const std::vector<CurveFit::Anchor> backwardAnchors =
        CurveFit::fit(backwardChain, tolerance, backwardBreaks, CurveFit::Mode::forward, &backSeam, &backTip);
    std::vector<Point> control = CurveFit::controlPoints(forwardAnchors);
    std::vector<Point> back = CurveFit::controlPoints(backwardAnchors);
    if (control.size() < 4 || back.size() < 4 || (control.size() - 1) % 3 != 0 || (back.size() - 1) % 3 != 0) {
        return {};
    }
    // The backward chain runs seam -> tip against the ring; reversed it runs tip -> seam with the ring, closing the loop.
    control.insert(control.end(), back.rbegin() + 1, back.rend());
    control.pop_back();
    return control;
}

std::vector<CurveFit::Point> OutlineFit::closedCubic(const std::vector<CurveFit::Point>& polygon, const std::vector<std::size_t>& breaks, double tolerance) {
    if (polygon.size() < 3) {
        return {};
    }
    std::vector<uint32_t> newIndex;
    std::vector<CurveFit::Point> ring = subdivided(polygon, tolerance, newIndex);
    ring.push_back(polygon.front());
    std::vector<uint32_t> cuts;
    for (const std::size_t i : breaks) {
        if (i < polygon.size()) {
            cuts.push_back(newIndex[i]);
        }
    }
    const std::vector<CurveFit::Anchor> anchors = CurveFit::fit(ring, tolerance, cuts);
    std::vector<CurveFit::Point> control = CurveFit::controlPoints(anchors);
    if (control.size() < 4 || (control.size() - 1) % 3 != 0) {
        return {};
    }
    control.pop_back();
    return control;
}

std::vector<std::vector<CurveFit::Point>> OutlineFit::outlines(const BrushBorders& borders, double tolerance) {
    std::vector<std::vector<CurveFit::Point>> out;
    if (borders.single) {
        std::vector<CurveFit::Point> ring = closedCubic(borders.startCap, {}, tolerance);
        if (!ring.empty()) {
            out.push_back(std::move(ring));
        }
        return out;
    }
    if (borders.closed) {
        std::vector<CurveFit::Point> right(borders.right.rbegin(), borders.right.rend());
        std::vector<std::size_t> rightBreaks;
        for (const std::size_t i : borders.rightBreaks) {
            rightBreaks.push_back(borders.right.size() - 1 - i);
        }
        std::vector<CurveFit::Point> a = closedCubic(borders.left, borders.leftBreaks, tolerance);
        std::vector<CurveFit::Point> b = closedCubic(right, rightBreaks, tolerance);
        if (!a.empty()) {
            out.push_back(std::move(a));
        }
        if (!b.empty()) {
            out.push_back(std::move(b));
        }
        return out;
    }
    std::vector<std::size_t> breaks = borders.leftBreaks;
    const std::size_t rightBase = borders.left.size() + borders.endCap.size();
    for (const std::size_t i : borders.rightBreaks) {
        breaks.push_back(rightBase + borders.right.size() - 1 - i);
    }
    std::vector<CurveFit::Point> ring = openStrokeCubic(borders, breaks, tolerance);
    if (ring.empty()) {
        ring = closedCubic(BrushEnvelope::polygon(borders), breaks, tolerance);
    }
    if (!ring.empty()) {
        out.push_back(std::move(ring));
    }
    return out;
}

}  // namespace jitnodes
