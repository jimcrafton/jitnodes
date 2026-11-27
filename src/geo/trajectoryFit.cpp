#include "jitnodes/geo/trajectoryFit.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace jitnodes {

namespace {
using Point = CurveFit::Point;

double distance(const Point& a, const Point& b) { return std::hypot(b.x - a.x, b.y - a.y); }

// The direction through the first point of a loop (its last point repeats the first), read from the points `reach` of path length either side.
// False when there is none: too few points, or the loop turns a corner (60 degrees or more) there.
bool loopDirection(const std::vector<Point>& points, double reach, Point& direction) {
    if (points.size() < 4 || distance(points.front(), points.back()) > 1e-6) {
        return false;
    }
    const std::size_t n = points.size() - 1;
    const auto walk = [&](std::size_t step) {
        std::size_t at = 0;
        double run = 0.0;
        for (std::size_t k = 0; k < n / 2 && (k == 0 || run < reach); ++k) {
            const std::size_t next = (at + step) % n;
            run += distance(points[at], points[next]);
            at = next;
        }
        return points[at];
    };
    const Point before = walk(n - 1), after = walk(1), here = points.front();
    const double inX = here.x - before.x, inY = here.y - before.y, outX = after.x - here.x, outY = after.y - here.y;
    const double turn = std::abs(std::atan2(inX * outY - inY * outX, inX * outX + inY * outY));
    const double length = distance(before, after);
    if (turn >= 3.14159265358979 / 3.0 || length < 1e-9) {
        return false;
    }
    direction = {(after.x - before.x) / length, (after.y - before.y) / length};
    return true;
}


// Appends the vertices after the first of the cubic p0..p3, as many as keep the polygon within `flatness` of it.
void flatten(const Point& p0, const Point& p1, const Point& p2, const Point& p3, double flatness, std::vector<Point>& out) {
    const double ddx = std::max(std::abs(p0.x - 2.0 * p1.x + p2.x), std::abs(p1.x - 2.0 * p2.x + p3.x));
    const double ddy = std::max(std::abs(p0.y - 2.0 * p1.y + p2.y), std::abs(p1.y - 2.0 * p2.y + p3.y));
    const double dd = std::hypot(ddx, ddy);
    const int steps = std::clamp(int(std::ceil(std::sqrt(0.75 * dd / std::max(flatness, 1e-6)))), 1, 256);
    for (int k = 1; k <= steps; ++k) {
        const double t = double(k) / steps, u = 1.0 - t;
        const double b0 = u * u * u, b1 = 3.0 * u * u * t, b2 = 3.0 * u * t * t, b3 = t * t * t;
        out.push_back({b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x, b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y});
    }
}
// The input points strictly between `first` and `last` that a line through the attribute values must pass through to stay within `tolerance` of
// all of them (Ramer-Douglas-Peucker over arc length), in order. The ends of the line are at arc lengths s0 and s1 with the values v0 and v1.
std::vector<std::size_t> bends(const std::vector<std::vector<double>>& attributes, const std::vector<double>& chord, std::size_t first, std::size_t last,
                               double s0, double s1, const std::vector<double>& v0, const std::vector<double>& v1, double tolerance) {
    constexpr std::size_t kEnd = static_cast<std::size_t>(-1);
    struct Span {
        std::size_t first, last, from, to;   // the points to look at, and the points the line runs between (kEnd: the segment's own end)
    };
    std::vector<std::size_t> kept;
    std::vector<Span> open{{first, last, kEnd, kEnd}};
    while (!open.empty()) {
        const Span span = open.back();
        open.pop_back();
        const double a = span.from == kEnd ? s0 : chord[span.from], b = span.to == kEnd ? s1 : chord[span.to];
        std::size_t worst = kEnd;
        double most = tolerance;
        for (std::size_t j = span.first; j < span.last; ++j) {
            const double t = b - a > 1e-12 ? std::clamp((chord[j] - a) / (b - a), 0.0, 1.0) : 0.0;
            for (std::size_t k = 0; k < attributes.size(); ++k) {
                const double from = span.from == kEnd ? v0[k] : attributes[k][span.from];
                const double to = span.to == kEnd ? v1[k] : attributes[k][span.to];
                const double off = std::abs(attributes[k][j] - (from + (to - from) * t));
                if (off > most) {
                    most = off;
                    worst = j;
                }
            }
        }
        if (worst == kEnd) {
            continue;
        }
        kept.push_back(worst);
        open.push_back({span.first, worst, span.from, worst});
        open.push_back({worst + 1, span.last, worst, span.to});
    }
    std::sort(kept.begin(), kept.end());
    return kept;
}

// The value at normalised arc length `s`; `at` is where the search starts and ends (the calls go in increasing `s`).
double valueAt(const std::vector<double>& values, const std::vector<double>& chord, double s, std::size_t& at) {
    while (at + 1 < chord.size() && chord[at + 1] < s) {
        ++at;
    }
    if (at + 1 >= chord.size()) {
        return values.back();
    }
    const double span = chord[at + 1] - chord[at];
    const double t = span > 1e-12 ? std::clamp((s - chord[at]) / span, 0.0, 1.0) : 1.0;
    return values[at] + (values[at + 1] - values[at]) * t;
}

// Puts a vertex wherever an attribute bends between two vertices of the polygon.
void followAttributes(Trajectory& out, const std::vector<std::vector<double>>& attributes, const std::vector<double>& chord, double tolerance) {
    std::vector<Point> vertices{out.vertices.front()};
    std::vector<double> arc{out.arc.front()};
    std::vector<std::size_t> moved(out.vertices.size(), 0);
    std::vector<std::size_t> at(attributes.size(), 0);
    std::vector<double> v0(attributes.size()), v1(attributes.size());
    std::size_t first = 0;
    for (std::size_t k = 0; k < attributes.size(); ++k) {
        v0[k] = valueAt(attributes[k], chord, out.arc.front(), at[k]);
    }
    for (std::size_t i = 1; i < out.vertices.size(); ++i) {
        const double s0 = out.arc[i - 1], s1 = out.arc[i];
        for (std::size_t k = 0; k < attributes.size(); ++k) {
            v1[k] = valueAt(attributes[k], chord, s1, at[k]);
        }
        while (first < chord.size() && chord[first] <= s0) {
            ++first;
        }
        std::size_t last = first;
        while (last < chord.size() && chord[last] < s1) {
            ++last;
        }
        for (const std::size_t j : bends(attributes, chord, first, last, s0, s1, v0, v1, tolerance)) {
            if (chord[j] - arc.back() <= 1e-9 || s1 - chord[j] <= 1e-9) {
                continue;
            }
            const double t = (chord[j] - s0) / (s1 - s0);
            const Point& a = out.vertices[i - 1];
            const Point& b = out.vertices[i];
            vertices.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
            arc.push_back(chord[j]);
        }
        moved[i] = vertices.size();
        vertices.push_back(out.vertices[i]);
        arc.push_back(s1);
        v0 = v1;
    }
    for (std::size_t& vertex : out.anchorVertex) {
        vertex = moved[vertex];
    }
    out.vertices = std::move(vertices);
    out.arc = std::move(arc);
}
}  // namespace

