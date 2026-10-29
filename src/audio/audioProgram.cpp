#include "jitnodes/audio/audioProgram.h"

#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

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

struct AudioProgram::BuildState {
    size_t blockSize;
    std::set<NodeId> claimed;  // point-wise and mix nodes: their parameters live in one place, so each is used once
    std::map<NodeId, size_t> sourceIndex;
    std::vector<Geometry> sources;
};

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
    } else {
        auto known = state.sourceIndex.find(cursor);
        if (known != state.sourceIndex.end()) sourceIndex = known->second;
        else {
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

    BuildState state{options.blockSize, {}, {}, {}};
    Branch root = build(net, output, state);

    std::unique_ptr<AudioProgram> p(new AudioProgram());
    p->blockSize_ = options.blockSize;
    p->sources_ = std::move(state.sources);
    p->work_ = p->sources_.front();

    auto sourceAttrib = [&](const std::string& channel) -> const AttribArray* {
        size_t index;
        std::string attrib;
        return splitChannel(root.sources, channel, index, attrib) ? p->sources_[index].findAttrib(AttribClass::point, attrib) : nullptr;
    };

    // A branch channel such as "1.x" is a copy of its source's x; it has to exist, with the source's type, before the kernel is typed.
    for (const std::string& channel : root.spec.channels)
        if (const AttribArray* a = sourceAttrib(channel)) p->work_.attrib(AttribClass::point, channel, a->type());

    p->kernel_ = net.compiler().compileFor(root.spec, p->work_, AttribClass::point);
    resolveStringParams(root.spec, p->work_, AttribClass::point, root.params);
    p->params_ = std::move(root.params);
    p->paramRanges_ = std::move(root.ranges);

    // Every attribute is created and detached from the sources here, so the pointers stay valid and process() never allocates.
    const KernelSpec& spec = p->kernel_->spec();
    for (size_t c = 0; c < spec.channels.size(); ++c) {
        AttribArray& a = p->work_.attrib(AttribClass::point, spec.channels[c], spec.channelTypes[c]);
        p->channels_.push_back(a.data());
        const AttribArray* src = sourceAttrib(spec.channels[c]);
        p->restore_.push_back(src ? src->data() : nullptr);
    }
    p->paddedBytes_ = padToLanes(options.blockSize) * sizeof(float);

    for (const std::string& name : options.outputs) {
        const AttribArray* a = p->work_.findAttrib(AttribClass::point, name);
        if (!a || a->type() != AttribType::float32)
            throw std::runtime_error("audio program: output channel '" + name + "' is not a float channel of the program");
        p->outputs_.push_back(static_cast<const float*>(a->data()));
    }
    return p;
}

float& AudioProgram::param(NodeId node, size_t index) {
    for (const ParamRange& r : paramRanges_)
        if (r.node == node) {
            if (index >= r.count) throw std::out_of_range("audio program: parameter index out of range");
            return params_[r.first + index];
        }
    throw std::runtime_error("audio program: node is not part of the compiled chain");
}

void AudioProgram::process(float* const* out) {
    for (size_t c = 0; c < channels_.size(); ++c) {
        if (restore_[c]) std::memcpy(channels_[c], restore_[c], paddedBytes_);
        else std::memset(channels_[c], 0, paddedBytes_);
    }
    kernel_->run(channels_.data(), params_.data(), blockSize_);
    if (!out) return;
    for (size_t i = 0; i < outputs_.size(); ++i)
        if (out[i]) std::memcpy(out[i], outputs_[i], blockSize_ * sizeof(float));
}

}  // namespace jitnodes
