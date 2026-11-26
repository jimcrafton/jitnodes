#include "jitnodes/geo/brushEnvelope.h"

#include <algorithm>
#include <cmath>

namespace jitnodes {

namespace {
using Point = CurveFit::Point;

double cross(double ax, double ay, double bx, double by) { return ax * by - ay * bx; }

class Sweeper {
public:
    Sweeper(const NibBrush& nib, std::vector<Point> vertices, std::vector<double> scale, bool closed)
        : nib_(nib), v_(std::move(vertices)), s_(std::move(scale)), closed_(closed) {
        n_ = v_.size();
        segments_ = closed_ ? n_ : n_ - 1;
        for (std::size_t i = 0; i < segments_; ++i) {
            const Point& a = v_[i];
            const Point& b = v_[(i + 1) % n_];
            const double len = std::hypot(b.x - a.x, b.y - a.y);
            tx_.push_back((b.x - a.x) / len);
            ty_.push_back((b.y - a.y) / len);
            supportLeft_.push_back(nib_.support(-ty_.back(), tx_.back()));
            supportRight_.push_back(nib_.support(ty_.back(), -tx_.back()));
        }
    }

    BrushBorders run() {
        BrushBorders out;
        out.closed = closed_;
        border(true, out.left, out.leftBreaks);
        border(false, out.right, out.rightBreaks);
        if (!closed_) {
            const std::size_t k = nib_.size();
            const std::size_t startFrom = supportRight_.front(), startTo = supportLeft_.front();
            for (std::size_t m = 1, steps = (startFrom + k - startTo) % k; m < steps; ++m) {
                out.startCap.push_back(corner(0, (startFrom + k - m) % k));
            }
            const std::size_t endFrom = supportLeft_.back(), endTo = supportRight_.back();
            for (std::size_t m = 1, steps = (endFrom + k - endTo) % k; m < steps; ++m) {
                out.endCap.push_back(corner(n_ - 1, (endFrom + k - m) % k));
            }
        }
        return out;
    }

private:
    Point corner(std::size_t vertex, std::size_t index) const {
        return {v_[vertex].x + s_[vertex] * nib_.corners[index].x, v_[vertex].y + s_[vertex] * nib_.corners[index].y};
    }

    std::size_t support(bool left, std::size_t segment) const { return left ? supportLeft_[segment] : supportRight_[segment]; }

