#include "jitnodes/geo/curveFit.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace jitnodes {

namespace {
constexpr double kPi = 3.141592653589793;

struct Vec {
    double x = 0.0, y = 0.0;
};
Vec operator+(const Vec& a, const Vec& b) { return Vec{a.x + b.x, a.y + b.y}; }
Vec operator-(const Vec& a, const Vec& b) { return Vec{a.x - b.x, a.y - b.y}; }
Vec operator*(const Vec& a, double s) { return Vec{a.x * s, a.y * s}; }
double dot(const Vec& a, const Vec& b) { return a.x * b.x + a.y * b.y; }
double length(const Vec& a) { return std::sqrt(dot(a, a)); }
Vec unit(const Vec& a) {
    const double l = length(a);
    return l > 1e-12 ? a * (1.0 / l) : Vec{0.0, 0.0};
}

using Bezier = std::array<Vec, 4>;

Vec bezierAt(const Bezier& b, double t) {
    const double s = 1.0 - t;
    return b[0] * (s * s * s) + b[1] * (3.0 * s * s * t) + b[2] * (3.0 * s * t * t) + b[3] * (t * t * t);
}
Vec bezierDerivative(const Bezier& b, double t) {
    const double s = 1.0 - t;
    return (b[1] - b[0]) * (3.0 * s * s) + (b[2] - b[1]) * (6.0 * s * t) + (b[3] - b[2]) * (3.0 * t * t);
}
Vec bezierSecond(const Bezier& b, double t) {
    return (b[2] - b[1] * 2.0 + b[0]) * (6.0 * (1.0 - t)) + (b[3] - b[2] * 2.0 + b[1]) * (6.0 * t);
}

std::vector<double> chordLength(const std::vector<Vec>& d, std::size_t first, std::size_t last) {
    std::vector<double> u(last - first + 1, 0.0);
    for (std::size_t i = first + 1; i <= last; ++i) {
        u[i - first] = u[i - first - 1] + length(d[i] - d[i - 1]);
    }
    const double total = u.back();
    for (double& value : u) {
        value = total > 0.0 ? value / total : 0.0;
    }
    return u;
}

// The cubic from d[first] to d[last] with the given end tangents whose handle lengths best fit the samples (least squares).
Bezier generateBezier(const std::vector<Vec>& d, std::size_t first, std::size_t last, const std::vector<double>& u, const Vec& tangent1,
                      const Vec& tangent2) {
    const std::size_t count = last - first + 1;
    double c00 = 0.0, c01 = 0.0, c11 = 0.0, x0 = 0.0, x1 = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double t = u[i], s = 1.0 - t;
        const double b0 = s * s * s, b1 = 3.0 * t * s * s, b2 = 3.0 * t * t * s, b3 = t * t * t;
        const Vec a0 = tangent1 * b1, a1 = tangent2 * b2;
        c00 += dot(a0, a0);
        c01 += dot(a0, a1);
        c11 += dot(a1, a1);
        const Vec tmp = d[first + i] - (d[first] * (b0 + b1) + d[last] * (b2 + b3));
        x0 += dot(a0, tmp);
        x1 += dot(a1, tmp);
    }
    const double det = c00 * c11 - c01 * c01;
    const double alphaLeft = det == 0.0 ? 0.0 : (x0 * c11 - x1 * c01) / det;
    const double alphaRight = det == 0.0 ? 0.0 : (c00 * x1 - c01 * x0) / det;
    const double segment = length(d[last] - d[first]);
    const double epsilon = 1e-6 * segment;
    Bezier out;
    out[0] = d[first];
    out[3] = d[last];
    // A handle longer than its chord swings the curve out past its end and back (a small hook at a corner): treat that, like a negative
    // length, as a failed fit and use the usual third of the chord.
    if (alphaLeft < epsilon || alphaRight < epsilon || alphaLeft > segment || alphaRight > segment) {
        out[1] = d[first] + tangent1 * (segment / 3.0);
        out[2] = d[last] + tangent2 * (segment / 3.0);
    } else {
        out[1] = d[first] + tangent1 * alphaLeft;
        out[2] = d[last] + tangent2 * alphaRight;
    }
    return out;
}

