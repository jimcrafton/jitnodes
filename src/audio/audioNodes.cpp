#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "jitnodes/audio/audioProgram.h"
#include "jitnodes/audio/audioStep.h"
#include "jitnodes/audio/sampleBank.h"
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

// Linear ADSR with times in samples. `rate` is the per-sample drop of the current release.
enum class EnvStage : uint32_t { idle, attack, decay, sustain, release };

struct Envelope {
    float level = 0.0f;
    float rate = 0.0f;
    EnvStage stage = EnvStage::idle;

    void gateOn() { stage = EnvStage::attack; }
    void gateOff(float releaseSamples) {
        if (stage == EnvStage::idle || stage == EnvStage::release) return;
        rate = level / std::max(releaseSamples, 1.0f);
        stage = EnvStage::release;
    }

    float next(float attack, float decay, float sustain) {
        switch (stage) {
        case EnvStage::idle: break;
        case EnvStage::attack:
            level += 1.0f / std::max(attack, 1.0f);
            if (level >= 1.0f) {
                level = 1.0f;
                stage = EnvStage::decay;
            }
            break;
        case EnvStage::decay:
            level -= (1.0f - sustain) / std::max(decay, 1.0f);
            if (level <= sustain) {
                level = sustain;
                stage = EnvStage::sustain;
            }
            break;
        case EnvStage::sustain: level = sustain; break;
        case EnvStage::release:
            level -= rate;
            if (level <= 0.0f) {
                level = 0.0f;
                stage = EnvStage::idle;
            }
            break;
        }
        return level;
    }
};

// Calls render(from, to) for each stretch between events and apply(event) at the event's offset, so every event takes effect on its exact
// sample. Offsets past the block are clamped to its end.
template <class Render, class Apply>
void splitAtEvents(size_t count, const EventBuffer& events, Render render, Apply apply) {
    size_t pos = 0;
    for (const AudioEvent& e : events) {
        const size_t at = std::min<size_t>(e.offset, count);
        if (at > pos) render(pos, at);
        pos = std::max(pos, at);
        apply(e);
    }
    if (pos < count) render(pos, count);
}

// Gates x with an envelope: note on (any note, any channel) opens it, and it closes once every held note is released.
// Params: attack, decay, sustain, release (times in samples). State: Envelope, then a 128-bit held-note mask.
class AdsrStep : public AudioStep {
    struct State {
        Envelope env;
        uint32_t held[4];
    };

public:
    size_t stateBytes() const override { return sizeof(State); }

    void process(float* x, size_t count, const float* p, void* state) const override { processEvents(x, count, p, state, EventBuffer()); }

    void processEvents(float* x, size_t count, const float* p, void* state, const EventBuffer& events) const override {
        State& s = *static_cast<State*>(state);
        const float attack = p[0], decay = p[1], sustain = std::clamp(p[2], 0.0f, 1.0f), release = p[3];
        splitAtEvents(
            count, events,
            [&](size_t from, size_t to) {
                for (size_t i = from; i < to; ++i) x[i] *= s.env.next(attack, decay, sustain);
            },
            [&](const AudioEvent& e) {
                const uint32_t bit = 1u << (e.data1 & 31);
                uint32_t& word = s.held[(e.data1 & 127) >> 5];
                if (e.isNoteOn()) {
                    word |= bit;
                    s.env.gateOn();
                } else if (e.isNoteOff()) {
                    word &= ~bit;
                    if (!(s.held[0] | s.held[1] | s.held[2] | s.held[3])) s.env.gateOff(release);
                }
            });
    }
};

// A bank of polyphonic oscillator voices, each with its own envelope, driven by note events. The voices' sum is added to x.
// Params: attack, decay, sustain, release (samples), waveform (0 sine, 1 saw, 2 square, 3 triangle), sampleRate. A new note takes a free
// voice, else the quietest releasing one, else the oldest. State: one Voice per slot.
struct Voice {
    Envelope env;
    double phase = 0.0;
    double inc = 0.0;
    float velocity = 0.0f;
    uint32_t note = 0;
    bool held = false;
    uint64_t age = 0;
};

