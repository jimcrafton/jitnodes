#include "jitnodes/audio/audioProgram.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <stdexcept>

#include "jitnodes/audio/audioStep.h"
#include "jitnodes/jit/fuse.h"

namespace jitnodes {

namespace {

// Channel-name prefix -> source index. A source reached through nested mixes is read under the prefixes mixKernels gave its branches
// ("1.0."); a source feeding the root chain directly has the empty prefix. Attribute names contain no '.', which keeps the prefixes
// from matching each other's channels.
using SourcePrefixes = std::vector<std::pair<std::string, size_t>>;

bool splitChannel(const SourcePrefixes& sources, const std::string& channel, size_t& index, std::string& attrib) {
    for (const auto& [prefix, source] : sources) {
        if (channel.compare(0, prefix.size(), prefix) != 0 || channel.find('.', prefix.size()) != std::string::npos) continue;
        index = source;
        attrib = channel.substr(prefix.size());
        return true;
    }
    return false;
}

bool isMix(const Node& node) { return node.typeName() == "mix"; }

}  // namespace

struct AudioProgram::Branch {
    KernelSpec spec;
    std::vector<float> params;
    std::vector<ParamRange> ranges;
    SourcePrefixes sources;
};

// Everything one kernel stage needs at run time.
struct AudioProgram::Stage {
    std::shared_ptr<const Kernel> kernel;
    Geometry work;                     // owns the channel buffers the kernel runs on
    std::vector<Geometry> sources;     // never written after compile; a step's source is overwritten through its buffer instead
    std::vector<void*> channels;
    std::vector<const void*> restore;  // per channel: copy restored each block, null = zeroed
    std::vector<const float*> outputs;
    std::vector<ParamRange> ranges;
    std::vector<float> params;
    size_t paddedBytes = 0;

    void run(size_t blockSize) {
        for (size_t c = 0; c < channels.size(); ++c) {
            if (restore[c]) std::memcpy(channels[c], restore[c], paddedBytes);
            else std::memset(channels[c], 0, paddedBytes);
        }
        kernel->run(channels.data(), params.data(), blockSize);
    }
};

struct AudioProgram::StepRun {
    NodeId node;
    std::unique_ptr<AudioStep> step;
    std::vector<float> params;
    StateBlock* state = nullptr;
    const float* src = nullptr;
    float* dst = nullptr;
    size_t count = 0;
    float* tap = nullptr;  // fbSink: the block is also copied here

