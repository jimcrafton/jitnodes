#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "jitnodes/audio/audioProgram.h"
#include "jitnodes/audio/audioStep.h"
#include "jitnodes/graph/network.h"

namespace jitnodes {

namespace {

constexpr size_t delayHeaderBytes = 16;

// Keeps the most recent samples of a ring (write index, then samples), oldest first from the start of the new ring.
void migrateRing(void* dst, size_t dstBytes, const void* src, size_t srcBytes) {
    if (srcBytes < delayHeaderBytes + sizeof(float) || dstBytes < delayHeaderBytes + sizeof(float)) return;
    const size_t oldSize = (srcBytes - delayHeaderBytes) / sizeof(float);
    const size_t newSize = (dstBytes - delayHeaderBytes) / sizeof(float);
    const size_t keep = std::min(oldSize, newSize);
    const size_t oldWrite = *static_cast<const uint32_t*>(src) % oldSize;
    const float* from = reinterpret_cast<const float*>(static_cast<const char*>(src) + delayHeaderBytes);
    float* to = reinterpret_cast<float*>(static_cast<char*>(dst) + delayHeaderBytes);
    for (size_t age = 1; age <= keep; ++age) to[keep - age] = from[(oldWrite + oldSize - age) % oldSize];
    *static_cast<uint32_t*>(dst) = static_cast<uint32_t>(keep % newSize);
}

// State: the write index, then a ring of maxSamples + 1 samples, so a delay of maxSamples still reads a sample that is not yet overwritten.
class DelayStep : public AudioStep {
public:
    explicit DelayStep(size_t maxSamples) : ringSize_(maxSamples + 1) {}

    size_t stateBytes() const override { return delayHeaderBytes + ringSize_ * sizeof(float); }

    void process(float* x, size_t count, const float* params, void* state) const override {
        uint32_t& write = *static_cast<uint32_t*>(state);
        float* ring = reinterpret_cast<float*>(static_cast<char*>(state) + delayHeaderBytes);
        const float requested = std::isnan(params[0]) ? 0.0f : std::floor(params[0] + 0.5f);
        const size_t delay = static_cast<size_t>(std::clamp(requested, 0.0f, static_cast<float>(ringSize_ - 1)));
        size_t w = write;
        for (size_t i = 0; i < count; ++i) {
            ring[w] = x[i];
            x[i] = ring[w >= delay ? w - delay : w + ringSize_ - delay];
            if (++w == ringSize_) w = 0;
        }
        write = static_cast<uint32_t>(w);
    }

    void migrate(void* dst, size_t dstBytes, const void* src, size_t srcBytes) const override { migrateRing(dst, dstBytes, src, srcBytes); }

private:
    size_t ringSize_;
};

// y[n] = sum c[k] * x[n - k]. State: the write index, then a ring of the last taps inputs. The coefficients are fixed per program.
class FirStep : public AudioStep {
public:
    explicit FirStep(std::vector<float> taps) : taps_(std::move(taps)) {}

    size_t stateBytes() const override { return delayHeaderBytes + taps_.size() * sizeof(float); }

    void process(float* x, size_t count, const float*, void* state) const override {
        uint32_t& write = *static_cast<uint32_t*>(state);
        float* ring = reinterpret_cast<float*>(static_cast<char*>(state) + delayHeaderBytes);
        const size_t n = taps_.size();
        const float* c = taps_.data();
        size_t w = write;
        for (size_t i = 0; i < count; ++i) {
            ring[w] = x[i];
            // Newest sample first: ring[w], ring[w - 1], ... wrapping to the end of the ring.
            float acc = 0.0f;
            for (size_t k = 0; k <= w; ++k) acc += c[k] * ring[w - k];
            for (size_t k = w + 1; k < n; ++k) acc += c[k] * ring[w + n - k];
            x[i] = acc;
            if (++w == n) w = 0;
        }
        write = static_cast<uint32_t>(w);
    }

    void migrate(void* dst, size_t dstBytes, const void* src, size_t srcBytes) const override { migrateRing(dst, dstBytes, src, srcBytes); }

private:
    std::vector<float> taps_;
};

std::vector<float> parseTaps(const std::string& text) {
    constexpr size_t maxTaps = 4096;
    std::vector<float> taps;
    const char* p = text.c_str();
    while (*p) {
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p) throw std::invalid_argument("fir: bad coefficient list '" + text + "'");
        taps.push_back(v);
        p = end;
        while (*p == ',' || std::isspace(static_cast<unsigned char>(*p))) ++p;
    }
    if (taps.empty() || taps.size() > maxTaps) throw std::invalid_argument("fir: needs 1 to 4096 coefficients");
    return taps;
}

// Transposed direct form II; the coefficients are normalised by a0 already.
class BiquadStep : public AudioStep {
public:
    size_t stateBytes() const override { return 2 * sizeof(float); }

