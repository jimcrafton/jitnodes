#pragma once

#include <cstddef>
#include <deque>
#include <map>
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

// Per-node state of an AudioProgram: a cache-line aligned, zeroed byte range that keeps its address for the program's lifetime.
class StateBlock {
public:
    explicit StateBlock(size_t bytes);
    ~StateBlock();
    StateBlock(const StateBlock&) = delete;
    StateBlock& operator=(const StateBlock&) = delete;

    size_t size() const { return size_; }
    void* data() { return data_; }
    const void* data() const { return data_; }
    template <class T> T* as() { return static_cast<T*>(data_); }
    void clear();

private:
    void* data_ = nullptr;
    size_t size_ = 0;
};

// An immutable-structure, preallocated program compiled from a Network on the main thread and run on the audio thread.
//
// The point-wise nodes that end at `output` are fused into one kernel and run over a block of `blockSize` points. A `mix` node
// joins any number of such chains (and nested mixes) into the sum of their x channels, and the chain above it continues from that
// sum. Each chain ends in a source (such as `line`, cooked once at compile time) that supplies its initial channels, restored at the
// start of every block. process() allocates nothing, locks nothing and never touches a thread pool.
//
// A node with an audioStep() (delay, biquad) cannot fuse: it splits the program into stages. The chain above it becomes its own kernel,
// the step runs on that kernel's x with the node's StateBlock, and the chain below it starts from the result as an ordinary source.
// Steps may sit inside mix branches. The program's output node itself must still be point-wise or a mix.
//
// An `fbSink` node (pass-through) and an `fbSource` node with the same `name` close a feedback loop that the acyclic graph cannot
// express: the source supplies the sink's signal from the previous block (zeros in the first), so the loop has one block of latency
// however the stages are ordered; loops shorter than a block are not possible. Both ends are required, and each name has one sink.
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

    // Same address as param(), or null when the node or index is not part of the program. Never throws, for the audio thread.
    float* tryParam(NodeId node, size_t index) noexcept;

    // Aligned, zero-initialised state owned by the program and keyed by node id. Allocated on the main thread before streaming (a
    // second call for the same node returns the existing block, which must have the same size); process() never allocates.
    StateBlock& state(NodeId node, size_t bytes);

    // Makes process() drive an oscillator whose kernel computes sin(2*pi*(phase + sampleIndex*inc)): before each block it writes the
    // accumulated phase (wrapped to [0,1)) into param `phaseParam` of `node`, and afterwards advances it by blockSize * the current
    // value of param `incParam`. The accumulator lives in the node's StateBlock, in double precision, so long runs do not drift.
    void addPhase(NodeId node, size_t phaseParam, size_t incParam);

    // Zeroes every StateBlock (phases restart at 0).
    void resetState();

    // Main thread, after addPhase()/state() have created this program's blocks and before it starts streaming (while `previous` is not
    // running): copies the state of every node that
    // exists in both programs, so a recompiled graph continues phases, delay lines, filter memory and feedback instead of restarting.
    // Steps decide how to carry state across a changed size (a longer or shorter delay keeps its most recent samples). Feedback buffers
    // are carried only when the block sizes match; nodes only in one program start from zero.
    void migrateState(const AudioProgram& previous);

    // Runs one block. out[i] receives blockSize floats for options.outputs[i]; null skips the copy.
    void process(float* const* out);

    // Runs one block into the internal buffers: commits the JIT stack and faults in the pages before streaming starts.
    void warm() { process(nullptr); }

private:
    AudioProgram();

    struct ParamRange {
        NodeId node;
        size_t first;
        size_t count;
    };
    struct Branch;
    struct BuildState;
    struct Stage;
    struct StepRun;
    struct Feedback;
    static Branch build(Network& net, NodeId id, BuildState& state);
    static Branch buildMix(Network& net, NodeId id, BuildState& state);
    static Stage* makeStage(Network& net, Branch& root, BuildState& state, const std::vector<std::string>& outputs);
    static size_t makeStepSource(Network& net, NodeId id, BuildState& state);
    static size_t makeFeedbackSource(Network& net, NodeId id, BuildState& state);
    Feedback& feedback(const std::string& name, size_t blockSize);

    size_t blockSize_ = 0;
    // Kernel stages and sequential steps in the order they run; a step reads the output of the stage before it and feeds a source of the
    // stage after it. The last stage produces the program's outputs.
    std::vector<std::unique_ptr<Stage>> stages_;
    std::vector<std::unique_ptr<StepRun>> steps_;
    std::vector<std::pair<Stage*, StepRun*>> order_;
    std::vector<std::unique_ptr<StateBlock>> buffers_;  // step outputs, read by the next stage as a source
    std::deque<Geometry> inputs_;                       // sources that feed a step without a kernel stage in between
    std::vector<const float*> outputs_;                 // the last stage's output channels

    struct Phase {
        float* phaseParam;
        const float* incParam;
        double* phase;
    };
    std::map<std::string, std::unique_ptr<Feedback>> feedback_;  // fbSink/fbSource pairs by name
    std::map<NodeId, std::unique_ptr<StateBlock>> states_;
    std::vector<Phase> phases_;
};

}  // namespace jitnodes
