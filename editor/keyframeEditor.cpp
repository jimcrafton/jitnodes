#include "keyframeEditor.h"

#include <algorithm>
#include <cmath>

namespace jitnodes::editor {

const char* interpName(KeyInterp interp) {
    switch (interp) {
        case KeyInterp::constant: return "constant";
        case KeyInterp::linear: return "linear";
        case KeyInterp::smooth: return "smooth";
    }
    return "linear";
}

bool parseInterp(const std::string& name, KeyInterp& out) {
    for (KeyInterp k : {KeyInterp::constant, KeyInterp::linear, KeyInterp::smooth}) {
        if (name == interpName(k)) {
            out = k;
            return true;
        }
    }
    return false;
}

KeyframeEditor::KeyframeEditor(std::vector<Keyframe> keys) : keys_(std::move(keys)) { sortKeyframes(keys_); }

std::string KeyframeEditor::setTime(size_t& index, double time) {
    if (index >= keys_.size()) return "no such key";
    if (!std::isfinite(time)) return "the time must be a number";
    for (size_t i = 0; i < keys_.size(); ++i)
        if (i != index && std::fabs(keys_[i].time - time) < 1e-9) return "another key is already at that time";
    Keyframe k = keys_[index];
    k.time = time;
    keys_.erase(keys_.begin() + static_cast<std::ptrdiff_t>(index));
    auto at = std::lower_bound(keys_.begin(), keys_.end(), k, [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    index = static_cast<size_t>(at - keys_.begin());
    keys_.insert(at, k);
    return {};
}

std::string KeyframeEditor::setValue(size_t index, float value) {
    if (index >= keys_.size()) return "no such key";
    if (!std::isfinite(value)) return "the value must be a number";
    keys_[index].value = value;
    return {};
}

std::string KeyframeEditor::setInterp(size_t index, KeyInterp interp) {
    if (index >= keys_.size()) return "no such key";
    keys_[index].interp = interp;
    return {};
}

std::string KeyframeEditor::remove(size_t index) {
    if (index >= keys_.size()) return "no such key";
    keys_.erase(keys_.begin() + static_cast<std::ptrdiff_t>(index));
    return {};
}

std::string KeyframeEditor::addAt(double time, size_t& index) {
    if (!std::isfinite(time)) return "the time must be a number";
    for (const Keyframe& k : keys_)
        if (std::fabs(k.time - time) < 1e-9) return "a key already exists at that time";
    Keyframe k{time, valueAt(time), KeyInterp::linear};
    auto at = std::lower_bound(keys_.begin(), keys_.end(), k, [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    index = static_cast<size_t>(at - keys_.begin());
    keys_.insert(at, k);
    return {};
}

float KeyframeEditor::valueAt(double time) const { return keys_.empty() ? 0.0f : evaluateKeyframes(keys_, time); }

KeyframeEditor::Range KeyframeEditor::range() const {
    Range r;
    if (keys_.empty()) return r;
    r.t0 = keys_.front().time;
    r.t1 = keys_.back().time;
    r.v0 = r.v1 = keys_.front().value;
    for (const Keyframe& k : keys_) {
        r.v0 = std::min(r.v0, k.value);
        r.v1 = std::max(r.v1, k.value);
    }
    if (r.t1 - r.t0 < 1e-6) r.t1 = r.t0 + 1.0;
    if (r.v1 - r.v0 < 1e-6f) {
        r.v0 -= 0.5f;
        r.v1 += 0.5f;
    }
    return r;
}

}  // namespace jitnodes::editor
