#pragma once

#include <cstddef>
#include <cstring>

#include "jitnodes/audio/eventBuffer.h"

namespace jitnodes {

// A sequential, stateful audio operation on the x channel that cannot be a point-wise kernel (a delay line, an IIR filter). An
// AudioProgram runs it between kernel stages with a StateBlock of stateBytes() zeroed bytes that it owns, so a block-by-block run and
// one run over the whole signal give the same result. process() must not allocate or lock.
class AudioStep {
public:
    virtual ~AudioStep() = default;
    virtual size_t stateBytes() const = 0;
    // `params` are the node's values in the order the node documents them; read afresh at every block.
    virtual void process(float* x, size_t count, const float* params, void* state) const = 0;
    // The same with the block's events, which a step can act on at their offsets (split the block there). The default ignores them.
    virtual void processEvents(float* x, size_t count, const float* params, void* state, const EventBuffer&) const {
        process(x, count, params, state);
    }
    // Carries the state of the same node in a previous program (possibly built with other settings, hence another size) into the zeroed
    // `dst` of this step. The default copies only when the layouts have the same size.
    virtual void migrate(void* dst, size_t dstBytes, const void* src, size_t srcBytes) const;
};

inline void AudioStep::migrate(void* dst, size_t dstBytes, const void* src, size_t srcBytes) const {
    if (dstBytes == srcBytes) std::memcpy(dst, src, dstBytes);
}

}  // namespace jitnodes
