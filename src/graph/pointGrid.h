#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace jitnodes {

constexpr float maxGridCells = 4.0e6f;
constexpr float maxGridSide = 8192.0f;

// A uniform grid whose cells are at least `radius` wide, so every neighbour closer than `radius` is in the 3 x 3 cells around a point.
class PointGrid {
public:
    void build(const float* xs, const float* ys, size_t n, float radius) {
        float minX = std::numeric_limits<float>::max(), minY = minX, maxX = std::numeric_limits<float>::lowest(), maxY = maxX;
        for (size_t i = 0; i < n; ++i) {
            if (xs[i] < minX) minX = xs[i];
            if (xs[i] > maxX) maxX = xs[i];
            if (ys[i] < minY) minY = ys[i];
            if (ys[i] > maxY) maxY = ys[i];
        }
        if (n == 0 || minX > maxX || minY > maxY) minX = minY = maxX = maxY = 0.0f;
        const float w = maxX - minX, h = maxY - minY;
        cell_ = std::max({radius, std::sqrt(w * h / maxGridCells), std::max(w, h) / maxGridSide});
        if (!std::isfinite(cell_)) throw std::runtime_error("point positions are not finite");
        minX_ = minX;
        minY_ = minY;
        cols_ = static_cast<int>(w / cell_) + 1;
        rows_ = static_cast<int>(h / cell_) + 1;
        start_.assign(static_cast<size_t>(cols_) * static_cast<size_t>(rows_) + 1, 0);
        for (size_t i = 0; i < n; ++i) ++start_[cellOf(xs[i], ys[i]) + 1];
        for (size_t c = 1; c < start_.size(); ++c) start_[c] += start_[c - 1];
        items_.resize(n);
        std::vector<uint32_t> fill(start_.begin(), start_.end() - 1);
        for (size_t i = 0; i < n; ++i) items_[fill[cellOf(xs[i], ys[i])]++] = static_cast<uint32_t>(i);
    }

    float cellSize() const { return cell_; }
    // The largest ring around any cell that still touches the grid.
    int maxRing() const { return std::max(cols_, rows_); }

    // Calls fn(index) for the points in the 3 x 3 cells around (x, y).
    template <class F>
    void forEachNear(float x, float y, F&& fn) const {
        const int cx = column(x), cy = row(y);
        for (int r = std::max(cy - 1, 0); r <= std::min(cy + 1, rows_ - 1); ++r)
            for (int c = std::max(cx - 1, 0); c <= std::min(cx + 1, cols_ - 1); ++c) visit(r, c, fn);
    }

    // Calls fn(index) for the points in the cells at Chebyshev distance exactly `ring` from the cell of (x, y); ring 0 is that cell. Every
    // point in ring k + 1 or beyond is farther than k * cellSize() from (x, y) when (x, y) lies inside the grid.
    template <class F>
    void forEachInRing(float x, float y, int ring, F&& fn) const {
        const int cx = column(x), cy = row(y);
        if (ring == 0) {
            visit(cy, cx, fn);
            return;
        }
        for (int r = cy - ring; r <= cy + ring; ++r) {
            if (r < 0 || r >= rows_) continue;
            const bool edgeRow = r == cy - ring || r == cy + ring;
            for (int c = cx - ring; c <= cx + ring; c += edgeRow ? 1 : 2 * ring) {
                if (c >= 0 && c < cols_) visit(r, c, fn);
            }
        }
    }

private:
    template <class F>
    void visit(int r, int c, F& fn) const {
        const size_t cell = static_cast<size_t>(r) * static_cast<size_t>(cols_) + static_cast<size_t>(c);
        for (uint32_t k = start_[cell]; k < start_[cell + 1]; ++k) fn(items_[k]);
    }

    // NaN and out-of-range positions land in the nearest edge cell.
    int column(float x) const {
        const float v = (x - minX_) / cell_;
        return static_cast<int>(std::min(v > 0.0f ? v : 0.0f, static_cast<float>(cols_ - 1)));
    }
    int row(float y) const {
        const float v = (y - minY_) / cell_;
        return static_cast<int>(std::min(v > 0.0f ? v : 0.0f, static_cast<float>(rows_ - 1)));
    }
    size_t cellOf(float x, float y) const { return static_cast<size_t>(row(y)) * static_cast<size_t>(cols_) + static_cast<size_t>(column(x)); }

    float minX_ = 0.0f, minY_ = 0.0f, cell_ = 1.0f;
    int cols_ = 1, rows_ = 1;
    std::vector<uint32_t> start_;
    std::vector<uint32_t> items_;
};

}  // namespace jitnodes