class VoicesStep : public AudioStep {
public:
    explicit VoicesStep(size_t voices) : voices_(voices) {}

    size_t stateBytes() const override { return sizeof(Voice) * voices_ + sizeof(uint64_t); }

    void process(float* x, size_t count, const float* p, void* state) const override { processEvents(x, count, p, state, EventBuffer()); }

    void processEvents(float* x, size_t count, const float* p, void* state, const EventBuffer& events) const override {
        Voice* voices = static_cast<Voice*>(state);
        uint64_t& clock = *reinterpret_cast<uint64_t*>(voices + voices_);
        const float attack = p[0], decay = p[1], sustain = std::clamp(p[2], 0.0f, 1.0f), release = p[3];
        const int wave = static_cast<int>(std::clamp(p[4], 0.0f, 3.0f) + 0.5f);
        const double sampleRate = p[5] > 0.0f ? p[5] : 48000.0;

        splitAtEvents(
            count, events,
            [&](size_t from, size_t to) {
                for (size_t v = 0; v < voices_; ++v) {
                    Voice& voice = voices[v];
                    if (voice.env.stage == EnvStage::idle) continue;
                    for (size_t i = from; i < to; ++i) {
                        const float level = voice.env.next(attack, decay, sustain);
                        x[i] += level * voice.velocity * shape(wave, voice.phase);
                        voice.phase += voice.inc;
                        if (voice.phase >= 1.0) voice.phase -= 1.0;
                        if (voice.env.stage == EnvStage::idle) break;
                    }
                }
            },
            [&](const AudioEvent& e) {
                if (e.isNoteOn()) {
                    Voice& voice = pick(voices, e.data1);
                    voice.phase = 0.0;
                    voice.note = e.data1;
                    voice.held = true;
                    voice.velocity = static_cast<float>(e.data2) / 127.0f;
                    voice.inc = 440.0 * std::exp2((static_cast<double>(e.data1) - 69.0) / 12.0) / sampleRate;
                    voice.age = ++clock;
                    voice.env.gateOn();
                } else if (e.isNoteOff()) {
                    for (size_t v = 0; v < voices_; ++v)
                        if (voices[v].held && voices[v].note == e.data1) {
                            voices[v].held = false;
                            voices[v].env.gateOff(release);
                        }
                }
            });
    }

    void migrate(void* dst, size_t dstBytes, const void* src, size_t srcBytes) const override {
        // The age clock sits after the voices, so it only carries over when the voice count is unchanged; otherwise the surviving
        // voices keep their ages and new notes still order after them because ages only compare within a pick.
        const size_t voiceBytes = std::min(dstBytes, srcBytes) / sizeof(Voice) * sizeof(Voice);
        std::memcpy(dst, src, dstBytes == srcBytes ? dstBytes : voiceBytes);
    }

private:
    static float shape(int wave, double phase) {
        const float t = static_cast<float>(phase);
        switch (wave) {
        case 1: return 2.0f * t - 1.0f;
        case 2: return t < 0.5f ? 1.0f : -1.0f;
        case 3: return 4.0f * std::fabs(t - 0.5f) - 1.0f;
        default: return std::sin(6.28318530718f * t);
        }
    }

    Voice& pick(Voice* voices, uint32_t note) const {
        Voice* best = nullptr;
        auto better = [&](Voice* candidate, int rank, int& bestRank) {
            if (!best || rank < bestRank || (rank == bestRank && candidate->age < best->age)) {
                best = candidate;
                bestRank = rank;
            }
        };
        int bestRank = 0;
        for (size_t v = 0; v < voices_; ++v) {
            Voice& c = voices[v];
            // 0: same note again, 1: free, 2: releasing, 3: playing
            const int rank = (c.held && c.note == note) ? 0 : c.env.stage == EnvStage::idle ? 1 : !c.held ? 2 : 3;
            better(&c, rank, bestRank);
        }
        return *best;
    }