    void run(const EventBuffer& events) {
        std::memcpy(dst, src, count * sizeof(float));
        step->processEvents(dst, count, params.data(), state->data(), events);
        if (tap) std::memcpy(tap, dst, count * sizeof(float));
    }
};

// `next` is written by the sink during a block and becomes `current`, which the sources read, only when the block ends.
struct AudioProgram::Feedback {
    explicit Feedback(size_t bytes) : current(bytes), next(bytes) {}
    StateBlock current;
    StateBlock next;
    bool hasSource = false;
    bool hasSink = false;
};

struct AudioProgram::BuildState {
    AudioProgram& program;
    std::set<NodeId>& claimed;  // point-wise, mix and step nodes: their parameters live in one place, so each is used once
    size_t blockSize;
    std::map<NodeId, size_t> sourceIndex;
    std::map<size_t, const float*> stepData;  // source index -> the buffer its step writes, which is what the stage restores x from
    std::vector<Geometry> sources;
};

AudioProgram::AudioProgram() = default;
AudioProgram::~AudioProgram() = default;

AudioProgram::Branch AudioProgram::build(Network& net, NodeId id, BuildState& state) {
    auto claim = [&](NodeId node) {
        if (!state.claimed.insert(node).second) throw std::runtime_error("audio program: " + net.node(node).name() + " is used by more than one branch");
    };

    // Tail first, as in a cook task's chain.
    std::vector<NodeId> chain;
    NodeId cursor = id;
    while (net.node(cursor).isPointwise()) {
        claim(cursor);
        chain.push_back(cursor);
        cursor = net.inputOf(cursor, 0);
        if (cursor == invalidNodeId) throw std::runtime_error("audio program: " + net.node(chain.back()).name() + " has no input connected");
    }

    Branch branch;
    size_t sourceIndex = state.sources.size();
    const bool mixed = isMix(net.node(cursor));
    if (mixed) {
        claim(cursor);
        branch = buildMix(net, cursor, state);
    } else if (std::vector<float> probe; net.node(cursor).audioStep(probe)) {
        claim(cursor);
        sourceIndex = makeStepSource(net, cursor, state);
        branch.sources.push_back({"", sourceIndex});
    } else {
        auto known = state.sourceIndex.find(cursor);
        if (known != state.sourceIndex.end()) sourceIndex = known->second;
        else if (net.node(cursor).typeName() == "fbSource") {
            sourceIndex = makeFeedbackSource(net, cursor, state);
            state.sourceIndex[cursor] = sourceIndex;
        } else {
            Geometry source = *net.cook(cursor);
            if (source.pointCount() != state.blockSize)
                throw std::runtime_error("audio program: source " + net.node(cursor).name() + " has " + std::to_string(source.pointCount()) +
                                         " points, the block size is " + std::to_string(state.blockSize));
            state.sourceIndex[cursor] = sourceIndex;
            state.sources.push_back(std::move(source));
        }
        branch.sources.push_back({"", sourceIndex});
    }

    auto channelType = [&](const std::string& name) {
        const AttribArray* a = mixed ? nullptr : state.sources[sourceIndex].findAttrib(AttribClass::point, name);
        return a ? a->type() : AttribType::float32;
    };

    for (size_t i = chain.size(); i-- > 0;) {
        Node& node = net.node(chain[i]);
        if (node.attribClass() != AttribClass::point) throw std::runtime_error("audio program: " + node.name() + " is not a point node");
        PointKernel k;
        node.pointKernel(k);
        appendKernel(branch.spec, k.spec, channelType);
        branch.ranges.push_back({chain[i], branch.params.size(), k.params.size()});
        branch.params.insert(branch.params.end(), k.params.begin(), k.params.end());
    }
    return branch;
}

AudioProgram::Branch AudioProgram::buildMix(Network& net, NodeId id, BuildState& state) {
    std::vector<Branch> inputs;
    const int slots = net.node(id).inputCount();
    for (int i = 0; i < slots; ++i) {
        const NodeId up = net.inputOf(id, i);
        if (up != invalidNodeId) inputs.push_back(build(net, up, state));
    }
    if (inputs.empty()) throw std::runtime_error("audio program: " + net.node(id).name() + " has no input connected");

    Branch mix;
    std::vector<KernelSpec> specs;
    for (size_t i = 0; i < inputs.size(); ++i) {
        Branch& in = inputs[i];
        const size_t offset = mix.params.size();
        for (const ParamRange& r : in.ranges) mix.ranges.push_back({r.node, r.first + offset, r.count});
        mix.params.insert(mix.params.end(), in.params.begin(), in.params.end());
        for (const auto& [prefix, source] : in.sources) mix.sources.push_back({std::to_string(i) + "." + prefix, source});
        specs.push_back(std::move(in.spec));
    }
    mix.spec = mixKernels(specs, "x");
    return mix;
}

std::unique_ptr<AudioProgram> AudioProgram::compile(Network& net, NodeId output, const AudioProgramOptions& options) {
    if (options.blockSize == 0 || options.blockSize % simdLanes != 0)
        throw std::runtime_error("audio program: block size must be a multiple of " + std::to_string(simdLanes));
    if (options.outputs.empty()) throw std::runtime_error("audio program: no outputs requested");
    if (!net.node(output).isPointwise() && !isMix(net.node(output)))
        throw std::runtime_error("audio program: " + net.node(output).name() + " is not a point-wise node");

    std::unique_ptr<AudioProgram> p(new AudioProgram());
    p->blockSize_ = options.blockSize;
    std::set<NodeId> claimed;
    BuildState state{*p, claimed, options.blockSize, {}, {}, {}};
    Branch root = build(net, output, state);
    p->outputs_ = makeStage(net, root, state, options.outputs)->outputs;
    for (const auto& [name, fb] : p->feedback_)
        if (!fb->hasSource || !fb->hasSink) throw std::runtime_error("audio program: feedback '" + name + "' needs both an fbSink and an fbSource");
    return p;
}

AudioProgram::Feedback& AudioProgram::feedback(const std::string& name, size_t blockSize) {
    if (name.empty()) throw std::runtime_error("audio program: feedback node without a name");
    auto it = feedback_.find(name);
    if (it == feedback_.end()) it = feedback_.emplace(name, std::make_unique<Feedback>(padToLanes(blockSize) * sizeof(float))).first;
    return *it->second;
}

size_t AudioProgram::makeFeedbackSource(Network& net, NodeId id, BuildState& state) {
    Feedback& fb = state.program.feedback(net.node(id).getString("name"), state.blockSize);
    fb.hasSource = true;
    Geometry placeholder;
    placeholder.resizePoints(state.blockSize);
    state.sources.push_back(std::move(placeholder));
    state.stepData[state.sources.size() - 1] = fb.current.as<float>();
    return state.sources.size() - 1;
}

// Compiles `root` (whose sources are in state.sources) into the next stage of the program.
AudioProgram::Stage* AudioProgram::makeStage(Network& net, Branch& root, BuildState& state, const std::vector<std::string>& outputs) {
    auto stage = std::make_unique<Stage>();
    stage->sources = std::move(state.sources);
    stage->work = stage->sources.front();

    auto sourceAttrib = [&](const std::string& channel) -> const AttribArray* {
        size_t index;
        std::string attrib;
        return splitChannel(root.sources, channel, index, attrib) ? stage->sources[index].findAttrib(AttribClass::point, attrib) : nullptr;
    };
    // A step's source reads its x from the step's buffer, which the step rewrites every block; any other channel is the placeholder's.
    auto restoreOf = [&](const std::string& channel) -> const void* {
        size_t index;
        std::string attrib;
        if (!splitChannel(root.sources, channel, index, attrib)) return nullptr;
        const auto step = state.stepData.find(index);
        if (step != state.stepData.end() && attrib == "x") return step->second;
        const AttribArray* a = stage->sources[index].findAttrib(AttribClass::point, attrib);
        return a ? a->data() : nullptr;
    };

    // A branch channel such as "1.x" is a copy of its source's x; it has to exist, with the source's type, before the kernel is typed.
    for (const std::string& channel : root.spec.channels)
        if (const AttribArray* a = sourceAttrib(channel)) stage->work.attrib(AttribClass::point, channel, a->type());

    stage->kernel = net.compiler().compileFor(root.spec, stage->work, AttribClass::point);
    resolveStringParams(root.spec, stage->work, AttribClass::point, root.params);
    stage->params = std::move(root.params);
    stage->ranges = std::move(root.ranges);

    // Every attribute is created and detached from the sources here, so the pointers stay valid and process() never allocates.
    const KernelSpec& spec = stage->kernel->spec();
    for (size_t c = 0; c < spec.channels.size(); ++c) {
        AttribArray& a = stage->work.attrib(AttribClass::point, spec.channels[c], spec.channelTypes[c]);
        stage->channels.push_back(a.data());
        stage->restore.push_back(restoreOf(spec.channels[c]));
    }
    stage->paddedBytes = padToLanes(state.blockSize) * sizeof(float);

    for (const std::string& name : outputs) {
        const AttribArray* a = stage->work.findAttrib(AttribClass::point, name);
        if (!a || a->type() != AttribType::float32)
            throw std::runtime_error("audio program: output channel '" + name + "' is not a float channel of the program");
        stage->outputs.push_back(static_cast<const float*>(a->data()));
    }

    Stage* raw = stage.get();
    state.program.stages_.push_back(std::move(stage));
    state.program.order_.push_back({raw, nullptr});
    return raw;
}

// Builds everything above a step node as its own stage, schedules the step after it, and returns the index (in `state`) of the placeholder
// source the stage below the step reads the step's output through.
size_t AudioProgram::makeStepSource(Network& net, NodeId id, BuildState& state) {
    AudioProgram& program = state.program;
    const NodeId up = net.inputOf(id, 0);
    if (up == invalidNodeId) throw std::runtime_error("audio program: " + net.node(id).name() + " has no input connected");

    auto run = std::make_unique<StepRun>();
    run->node = id;
    run->step = net.node(id).audioStep(run->params);
    run->count = state.blockSize;
    run->state = &program.state(id, run->step->stateBytes());
    program.buffers_.push_back(std::make_unique<StateBlock>(padToLanes(state.blockSize) * sizeof(float)));
    run->dst = program.buffers_.back()->as<float>();
    if (net.node(id).typeName() == "fbSink") {
        Feedback& fb = program.feedback(net.node(id).getString("name"), state.blockSize);
        if (fb.hasSink) throw std::runtime_error("audio program: feedback '" + net.node(id).getString("name") + "' has more than one fbSink");
        fb.hasSink = true;
        run->tap = fb.next.as<float>();
    }

    BuildState inner{program, state.claimed, state.blockSize, {}, {}, {}};
    Branch branch = build(net, up, inner);
    const bool bareSource = branch.sources.size() == 1 && branch.sources[0].first.empty() && branch.ranges.empty() && branch.spec.channels.empty();
    if (bareSource) {
        const auto upstream = inner.stepData.find(0);
        if (upstream != inner.stepData.end()) run->src = upstream->second;
        else {
            program.inputs_.push_back(std::move(inner.sources.front()));
            run->src = static_cast<const float*>(program.inputs_.back().findAttrib(AttribClass::point, "x")->data());
        }
    } else {
        run->src = makeStage(net, branch, inner, {"x"})->outputs[0];
    }

    Geometry placeholder;
    placeholder.resizePoints(state.blockSize);
    state.sources.push_back(std::move(placeholder));
    state.stepData[state.sources.size() - 1] = run->dst;

    program.order_.push_back({nullptr, run.get()});
    program.steps_.push_back(std::move(run));
    return state.sources.size() - 1;
}

float* AudioProgram::tryParam(NodeId node, size_t index) noexcept {
    for (const auto& stage : stages_)
        for (const ParamRange& r : stage->ranges)
            if (r.node == node) return index < r.count ? &stage->params[r.first + index] : nullptr;
    for (const auto& step : steps_)
        if (step->node == node) return index < step->params.size() ? &step->params[index] : nullptr;
    return nullptr;
}

float& AudioProgram::param(NodeId node, size_t index) {
    if (float* p = tryParam(node, index)) return *p;
    for (const auto& stage : stages_)
        for (const ParamRange& r : stage->ranges)
            if (r.node == node) throw std::out_of_range("audio program: parameter index out of range");
    for (const auto& step : steps_)
        if (step->node == node) throw std::out_of_range("audio program: parameter index out of range");
    throw std::runtime_error("audio program: node is not part of the compiled chain");
}

StateBlock::StateBlock(size_t bytes) : size_(bytes) {
    data_ = ::operator new(bytes ? bytes : 1, std::align_val_t(64));
    clear();
}

StateBlock::~StateBlock() { ::operator delete(data_, std::align_val_t(64)); }

void StateBlock::clear() { std::memset(data_, 0, size_ ? size_ : 1); }

StateBlock& AudioProgram::state(NodeId node, size_t bytes) {
    auto it = states_.find(node);
    if (it == states_.end()) it = states_.emplace(node, std::make_unique<StateBlock>(bytes)).first;
    else if (it->second->size() != bytes) throw std::runtime_error("audio program: state of " + std::to_string(node) + " already exists with another size");
    return *it->second;
}

void AudioProgram::addPhase(NodeId node, size_t phaseParam, size_t incParam) {
    float* phaseAt = &param(node, phaseParam);
    const float* incAt = &param(node, incParam);
    if (phaseAt == incAt) throw std::runtime_error("audio program: phase and increment must be different parameters");
    phases_.push_back({phaseAt, incAt, state(node, sizeof(double)).as<double>()});
}

void AudioProgram::resetState() {
    for (auto& [node, block] : states_) block->clear();
    for (auto& [name, fb] : feedback_) {
        fb->current.clear();
        fb->next.clear();
    }
}

void AudioProgram::migrateState(const AudioProgram& previous) {
    for (auto& [node, block] : states_) {
        const auto old = previous.states_.find(node);
        if (old == previous.states_.end()) continue;
        const StateBlock& from = *old->second;
        const auto step = std::find_if(steps_.begin(), steps_.end(), [&](const auto& s) { return s->node == node; });
        if (step != steps_.end()) (*step)->step->migrate(block->data(), block->size(), from.data(), from.size());
        else if (from.size() == block->size()) std::memcpy(block->data(), from.data(), block->size());
    }
    if (previous.blockSize_ != blockSize_) return;
    for (auto& [name, fb] : feedback_) {
        const auto old = previous.feedback_.find(name);
        if (old == previous.feedback_.end()) continue;
        std::memcpy(fb->current.data(), old->second->current.data(), fb->current.size());
        std::memcpy(fb->next.data(), old->second->next.data(), fb->next.size());
    }
}

void AudioProgram::process(float* const* out, const EventBuffer* events) {
    static const EventBuffer none;
    const EventBuffer& block = events ? *events : none;
    for (const Phase& p : phases_) *p.phaseParam = static_cast<float>(*p.phase);
    for (const auto& [stage, step] : order_) {
        if (stage) stage->run(blockSize_);
        else step->run(block);
    }
    for (auto& [name, fb] : feedback_) std::memcpy(fb->current.data(), fb->next.data(), blockSize_ * sizeof(float));
    for (const Phase& p : phases_) {
        double next = *p.phase + static_cast<double>(*p.incParam) * static_cast<double>(blockSize_);
        *p.phase = next - std::floor(next);
    }
    if (!out) return;
    for (size_t i = 0; i < outputs_.size(); ++i)
        if (out[i]) std::memcpy(out[i], outputs_[i], blockSize_ * sizeof(float));
}

}  // namespace jitnodes
