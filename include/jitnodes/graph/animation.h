#pragma once

#include <vector>

namespace jitnodes {

enum class KeyInterp { constant, linear, smooth };

// The interpolation mode of a key applies to the segment that starts at that key.
struct Keyframe {
    double time = 0.0;
    float value = 0.0f;
    KeyInterp interp = KeyInterp::linear;
};

void sortKeyframes(std::vector<Keyframe>& keys);

// Holds the first/last value outside the keyed range. keys must be sorted by time.
float evaluateKeyframes(const std::vector<Keyframe>& keys, double time);

}  // namespace jitnodes
