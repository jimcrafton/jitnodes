#pragma once

#include <string>
#include <vector>

#include "jitnodes/jitnodes.h"

namespace jitnodes::editor {

const char* interpName(KeyInterp interp);
bool parseInterp(const std::string& name, KeyInterp& out);

// A working copy of one parameter's keyframes for the keyframe dialog: always sorted by time, no two keys at the same
// time. Operations return an error message (empty on success) and leave the keys unchanged on failure.
class KeyframeEditor {
public:
    explicit KeyframeEditor(std::vector<Keyframe> keys = {});

    const std::vector<Keyframe>& keys() const { return keys_; }
    size_t size() const { return keys_.size(); }

    // Moves key `index` to `time`; `index` becomes the key's new position in the sorted list.
    std::string setTime(size_t& index, double time);
    std::string setValue(size_t index, float value);
    std::string setInterp(size_t index, KeyInterp interp);
    std::string remove(size_t index);
    // Adds a key at `time` holding the curve's current value there, so the curve does not change; `index` is its position.
    std::string addAt(double time, size_t& index);

    // The curve at `time`; 0 without keys.
    float valueAt(double time) const;

    struct Range {
        double t0 = 0.0, t1 = 1.0;
        float v0 = 0.0f, v1 = 1.0f;
    };
    // Time and value extent of the keys, widened so it is never empty (for drawing).
    Range range() const;

private:
    std::vector<Keyframe> keys_;
};

}  // namespace jitnodes::editor