    void border(bool left, std::vector<Point>& pts, std::vector<std::size_t>& breaks) const {
        const std::size_t k = nib_.size();
        const double sigma = left ? 1.0 : -1.0;
        for (std::size_t i = 0; i < n_; ++i) {
            const bool hasPrev = closed_ || i > 0;
            const bool hasNext = closed_ || i + 1 < n_;
            const std::size_t prev = (i + n_ - 1) % n_, next = i;
            if (!hasPrev) {
                pts.push_back(corner(i, support(left, next)));
                continue;
            }
            if (!hasNext) {
                pts.push_back(corner(i, support(left, prev)));
                continue;
            }
            const std::size_t jp = support(left, prev), jn = support(left, next);
            if (jp == jn) {
                pts.push_back(corner(i, jp));
                continue;
            }
            std::ptrdiff_t d = std::ptrdiff_t((jn + k - jp) % k);
            if (d > std::ptrdiff_t(k / 2)) {
                d -= std::ptrdiff_t(k);
            }
            const double turn = cross(tx_[prev], ty_[prev], tx_[next], ty_[next]);
            if (sigma * turn <= 0.0) {   // the outside of the turn: round the tip's corners
                const std::ptrdiff_t step = d > 0 ? 1 : -1;
                for (std::ptrdiff_t m = 0; m <= std::abs(d); ++m) {
                    pts.push_back(corner(i, std::size_t((std::ptrdiff_t(jp) + step * m + std::ptrdiff_t(k) * 2) % std::ptrdiff_t(k))));
                }
                continue;
            }
            const Point a0 = corner(prev, jp), a1 = corner(i, jp);
            const Point b0 = corner(i, jn), b1 = corner((i + 1) % n_, jn);
            const double rx = a1.x - a0.x, ry = a1.y - a0.y, qx = b1.x - b0.x, qy = b1.y - b0.y;
            const double den = cross(rx, ry, qx, qy);
            if (std::abs(den) > 1e-12) {
                const double t = cross(b0.x - a0.x, b0.y - a0.y, qx, qy) / den;
                const double u = cross(b0.x - a0.x, b0.y - a0.y, rx, ry) / den;
                if (t >= -1e-9 && t <= 1.0 + 1e-9 && u >= -1e-9 && u <= 1.0 + 1e-9) {
                    pts.push_back({a0.x + t * rx, a0.y + t * ry});
                    continue;
                }
            }
            // No crossing. The step from one tip corner to the next lies mostly along the path on a smooth curve (the offset lines meet at a
            // shallow angle, far outside their segments): one point between the two is as good as either, and it keeps the border from zigzagging
            // by an edge of the tip. A step across the path (a hairpin) is a corner of the outline: both points, cut there.
            const double ex = b0.x - a1.x, ey = b0.y - a1.y, edge = std::hypot(ex, ey);
            if (edge < 1e-12 || std::abs(cross(tx_[prev], ty_[prev], ex, ey)) <= 0.25 * edge) {
                pts.push_back({a1.x + ex * 0.5, a1.y + ey * 0.5});
                continue;
            }
            breaks.push_back(pts.size());
            pts.push_back(a1);
            breaks.push_back(pts.size());
            pts.push_back(b0);
        }
    }

    const NibBrush& nib_;
    std::vector<Point> v_;
    std::vector<double> s_;
    bool closed_ = false;
    std::size_t n_ = 0;
    std::size_t segments_ = 0;
    std::vector<double> tx_, ty_;
    std::vector<std::size_t> supportLeft_, supportRight_;
};
}  // namespace

BrushBorders BrushEnvelope::sweep(const NibBrush& nib, const std::vector<Point>& vertices, const std::vector<double>& scale, bool closed) {
    BrushBorders out;
    if (vertices.empty() || nib.size() < 3) {
        out.single = true;
        return out;
    }
    std::vector<Point> kept;
    std::vector<double> scales;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const Point& p = vertices[i];
        if (!kept.empty() && std::hypot(p.x - kept.back().x, p.y - kept.back().y) < 1e-9) {
            continue;
        }
        kept.push_back(p);
        scales.push_back(i < scale.size() ? std::max(scale[i], 1e-3) : 1.0);
    }
    if (closed && kept.size() > 1 && std::hypot(kept.front().x - kept.back().x, kept.front().y - kept.back().y) < 1e-9) {
        kept.pop_back();
        scales.pop_back();
    }
    if (kept.size() < 2 || (closed && kept.size() < 3)) {
        out.single = true;
        for (const Point& c : nib.corners) {
            out.startCap.push_back({kept[0].x + scales[0] * c.x, kept[0].y + scales[0] * c.y});
        }
        return out;
    }
    return Sweeper(nib, std::move(kept), std::move(scales), closed).run();
}

std::vector<Point> BrushEnvelope::polygon(const BrushBorders& borders) {
    std::vector<Point> out;
    if (borders.single) {
        return borders.startCap;
    }
    out.insert(out.end(), borders.left.begin(), borders.left.end());
    if (borders.closed) {
        out.insert(out.end(), borders.right.begin(), borders.right.end());
        return out;
    }
    out.insert(out.end(), borders.endCap.begin(), borders.endCap.end());
    out.insert(out.end(), borders.right.rbegin(), borders.right.rend());
    out.insert(out.end(), borders.startCap.begin(), borders.startCap.end());
    return out;
}

}  // namespace jitnodes
