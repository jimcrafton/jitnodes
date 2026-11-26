#include "jitnodes/geo/nibBrush.h"

#include <algorithm>
#include <cmath>

namespace jitnodes {

NibBrush NibBrush::oval(double width, double angleDegrees, double roundness, int sides) {
    constexpr double kPi = 3.14159265358979323846;
    NibBrush nib;
    const double major = width * 0.5;
    const double minor = std::max(major * roundness / 100.0, 0.05);
    const double turn = -angleDegrees * kPi / 180.0, c = std::cos(turn), s = std::sin(turn);
    const int count = std::max(sides, 3);
    for (int k = 0; k < count; ++k) {
        const double t = 2.0 * kPi * k / count, x = major * std::cos(t), y = minor * std::sin(t);
        nib.corners.push_back({x * c - y * s, x * s + y * c});
    }
    return nib;
}

std::size_t NibBrush::support(double dx, double dy) const {
    std::size_t best = 0;
    double bestValue = -1e300;
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const double value = corners[i].x * dx + corners[i].y * dy;
        if (value > bestValue + 1e-12) {
            bestValue = value;
            best = i;
        }
    }
    return best;
}

}  // namespace jitnodes
