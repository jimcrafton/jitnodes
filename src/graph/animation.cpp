#include "jitnodes/graph/animation.h"

#include <algorithm>

namespace jitnodes {

void sortKeyframes(std::vector<Keyframe>& keys) {
    std::stable_sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
}

float evaluateKeyframes(const std::vector<Keyframe>& keys, double time) {
    if (keys.empty()) return 0.0f;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;

    auto next = std::upper_bound(keys.begin(), keys.end(), time, [](double t, const Keyframe& k) { return t < k.time; });
    const Keyframe& b = *next;
    const Keyframe& a = *(next - 1);
    if (a.interp == KeyInterp::constant) return a.value;

    float t = static_cast<float>((time - a.time) / (b.time - a.time));
    if (a.interp == KeyInterp::smooth) t = t * t * (3.0f - 2.0f * t);
    return a.value + (b.value - a.value) * t;
}

}  // namespace jitnodes
