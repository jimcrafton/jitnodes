#include "jitnodes/audio/audioProgram.h"

#include <cstring>
#include <stdexcept>

#include "jitnodes/jit/fuse.h"

namespace jitnodes {

AudioProgram::~AudioProgram() = default;

std::unique_ptr<AudioProgram> AudioProgram::compile(Network& net, NodeId output, const AudioProgramOptions& options) {
    if (options.blockSize == 0 || options.blockSize % simdLanes != 0)
        throw std::runtime_error("audio program: block size must be a multiple of " + std::to_string(simdLanes));
    if (options.outputs.empty()) throw std::runtime_error("audio program: no outputs requested");

    // Tail first, as in a cook task's chain.
    std::vector<NodeId> chain;
    NodeId cursor = output;
    while (net.node(cursor).isPointwise()) {
        chain.push_back(cursor);
        cursor = net.inputOf(cursor, 0);
        if (cursor == invalidNodeId) throw std::runtime_error("audio program: " + net.node(chain.back()).name() + " has no input connected");
    }
    if (chain.empty()) throw std::runtime_error("audio program: " + net.node(output).name() + " is not a point-wise node");
    const NodeId sourceId = cursor;

    std::unique_ptr<AudioProgram> p(new AudioProgram());
    p->blockSize_ = options.blockSize;
    p->source_ = *net.cook(sourceId);
    if (p->source_.pointCount() != options.blockSize)
        throw std::runtime_error("audio program: source " + net.node(sourceId).name() + " has " + std::to_string(p->source_.pointCount()) +
                                 " points, the block size is " + std::to_string(options.blockSize));

    auto channelType = [&](const std::string& name) {
        const AttribArray* a = p->source_.findAttrib(AttribClass::point, name);
        return a ? a->type() : AttribType::float32;
    };

    PointKernel fused;
    for (size_t i = chain.size(); i-- > 0;) {
        Node& node = net.node(chain[i]);
        if (node.attribClass() != AttribClass::point) throw std::runtime_error("audio program: " + node.name() + " is not a point node");
        PointKernel k;
        node.pointKernel(k);
        const size_t first = fused.params.size();
        if (i + 1 == chain.size()) fused = std::move(k);
        else {
            appendKernel(fused.spec, k.spec, channelType);
            fused.params.insert(fused.params.end(), k.params.begin(), k.params.end());
        }
        p->paramRanges_.push_back({chain[i], first, fused.params.size() - first});
    }

    p->work_ = p->source_;
    p->kernel_ = net.compiler().compileFor(fused.spec, p->work_, AttribClass::point);
    resolveStringParams(fused.spec, p->work_, AttribClass::point, fused.params);
    p->params_ = std::move(fused.params);

    // Every attribute is created and detached from source_ here, so the pointers stay valid and process() never allocates.
    const KernelSpec& spec = p->kernel_->spec();
    for (size_t c = 0; c < spec.channels.size(); ++c) {
        AttribArray& a = p->work_.attrib(AttribClass::point, spec.channels[c], spec.channelTypes[c]);
        p->channels_.push_back(a.data());
        const AttribArray* src = p->source_.findAttrib(AttribClass::point, spec.channels[c]);
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