// The largest squared distance from the curve to the samples: at each sample, and at the middle of each segment between two samples (the squared
// distance from the curve there to the segment), so a curve cannot bow out between sparse samples unseen. Also which sample to split at.
double maxError(const std::vector<Vec>& d, std::size_t first, std::size_t last, const Bezier& b, const std::vector<double>& u, std::size_t& split) {
    double worst = 0.0;
    split = (last - first + 1) / 2 + first;
    for (std::size_t i = first + 1; i < last; ++i) {
        const Vec diff = bezierAt(b, u[i - first]) - d[i];
        const double distance = dot(diff, diff);
        if (distance >= worst) {
            worst = distance;
            split = i;
        }
    }
    for (std::size_t i = first; i < last; ++i) {
        const Vec p = bezierAt(b, 0.5 * (u[i - first] + u[i + 1 - first]));
        const Vec ab = d[i + 1] - d[i];
        const double abAb = dot(ab, ab);
        const double t = abAb > 0.0 ? std::max(0.0, std::min(1.0, dot(p - d[i], ab) / abAb)) : 0.0;
        const Vec diff = p - (d[i] + ab * t);
        const double distance = dot(diff, diff);
        if (distance > worst) {
            worst = distance;
            split = i > first ? i : i + 1;
        }
    }
    return worst;
}

// One Newton-Raphson step moving each parameter toward the closest point of the curve.
std::vector<double> reparameterize(const std::vector<Vec>& d, std::size_t first, const std::vector<double>& u, const Bezier& b) {
    std::vector<double> better(u.size());
    for (std::size_t i = 0; i < u.size(); ++i) {
        const Vec diff = bezierAt(b, u[i]) - d[first + i];
        const Vec d1 = bezierDerivative(b, u[i]), d2 = bezierSecond(b, u[i]);
        const double numerator = dot(diff, d1);
        const double denominator = dot(d1, d1) + dot(diff, d2);
        better[i] = denominator == 0.0 ? u[i] : std::max(0.0, std::min(1.0, u[i] - numerator / denominator));
    }
    return better;
}

// The direction in which the polyline leaves d[from] toward d[toward]: the principal axis of the samples within a few tolerances of the end (a
// least-squares line), pointing along the stroke.
Vec endTangent(const std::vector<Vec>& d, std::size_t from, std::size_t toward, double reach) {
    const bool forward = toward > from;
    const std::size_t span = forward ? toward - from : from - toward;
    const std::size_t cap = std::max<std::size_t>(2, span / 3);
    std::vector<Vec> near;
    near.push_back(d[from]);
    for (std::size_t k = 1; k <= cap; ++k) {
        const Vec p = forward ? d[from + k] : d[from - k];
        near.push_back(p);
        if (length(p - d[from]) > reach && near.size() >= 2) {
            break;
        }
    }
    Vec mean{0.0, 0.0};
    for (const Vec& p : near) {
        mean = mean + p;
    }
    mean = mean * (1.0 / double(near.size()));
    double sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (const Vec& p : near) {
        const Vec q = p - mean;
        sxx += q.x * q.x;
        sxy += q.x * q.y;
        syy += q.y * q.y;
    }
    const double angle = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
    const Vec axis{std::cos(angle), std::sin(angle)};
    const Vec along = near.back() - d[from];
    return dot(axis, along) >= 0.0 ? axis : axis * -1.0;
}