    size_t voices_;
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

class AdsrNode : public AudioStepNode {
public:
    AdsrNode() : AudioStepNode("adsr", 1) {
        declareFloat("attack", 480.0f);
        declareFloat("decay", 4800.0f);
        declareFloat("sustain", 0.7f);
        declareFloat("release", 4800.0f);
    }

    std::unique_ptr<AudioStep> audioStep(std::vector<float>& params) override {
        params = {getFloat("attack"), getFloat("decay"), getFloat("sustain"), getFloat("release")};
        return std::make_unique<AdsrStep>();
    }
};

class VoicesNode : public AudioStepNode {
public:
    VoicesNode() : AudioStepNode("voices", 1) {
        declareFloat("attack", 480.0f);
        declareFloat("decay", 4800.0f);
        declareFloat("sustain", 0.7f);
        declareFloat("release", 4800.0f);
        declareFloat("waveform", 0.0f);
        declareFloat("sampleRate", 48000.0f);
        declareFloat("voiceCount", 8.0f);
    }

    // voiceCount sizes the state when the program is compiled; recompile and migrateState to change it.
    std::unique_ptr<AudioStep> audioStep(std::vector<float>& params) override {
        params = {getFloat("attack"), getFloat("decay"), getFloat("sustain"), getFloat("release"), getFloat("waveform"), getFloat("sampleRate")};
        return std::make_unique<VoicesStep>(static_cast<size_t>(std::clamp(getFloat("voiceCount"), 1.0f, 64.0f)));
    }
};

// Replaces x with a stretch of a shared buffer. State: the play position, relative to the start of the region.
class SamplePlayerStep : public AudioStep {
public:
    explicit SamplePlayerStep(std::shared_ptr<const SampleBuffer> buffer) : buffer_(std::move(buffer)) {}

    size_t stateBytes() const override { return sizeof(uint64_t); }

    void process(float* x, size_t count, const float* params, void* state) const override {
        const size_t total = buffer_->frames.size();
        const size_t start = static_cast<size_t>(std::clamp(std::isnan(params[0]) ? 0.0f : params[0], 0.0f, static_cast<float>(total)));
        const size_t available = total - start;
        const size_t length = params[1] > 0.0f ? std::min(static_cast<size_t>(params[1]), available) : available;
        const bool loop = params[2] != 0.0f;
        const float* data = buffer_->frames.data() + start;
        uint64_t& position = *static_cast<uint64_t*>(state);
        size_t pos = static_cast<size_t>(std::min<uint64_t>(position, length));
        size_t i = 0;
        while (i < count) {
            if (pos >= length) {
                if (!loop || length == 0) break;
                pos = 0;
            }
            const size_t n = std::min(count - i, length - pos);
            std::memcpy(x + i, data + pos, n * sizeof(float));
            i += n;
            pos += n;
        }
        if (i < count) std::memset(x + i, 0, (count - i) * sizeof(float));
        position = pos;
    }

private:
    std::shared_ptr<const SampleBuffer> buffer_;
};

class SamplePlayerNode : public AudioStepNode {
public:
    SamplePlayerNode() : AudioStepNode("samplePlayer", 1) {
        declareString("sample", "");
        declareFloat("start", 0.0f);
        declareFloat("length", 0.0f);
        declareFloat("loop", 0.0f);
    }

    // `sample` names a SampleBank entry (taken when the program is compiled); start and length are frames, length 0 meaning to the end.
    std::unique_ptr<AudioStep> audioStep(std::vector<float>& params) override {
        auto buffer = SampleBank::instance().get(getString("sample"));
        if (!buffer) throw std::invalid_argument("samplePlayer: no sample named '" + getString("sample") + "'");
        params = {getFloat("start"), getFloat("length"), getFloat("loop")};
        return std::make_unique<SamplePlayerStep>(std::move(buffer));
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
    add<AdsrNode>("adsr");
    add<VoicesNode>("voices");
    add<SamplePlayerNode>("samplePlayer");
    add<FbSinkNode>("fbSink");
    add<FbSourceNode>("fbSource");
}

}  // namespace jitnodes
