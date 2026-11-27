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

// The ring indices of the polygon points `round` names (ascending), with the points put between two neighbouring ones.
std::vector<uint32_t> ringIndices(const std::vector<std::size_t>& round, const std::vector<uint32_t>& newIndex, std::size_t ringSize) {
    std::vector<uint32_t> out;
    for (std::size_t k = 0; k < round.size(); ++k) {
        const std::size_t i = round[k];
        if (i + 1 >= newIndex.size()) {
            continue;
        }
        const bool run = k + 1 < round.size() && round[k + 1] == i + 1;
        const bool wraps = i + 2 == newIndex.size() && round.front() == 0;
        const std::size_t last = wraps ? ringSize - 1 : !run ? newIndex[i] : (i + 2 < newIndex.size() ? newIndex[i + 1] : ringSize) - 1;
        for (std::size_t at = newIndex[i]; at <= last; ++at) {
            out.push_back(static_cast<uint32_t>(at));
        }
    }
    return out;
}

// Where to open a loop for fitting: the middle of its longest stretch that does not turn (by 20 degrees or more over `reach` either side) and
// holds no break, so the join is not at a corner and the corners are all inside the line the fitter looks at. False when it turns everywhere.
bool flattestPlace(const std::vector<Point>& ring, const std::vector<uint32_t>& breaks, double reach, std::size_t& place) {
    const std::size_t n = ring.size();
    std::vector<char> turns(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t before = i, after = i;
        double run = 0.0;
        for (std::size_t k = 0; k < n / 2 && (k == 0 || run < reach); ++k) {
            const std::size_t next = (after + 1) % n;
            run += std::hypot(ring[next].x - ring[after].x, ring[next].y - ring[after].y);
            after = next;
        }
        run = 0.0;
        for (std::size_t k = 0; k < n / 2 && (k == 0 || run < reach); ++k) {
            const std::size_t previous = (before + n - 1) % n;
            run += std::hypot(ring[before].x - ring[previous].x, ring[before].y - ring[previous].y);
            before = previous;
        }
        const double inX = ring[i].x - ring[before].x, inY = ring[i].y - ring[before].y;
        const double outX = ring[after].x - ring[i].x, outY = ring[after].y - ring[i].y;
        turns[i] = std::abs(std::atan2(inX * outY - inY * outX, inX * outX + inY * outY)) >= 3.14159265358979 / 9.0;
    }
    for (const uint32_t i : breaks) {
        turns[i % n] = 1;
    }
    const auto first = std::find(turns.begin(), turns.end(), char(1));
    if (first == turns.end()) {
        place = 0;
        return true;
    }
    const std::size_t from = std::size_t(first - turns.begin());
    std::size_t longest = 0, run = 0;
    for (std::size_t k = 1; k <= n; ++k) {
        const std::size_t i = (from + k) % n;
        run = turns[i] ? 0 : run + 1;
        if (run > longest) {
            longest = run;
            place = (i + n - run / 2) % n;
        }
    }
    return longest > 0;
}

// One loop of a closed path's outline, fitted so that it meets itself as smoothly as it runs anywhere else.
std::vector<Point> loopCubic(const std::vector<Point>& polygon, const std::vector<std::size_t>& breaks, const std::vector<std::size_t>& round,
                             double tolerance) {
    if (polygon.size() < 3) {
        return {};
    }
    std::vector<uint32_t> newIndex;
    const std::vector<Point> ring = subdivided(polygon, tolerance, newIndex);
    std::vector<uint32_t> cuts;
    for (const std::size_t i : breaks) {
        if (i < polygon.size()) {
            cuts.push_back(newIndex[i]);
        }
    }
    std::size_t place = 0;
    const bool smooth = flattestPlace(ring, cuts, tolerance * 4.0, place);
    std::vector<Point> opened(ring.begin() + place, ring.end());
    opened.insert(opened.end(), ring.begin(), ring.begin() + place);
    opened.push_back(opened.front());
    for (uint32_t& cut : cuts) {
        cut = static_cast<uint32_t>((cut + ring.size() - place) % ring.size());
    }
    std::vector<uint32_t> arcs = ringIndices(round, newIndex, ring.size());
    for (uint32_t& at : arcs) {
        at = static_cast<uint32_t>((at + ring.size() - place) % ring.size());
    }
    std::sort(arcs.begin(), arcs.end());
    const Point direction = smooth ? ringDirection(ring, place, tolerance * 4.0) : Point{};
    const std::vector<CurveFit::Anchor> anchors = CurveFit::fit(opened, tolerance, cuts, CurveFit::Mode::global, smooth ? &direction : nullptr,
                                                                smooth ? &direction : nullptr, arcs);
    std::vector<Point> control = CurveFit::controlPoints(anchors);
    if (control.size() < 4 || (control.size() - 1) % 3 != 0) {
        return {};
    }
    control.pop_back();
    return control;
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
    // The caps and the outsides of sharp turns are arcs of the tip, in the order of the polygon.
    // A cap's arc begins and ends on the border points it joins.
    std::vector<std::size_t> round = borders.leftRound;
    const std::size_t rightAt = borders.left.size() + borders.endCap.size();
    for (std::size_t i = borders.left.size() - 1; i <= rightAt; ++i) {
        round.push_back(i);
    }
    for (auto it = borders.rightRound.rbegin(); it != borders.rightRound.rend(); ++it) {
        round.push_back(rightAt + borders.right.size() - 1 - *it);
    }
    for (std::size_t i = rightAt + borders.right.size() - 1; i < polygon.size(); ++i) {
        round.push_back(i);
    }
    round.push_back(0);
    std::sort(round.begin(), round.end());
    round.erase(std::unique(round.begin(), round.end()), round.end());
    std::vector<uint32_t> forwardRound, backwardRound;
    for (const uint32_t at : ringIndices(round, newIndex, n)) {
        const std::size_t f = (at + n - seamAt) % n, g = (seamAt + n - at) % n;
        if (f < forwardChain.size()) {
            forwardRound.push_back(static_cast<uint32_t>(f));
        }
        if (g < backwardChain.size()) {
            backwardRound.push_back(static_cast<uint32_t>(g));
        }
    }
    std::sort(forwardRound.begin(), forwardRound.end());
    std::sort(backwardRound.begin(), backwardRound.end());
    const Point backSeam{-seamDirection.x, -seamDirection.y}, backTip{-tipDirection.x, -tipDirection.y};
    const std::vector<CurveFit::Anchor> forwardAnchors =
        CurveFit::fit(forwardChain, tolerance, forwardBreaks, CurveFit::Mode::forward, &seamDirection, &tipDirection, forwardRound);
    const std::vector<CurveFit::Anchor> backwardAnchors =
        CurveFit::fit(backwardChain, tolerance, backwardBreaks, CurveFit::Mode::forward, &backSeam, &backTip, backwardRound);
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
        std::vector<std::size_t> rightRound;
        for (auto it = borders.rightRound.rbegin(); it != borders.rightRound.rend(); ++it) {
            rightRound.push_back(borders.right.size() - 1 - *it);
        }
        std::vector<CurveFit::Point> a = loopCubic(borders.left, borders.leftBreaks, borders.leftRound, tolerance);
        std::vector<CurveFit::Point> b = loopCubic(right, rightBreaks, rightRound, tolerance);
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