// Of the samples within `reach` of d[around], the one farthest from the straight line between the samples `reach` before and after: the tip of
// the turn, even when smoothing has rounded the corner a little.
std::size_t apexNear(const std::vector<Vec>& d, std::size_t around, std::size_t reach) {
    const std::size_t from = around > reach ? around - reach : 0;
    const std::size_t to = std::min(d.size() - 1, around + reach);
    const Vec a = d[from], b = d[to];
    const Vec ab = b - a;
    const double abLength = length(ab);
    std::size_t best = around;
    double bestDistance = -1.0;
    for (std::size_t i = from; i <= to; ++i) {
        const Vec ap = d[i] - a;
        const double distance = abLength > 1e-12 ? std::fabs(ap.x * ab.y - ap.y * ab.x) / abLength : length(ap);
        if (distance > bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

void fitCubic(const std::vector<Vec>& d, std::size_t first, std::size_t last, const Vec& tangent1, const Vec& tangent2, double errorSquared,
              std::vector<Bezier>& out, std::vector<std::size_t>& ends, int depth) {
    if (last - first == 1) {
        const double dist = length(d[last] - d[first]) / 3.0;
        out.push_back({d[first], d[first] + tangent1 * dist, d[last] + tangent2 * dist, d[last]});
        ends.push_back(last);
        return;
    }
    std::vector<double> u = chordLength(d, first, last);
    Bezier b = generateBezier(d, first, last, u, tangent1, tangent2);
    std::size_t split = 0;
    double error = maxError(d, first, last, b, u, split);
    if (error < errorSquared) {
        out.push_back(b);
        ends.push_back(last);
        return;
    }
    if (error < errorSquared * 16.0) {   // close: a few refinements of the parameters may be enough
        for (int pass = 0; pass < 4; ++pass) {
            u = reparameterize(d, first, u, b);
            b = generateBezier(d, first, last, u, tangent1, tangent2);
            error = maxError(d, first, last, b, u, split);
            if (error < errorSquared) {
                out.push_back(b);
                ends.push_back(last);
                return;
            }
        }
    }
    if (depth > 40 || split <= first || split >= last) {
        out.push_back(b);
        ends.push_back(last);
        return;
    }
    // Split where the fit is worst, with one smooth tangent at the join.
    const Vec center = unit(d[split - 1] - d[split + 1]);
    fitCubic(d, first, split, tangent1, center, errorSquared, out, ends, depth + 1);
    fitCubic(d, split, last, center * -1.0, tangent2, errorSquared, out, ends, depth + 1);
}

// The direction of the line at d[around]: the principal axis of the samples within `reach` of path distance on either side (kept to [lo, hi], so
// at an end of the part it is one sided), pointing along the stroke.
Vec windowTangent(const std::vector<Vec>& d, std::size_t lo, std::size_t hi, std::size_t around, double reach) {
    std::size_t a = around, b = around;
    double run = 0.0;
    while (a > lo && run <= reach) {
        run += length(d[a] - d[a - 1]);
        --a;
    }
    run = 0.0;
    while (b < hi && run <= reach) {
        run += length(d[b + 1] - d[b]);
        ++b;
    }
    Vec mean{0.0, 0.0};
    for (std::size_t i = a; i <= b; ++i) {
        mean = mean + d[i];
    }
    mean = mean * (1.0 / double(b - a + 1));
    double sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (std::size_t i = a; i <= b; ++i) {
        const Vec q = d[i] - mean;
        sxx += q.x * q.x;
        sxy += q.x * q.y;
        syy += q.y * q.y;
    }
    const double angle = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
    const Vec axis{std::cos(angle), std::sin(angle)};
    const Vec along = d[b] - d[a];
    return dot(axis, along) >= 0.0 ? axis : axis * -1.0;
}

// Fits d[first..last] from the front: the longest run from the current start that one cubic fits within the tolerance (found by doubling the run
// and then bisecting), kept as it is, then the next from its end. Tangents at the joins come from windowTangent, so a kept cubic does not depend on
// anything further along than the window.
void fitForward(const std::vector<Vec>& d, std::size_t first, std::size_t last, double reach, double errorSquared, const Vec* startAlong,
                const Vec* endAlong, std::vector<Bezier>& out, std::vector<std::size_t>& ends) {
    std::size_t start = first;
    Vec tangent1 = startAlong != nullptr ? *startAlong : windowTangent(d, first, last, first, reach);
    auto alongAt = [&](std::size_t at) { return at == last && endAlong != nullptr ? *endAlong : windowTangent(d, first, last, at, reach); };
    while (start < last) {
        Bezier best;
        Vec bestAlong;
        std::size_t bestEnd = start + 1;
        {
            bestAlong = alongAt(bestEnd);
            const double dist = length(d[bestEnd] - d[start]) / 3.0;
            best = {d[start], d[start] + tangent1 * dist, d[bestEnd] + bestAlong * (-dist), d[bestEnd]};
        }
        auto attempt = [&](std::size_t end) {
            const Vec along = alongAt(end);
            const Vec tangent2 = along * -1.0;
            std::vector<double> u = chordLength(d, start, end);
            Bezier b = generateBezier(d, start, end, u, tangent1, tangent2);
            std::size_t split = 0;
            double error = maxError(d, start, end, b, u, split);
            for (int pass = 0; pass < 4 && error >= errorSquared && error < errorSquared * 16.0; ++pass) {
                u = reparameterize(d, start, u, b);
                b = generateBezier(d, start, end, u, tangent1, tangent2);
                error = maxError(d, start, end, b, u, split);
            }
            if (error >= errorSquared) {
                return false;
            }
            best = b;
            bestAlong = along;
            bestEnd = end;
            return true;
        };
        std::size_t okEnd = start + 1, failEnd = 0;
        std::size_t probe = start + 2;
        while (probe <= last) {
            if (!attempt(probe)) {
                failEnd = probe;
                break;
            }
            okEnd = probe;
            if (probe == last) {
                break;
            }
            probe = std::min(last, start + (probe - start) * 2);
        }
        while (failEnd != 0 && failEnd - okEnd > 1) {
            const std::size_t mid = (okEnd + failEnd) / 2;
            if (attempt(mid)) {
                okEnd = mid;
            } else {
                failEnd = mid;
            }
        }
        out.push_back(best);
        ends.push_back(bestEnd);
        tangent1 = bestAlong;
        start = bestEnd;
    }
}

CurveFit::Anchor anchorAt(const Vec& v, std::size_t index) {
    CurveFit::Anchor a;
    a.point = {v.x, v.y};
    a.index = static_cast<uint32_t>(index);
    return a;
}
}  // namespace

std::vector<CurveFit::Anchor> CurveFit::fit(const std::vector<Point>& points, double tolerance, const std::vector<uint32_t>& breakpoints, Mode mode,
                                            const Point* startAlong, const Point* endAlong, const std::vector<uint32_t>& rounded) {
    std::vector<Vec> d;
    std::vector<std::size_t> original;   // the input index of each kept sample
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Vec v{points[i].x, points[i].y};
        if (d.empty() || length(v - d.back()) > 1e-9) {
            d.push_back(v);
            original.push_back(i);
        }
    }
    std::vector<Anchor> anchors;
    if (d.size() < 3) {
        for (std::size_t i = 0; i < d.size(); ++i) {
            anchors.push_back(anchorAt(d[i], original[i]));
        }
        return anchors;
    }

    // Sharp corners: where the direction coming in and the direction going out (a few samples either side) differ by 60 degrees or more, and
    // nowhere nearer a sharper one, the line is cut and each part fitted on its own.
    const std::size_t span = 4;
    std::vector<std::size_t> cuts;
    cuts.push_back(0);
    const auto onArc = [&](std::size_t at) { return std::binary_search(rounded.begin(), rounded.end(), static_cast<uint32_t>(original[at])); };
    if (d.size() > 2 * span + 1) {
        double bestTurn = 0.0;
        std::size_t bestAt = 0;
        for (std::size_t i = span; i + span < d.size(); ++i) {
            const Vec in = unit(d[i] - d[i - span]), out = unit(d[i + span] - d[i]);
            const double turn = std::acos(std::max(-1.0, std::min(1.0, dot(in, out))));
            const bool corner = turn >= kPi / 3.0;
            if (corner && turn > bestTurn) {
                bestTurn = turn;
                bestAt = i;
            }
            if (!corner && bestAt != 0) {   // the run of corner-like samples ended: keep its apex
                const std::size_t apex = apexNear(d, bestAt, span);
                if (apex > cuts.back() + span && !onArc(apex)) {
                    cuts.push_back(apex);
                }
                bestTurn = 0.0;
                bestAt = 0;
            }
        }
        if (bestAt != 0 && bestAt + span < d.size()) {
            const std::size_t apex = apexNear(d, bestAt, span);
            if (apex > cuts.back() + span && !onArc(apex)) {
                cuts.push_back(apex);
            }
        }
    }
    cuts.push_back(d.size() - 1);

    // Where a straight run meets an arc, the line is cut and both sides leave the join along the straight, so that no one curve has to follow
    // both. A short or ragged neighbour (a hand drawn stroke) gives no direction to trust, and neither does one the arc does not carry on from.
    std::vector<Vec> joinAlong(d.size());
    const auto straightBeyond = [&](std::size_t at, bool forward, const Vec& along) {
        double run = 0.0;
        for (std::size_t j = at; run < 4.0 * tolerance;) {
            if (forward ? j + 1 >= d.size() : j == 0) {
                return false;
            }
            const std::size_t next = forward ? j + 1 : j - 1;
            const Vec off = d[next] - d[at];
            if (std::fabs(off.x * along.y - off.y * along.x) > 0.1 * tolerance) {
                return false;
            }
            run += length(d[next] - d[j]);
            j = next;
        }
        return true;
    };
    // The cap of a hair thin stroke end is an arc too, but pinning the borders there lets them cross and the tip drops out.
    const auto longArc = [&](std::size_t at, bool forward) {
        double run = 0.0;
        for (std::size_t j = at; run < 4.0 * tolerance;) {
            if (forward ? j + 1 >= d.size() : j == 0) {
                return false;
            }
            const std::size_t next = forward ? j + 1 : j - 1;
            if (!onArc(next)) {
                return false;
            }
            run += length(d[next] - d[j]);
            j = next;
        }
        return true;
    };
    const auto joinAt =[&](std::size_t at, bool straightAfter, const Vec& straight, const Vec& arc) {
        if (dot(straight, arc) >= std::cos(kPi / 6.0) && straightBeyond(at, straightAfter, straight) && longArc(at, !straightAfter)) {
            joinAlong[at] = straight;
            cuts.push_back(at);
        }
    };
    for (std::size_t i = 1; i + 1 < d.size(); ++i) {
        if (!onArc(i)) {
            continue;
        }
        const Vec in = unit(d[i] - d[i - 1]), out = unit(d[i + 1] - d[i]);
        if (!onArc(i - 1) && onArc(i + 1)) {
            joinAt(i, false, in, out);
        } else if (onArc(i - 1) && !onArc(i + 1)) {
            joinAt(i, true, out, in);
        }
    }

    // Forced breakpoints, moved onto the nearest kept sample at or after the input index (a dropped repeat is the sample before it).
    for (const uint32_t wanted : breakpoints) {
        const auto it = std::lower_bound(original.begin(), original.end(), std::size_t(wanted));
        if (it == original.end()) {
            continue;
        }
        const std::size_t at = std::size_t(it - original.begin());
        if (at > 0 && at + 1 < d.size()) {
            cuts.push_back(at);
            joinAlong[at] = Vec{};
        }
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    anchors.push_back(anchorAt(d[cuts[0]], original[cuts[0]]));
    for (std::size_t c = 0; c + 1 < cuts.size(); ++c) {
        const std::size_t first = cuts[c], last = cuts[c + 1];
        // The eye reads any unevenness of a short round arc, and fitting it closely costs a curve at most: half the tolerance.
        bool arcOnly = true;
        for (std::size_t i = first; i <= last && arcOnly; ++i) {
            arcOnly = onArc(i);
        }
        const double errorSquared = arcOnly ? 0.25 * tolerance * tolerance : tolerance * tolerance;
        // End tangents come from a line fitted to the samples within `reach` of each end, so the jitter of a hand does not decide which way the
        // curve leaves its end. A short reach follows a tight curve, a long one ignores noise on a flat run (a short reach on a long flat run
        // leaves a handle off by a fraction of a degree, which is several units at its far end), so the reaches are tried and the one that
        // needs the fewest curves wins.
        double partLength = 0.0;
        for (std::size_t i = first + 1; i <= last; ++i) {
            partLength += length(d[i] - d[i - 1]);
        }
        std::vector<Bezier> curves;
        std::vector<std::size_t> ends;
        // The direction of travel fixed at an end of this part, by the caller or by an arc join; zero where it is read from the samples.
        const Vec startDirection = c == 0 && startAlong != nullptr ? unit(Vec{startAlong->x, startAlong->y}) : joinAlong[first];
        const Vec endDirection = c + 2 == cuts.size() && endAlong != nullptr ? unit(Vec{endAlong->x, endAlong->y}) : joinAlong[last];
        const bool startFixed = dot(startDirection, startDirection) > 0.0, endFixed = dot(endDirection, endDirection) > 0.0;
        if (mode == Mode::forward) {
            fitForward(d, first, last, 6.0 * tolerance, errorSquared, startFixed ? &startDirection : nullptr, endFixed ? &endDirection : nullptr,
                       curves, ends);
        }
        for (const double reach : mode == Mode::forward ? std::vector<double>{} : std::vector<double>{6.0 * tolerance, 0.1 * partLength, 0.3 * partLength}) {
            const Vec tangent1 = startFixed ? startDirection : endTangent(d, first, last, reach);
            const Vec tangent2 = endFixed ? endDirection * -1.0 : endTangent(d, last, first, reach);
            std::vector<Bezier> tryCurves;
            std::vector<std::size_t> tryEnds;
            fitCubic(d, first, last, tangent1, tangent2, errorSquared, tryCurves, tryEnds, 0);
            if (curves.empty() || tryCurves.size() < curves.size()) {
                curves = std::move(tryCurves);
                ends = std::move(tryEnds);
            }
            if (curves.size() == 1) {
                break;
            }
        }
        for (std::size_t k = 0; k < curves.size(); ++k) {
            const Bezier& b = curves[k];
            anchors.back().out = {b[1].x, b[1].y};
            anchors.back().hasOut = true;
            Anchor next = anchorAt(b[3], original[ends[k]]);
            next.in = {b[2].x, b[2].y};
            next.hasIn = true;
            anchors.push_back(next);
        }
    }
    return anchors;
}

std::vector<CurveFit::Point> CurveFit::smooth(const std::vector<Point>& points, int passes) {
    std::vector<Point> current = points;
    for (int pass = 0; pass < passes && current.size() > 2; ++pass) {
        std::vector<Point> next = current;
        for (std::size_t i = 1; i + 1 < current.size(); ++i) {
            next[i] = {(current[i - 1].x + 2.0 * current[i].x + current[i + 1].x) / 4.0,
                       (current[i - 1].y + 2.0 * current[i].y + current[i + 1].y) / 4.0};
        }
        current = next;
    }
    return current;
}

std::vector<CurveFit::Point> CurveFit::controlPoints(const std::vector<Anchor>& anchors) {
    std::vector<Point> points;
    if (anchors.empty()) {
        return points;
    }
    points.push_back(anchors[0].point);
    for (std::size_t i = 1; i < anchors.size(); ++i) {
        const Anchor& from = anchors[i - 1];
        const Anchor& to = anchors[i];
        points.push_back(from.hasOut ? from.out : from.point);
        points.push_back(to.hasIn ? to.in : to.point);
        points.push_back(to.point);
    }
    return points;
}

}  // namespace jitnodes
