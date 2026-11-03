#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jitnodes {

// Mono, planar float audio at `sampleRate`, immutable once shared.
struct SampleBuffer {
    std::vector<float> frames;
    double sampleRate = 0.0;
};

// Named sample buffers for the `samplePlayer` node, whose string parameter names an entry. The node takes its shared_ptr when a program
// is compiled, so the audio thread never touches the bank. Thread-safe.
class SampleBank {
public:
    static SampleBank& instance() {
        static SampleBank bank;
        return bank;
    }

    void set(const std::string& name, std::shared_ptr<const SampleBuffer> buffer) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (buffer) samples_[name] = std::move(buffer);
        else samples_.erase(name);
    }

    std::shared_ptr<const SampleBuffer> get(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = samples_.find(name);
        return it == samples_.end() ? nullptr : it->second;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const SampleBuffer>> samples_;
};

}  // namespace jitnodes
