#include "jitnodes/geo/trajectoryFit.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace jitnodes {

namespace {
using Point = CurveFit::Point;

double distance(const Point& a, const Point& b) { return std::hypot(b.x - a.x, b.y - a.y); }

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
    out.anchors = CurveFit::fit(points, params.tolerance, std::vector<uint32_t>(breakpoints.begin(), breakpoints.end()),
                                params.forward ? CurveFit::Mode::forward : CurveFit::Mode::global);
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

    out.attributes.assign(attributes.size(), std::vector<double>(out.vertices.size(), 0.0));
    for (std::size_t a = 0; a < attributes.size(); ++a) {
        const std::vector<double>& values = attributes[a];
        std::size_t at = 0;   // the vertices go in increasing arc length, so the input position only moves forward
        for (std::size_t v = 0; v < out.vertices.size(); ++v) {
            const double s = out.arc[v];
            while (at + 1 < points.size() && chord[at + 1] < s) {
                ++at;
            }
            if (at + 1 >= points.size()) {
                out.attributes[a][v] = values.back();
                continue;
            }
            const double span = chord[at + 1] - chord[at];
            const double t = span > 1e-12 ? std::clamp((s - chord[at]) / span, 0.0, 1.0) : 1.0;
            out.attributes[a][v] = values[at] + (values[at + 1] - values[at]) * t;
        }
    }
    return out;
}

}  // namespace jitnodes