Trajectory TrajectoryFit::fit(const std::vector<CurveFit::Point>& points, const std::vector<std::vector<double>>& attributes, const TrajectoryParams& params,
                              const std::vector<unsigned>& breakpoints) {
    for (const std::vector<double>& list : attributes) {
        if (list.size() != points.size()) {
            throw std::runtime_error("TrajectoryFit: an attribute needs one value per point");
        }
    }
    Trajectory out;
    if (points.empty()) {
        return out;
    }
    Point seam;
    const bool smoothSeam = params.closed && loopDirection(points, 6.0 * params.tolerance, seam);
    out.anchors = CurveFit::fit(points, params.tolerance, std::vector<uint32_t>(breakpoints.begin(), breakpoints.end()),
                                params.forward ? CurveFit::Mode::forward : CurveFit::Mode::global, smoothSeam ? &seam : nullptr,
                                smoothSeam ? &seam : nullptr);
    const std::vector<Point> control = CurveFit::controlPoints(out.anchors);
    out.vertices.push_back(control.front());
    out.anchorVertex.push_back(0);
    for (std::size_t seg = 0; seg + 3 < control.size(); seg += 3) {
        flatten(control[seg], control[seg + 1], control[seg + 2], control[seg + 3], params.flatness, out.vertices);
        out.anchorVertex.push_back(out.vertices.size() - 1);
    }

    out.arc.assign(out.vertices.size(), 0.0);
    for (std::size_t i = 1; i < out.vertices.size(); ++i) {
        out.arc[i] = out.arc[i - 1] + distance(out.vertices[i - 1], out.vertices[i]);
    }
    out.length = out.arc.back();
    if (out.length > 0.0) {
        for (double& s : out.arc) {
            s /= out.length;
        }
    }

    // Normalised chord length of every input point (repeated points share their value).
    std::vector<double> chord(points.size(), 0.0);
    for (std::size_t i = 1; i < points.size(); ++i) {
        chord[i] = chord[i - 1] + distance(points[i - 1], points[i]);
    }
    const double chordTotal = chord.back();
    for (double& c : chord) {
        c = chordTotal > 0.0 ? c / chordTotal : 0.0;
    }

    if (!attributes.empty() && out.length > 0.0) {
        followAttributes(out, attributes, chord, params.attributeTolerance);
    }

    out.attributes.assign(attributes.size(), std::vector<double>(out.vertices.size(), 0.0));
    for (std::size_t a = 0; a < attributes.size(); ++a) {
        std::size_t at = 0;
        for (std::size_t v = 0; v < out.vertices.size(); ++v) {
            out.attributes[a][v] = valueAt(attributes[a], chord, out.arc[v], at);
        }
    }
    return out;
}

}  // namespace jitnodes