    void process(float* x, size_t count, const float* p, void* state) const override {
        float* z = static_cast<float*>(state);
        const float b0 = p[0], b1 = p[1], b2 = p[2], a1 = p[3], a2 = p[4];
        float z1 = z[0], z2 = z[1];
        for (size_t i = 0; i < count; ++i) {
            const float in = x[i];
            const float out = b0 * in + z1;
            z1 = b1 * in - a1 * out + z2;
            z2 = b2 * in - a2 * out;
            x[i] = out;
        }
        // A decaying filter would otherwise spend its tail in denormals.
        constexpr float tiny = 1e-20f;
        z[0] = std::fabs(z1) < tiny ? 0.0f : z1;
        z[1] = std::fabs(z2) < tiny ? 0.0f : z2;
    }
};

class AudioStepNode : public Node {
public:
    using Node::Node;

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        std::vector<float> params;
        const std::unique_ptr<AudioStep> step = audioStep(params);
        StateBlock state(step->stateBytes());
        auto geo = std::make_shared<Geometry>(*in[0]);
        step->process(geo->x(), geo->pointCount(), params.data(), state.data());
        return geo;
    }
};

class DelayNode : public AudioStepNode {
public:
    DelayNode() : AudioStepNode("delay", 1) {
        declareFloat("samples", 0.0f);
        declareFloat("maxSamples", 4800.0f);
    }

    // maxSamples sizes the ring when the program is compiled; `samples` is clamped to it and can change every block.
    std::unique_ptr<AudioStep> audioStep(std::vector<float>& params) override {
        params = {getFloat("samples")};
        return std::make_unique<DelayStep>(static_cast<size_t>(std::clamp(getFloat("maxSamples"), 0.0f, 48000.0f * 60.0f)));
    }
};

class BiquadNode : public AudioStepNode {
public:
    BiquadNode() : AudioStepNode("biquad", 1) {
        declareFloat("b0", 1.0f);
        declareFloat("b1", 0.0f);
        declareFloat("b2", 0.0f);
        declareFloat("a1", 0.0f);
        declareFloat("a2", 0.0f);
    }

    std::unique_ptr<AudioStep> audioStep(std::vector<float>& params) override {
        params = {getFloat("b0"), getFloat("b1"), getFloat("b2"), getFloat("a1"), getFloat("a2")};
        return std::make_unique<BiquadStep>();
    }
};

class FirNode : public AudioStepNode {
public:
    FirNode() : AudioStepNode("fir", 1) { declareString("coeffs", "1"); }

    // Comma-separated, c0 first (applied to the newest sample). Read when the program is compiled; recompile and migrateState to change them.
    std::unique_ptr<AudioStep> audioStep(std::vector<float>& params) override {
        params.clear();
        return std::make_unique<FirStep>(parseTaps(getString("coeffs")));
    }
};

class PassStep : public AudioStep {
public:
    size_t stateBytes() const override { return 0; }
    void process(float*, size_t, const float*, void*) const override {}
};

// The program taps the block at this node (see AudioProgram); the signal itself passes through unchanged.
class FbSinkNode : public AudioStepNode {
public:
    FbSinkNode() : AudioStepNode("fbSink", 1) { declareString("name", "fb"); }

    std::unique_ptr<AudioStep> audioStep(std::vector<float>& params) override {
        params.clear();
        return std::make_unique<PassStep>();
    }
};

// Silence when cooked; an AudioProgram reads the matching fbSink's previous block instead.
class FbSourceNode : public Node {
public:
    FbSourceNode() : Node("fbSource", 0) {
        declareFloat("count", 128.0f);
        declareString("name", "fb");
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>&) override {
        auto geo = std::make_shared<Geometry>();
        geo->resizePoints(static_cast<size_t>(std::max(getFloat("count"), 0.0f)));
        return geo;
    }
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerAudioNodes() {
    add<DelayNode>("delay");
    add<BiquadNode>("biquad");
    add<FirNode>("fir");
    add<FbSinkNode>("fbSink");
    add<FbSourceNode>("fbSource");
}

}  // namespace jitnodes
