#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

namespace jitnodes {

// A 2D affine transform, the six numbers of a 3x3 matrix whose last row is (0, 0, 1). Same layout as SVG's matrix(a b c d e f) and Blend2D's
// BLMatrix2D (m00 m01 m10 m11 m20 m21):
//
//     x' = a * x + c * y + e
//     y' = b * x + d * y + f
//
// The y axis points down in the images it is used for. then() composes in drawing order: `first.then(second)` applies first, then second.
struct Matrix2D {
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;

    static Matrix2D identity() { return Matrix2D(); }
    static Matrix2D translation(double tx, double ty) { return {1.0, 0.0, 0.0, 1.0, tx, ty}; }
    static Matrix2D scaling(double sx, double sy) { return {sx, 0.0, 0.0, sy, 0.0, 0.0}; }
    static Matrix2D scaling(double s) { return scaling(s, s); }
    static Matrix2D rotation(double radians) {
        const double cs = std::cos(radians), sn = std::sin(radians);
        return {cs, sn, -sn, cs, 0.0, 0.0};
    }

    std::pair<double, double> apply(double x, double y) const { return {a * x + c * y + e, b * x + d * y + f}; }
    // The vector (x, y) without the translation.
    std::pair<double, double> applyVector(double x, double y) const { return {a * x + c * y, b * x + d * y}; }

    // This transform followed by `next`.
    Matrix2D then(const Matrix2D& next) const {
        return {next.a * a + next.c * b, next.b * a + next.d * b, next.a * c + next.c * d, next.b * c + next.d * d,
                next.a * e + next.c * f + next.e, next.b * e + next.d * f + next.f};
    }

    double determinant() const { return a * d - b * c; }
    bool invertible() const { return std::fabs(determinant()) > 1e-12; }
    // The transform that undoes this one; the identity when it is not invertible.
    Matrix2D inverse() const {
        const double det = determinant();
        if (std::fabs(det) <= 1e-12) {
            return Matrix2D();
        }
        const double ia = d / det, ib = -b / det, ic = -c / det, id = a / det;
        return {ia, ib, ic, id, -(ia * e + ic * f), -(ib * e + id * f)};
    }

    // Length of the image of a unit step along x: the scale of a transform that only scales, turns and moves.
    double scaleX() const { return std::hypot(a, b); }
    double scaleY() const { return std::hypot(c, d); }
    // True when it only moves and scales alike in x and y (no turn, no shear, no flip): what a picture can be stretched by without resampling
    // errors beyond blur.
    bool isTranslateScale(double tolerance = 1e-9) const {
        return std::fabs(b) <= tolerance && std::fabs(c) <= tolerance && std::fabs(a - d) <= tolerance * (std::max)(1.0, std::fabs(a)) && a > 0.0;
    }

    bool operator==(const Matrix2D& o) const { return a == o.a && b == o.b && c == o.c && d == o.d && e == o.e && f == o.f; }
    bool operator!=(const Matrix2D& o) const { return !(*this == o); }
};

}  // namespace jitnodes
