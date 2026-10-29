#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "jitnodes/geo/geometry.h"
#include "jitnodes/graph/network.h"
#include "jitnodes/jit/kernel.h"

namespace jitnodes {

struct AudioProgramOptions {
    size_t blockSize = 128;                // frames per process() call; a multiple of simdLanes
    std::vector<std::string> outputs{"x"}; // channels copied to the caller, one per output
};

// An immutable-structure, preallocated program compiled from a Network on the main thread and run on the audio thread.
//
// The point-wise nodes that end at `output` are fused into one kernel and run over a block of `blockSize` points. A `mix` node
// joins any number of such chains (and nested mixes) into the sum of their x channels, and the chain above it continues from that
// sum. Each chain ends in a source (such as `line`, cooked once at compile time) that supplies its initial channels, restored at the
// start of every block. process() allocates nothing, locks nothing and never touches a thread pool.
class AudioProgram {
public:
    // Throws std::runtime_error when the graph is not point-wise chains and mixes over sources with blockSize points, or when a
    // node is reachable twice.
    static std::unique_ptr<AudioProgram> compile(Network& net, NodeId output, const AudioProgramOptions& options = {});

    ~AudioProgram();
    AudioProgram(const AudioProgram&) = delete;
    AudioProgram& operator=(const AudioProgram&) = delete;

    size_t blockSize() const { return blockSize_; }
    size_t outputCount() const { return outputs_.size(); }

    // Block-rate parameter of a node of the program: the index-th value of that node's PointKernel params (for a wrangle, a b c d).
    // Written by the audio thread (or before streaming starts) and read at the start of the next block.
    float& param(NodeId node, size_t index);

    // Runs one block. out[i] receives blockSize floats for options.outputs[i]; null skips the copy.
    void process(float* const* out);

    // Runs one block into the internal buffers: commits the JIT stack and faults in the pages before streaming starts.
    void warm() { process(nullptr); }

private:
    AudioProgram() = default;

    struct ParamRange {
        NodeId node;
        size_t first;
        size_t count;
    };
    struct Branch;
    struct BuildState;
    static Branch build(Network& net, NodeId id, BuildState& state);
    static Branch buildMix(Network& net, NodeId id, BuildState& state);

    size_t blockSize_ = 0;
    size_t paddedBytes_ = 0;
    std::shared_ptr<const Kernel> kernel_;
    Geometry work_;                   // owns the channel buffers the kernel runs on
    std::vector<Geometry> sources_;   // the cooked source nodes, never written after compile
    std::vector<void*> channels_;
    std::vector<const void*> restore_;  // per channel: pristine copy restored each block, null = zeroed
    std::vector<const float*> outputs_;
    std::vector<ParamRange> paramRanges_;
    std::vector<float> params_;
};

}  // namespace jitnodes
