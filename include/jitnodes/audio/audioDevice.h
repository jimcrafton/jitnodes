#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jitnodes {

struct AudioDeviceConfig {
    double sampleRate = 48000.0;
    int bufferFrames = 0;       // 0 = the driver's preferred size
    int outputChannels = 2;     // clamped to what the driver offers
};

// Runs on the driver's realtime thread: no allocation, no locks. channels[c] is a zeroed planar
// float buffer of `frames` samples, to be filled with values in [-1, 1].
using AudioCallback = std::function<void(float* const* channels, int channelCount, int frames)>;

// One ASIO driver, output only. Only one device can be open at a time because ASIO callbacks carry no context.
class AsioDevice {
public:
    static std::vector<std::string> driverNames();

    AsioDevice();
    AsioDevice(const AsioDevice&) = delete;
    AsioDevice& operator=(const AsioDevice&) = delete;
    ~AsioDevice();

    bool open(const std::string& driverName, const AudioDeviceConfig& config);
    bool start(AudioCallback callback);
    void stop();
    void close();

    bool isOpen() const;
    bool isRunning() const;
    bool resetRequested() const;
    const std::string& error() const;

    double sampleRate() const;
    int bufferFrames() const;
    int outputChannels() const;
    double outputLatencyMs() const;
    unsigned long long dropoutCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace jitnodes
