#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "jitnodes/geo/floatImage.h"

namespace jitnodes {

// How a channel is stored in the file. A float16 channel is held in memory as floats (reading widens it, writing rounds it to half).
enum class ExrSampleType { float32, float16, uint32 };

// One named channel of an EXR picture: "R", "G", "B", "A", "Z" (depth), "id" ... Float and half channels use `values`, a uint32 channel (an
// object id, which a float cannot hold exactly above 2^24) uses `ids`. Each has width * height entries, row 0 at the top.
struct ExrChannel {
    std::string name;
    ExrSampleType type = ExrSampleType::float32;
    std::vector<float> values;
    std::vector<uint32_t> ids;
};

// A single-part scanline EXR file: any number of named channels. Tiled, deep and multipart files are refused on read.
struct ExrImage {
    int width = 0;
    int height = 0;
    std::vector<ExrChannel> channels;

    const ExrChannel* find(const std::string& name) const;

    // An R, G, B, A picture from a FloatImage; `half` stores the channels as 16-bit floats instead of 32-bit.
    static ExrImage fromFloatImage(const FloatImage& image, bool half = false);
    // The R, G, B and (when present) A channels as a linear FloatImage; throws std::runtime_error when R, G or B is missing. A missing alpha is
    // 1. EXR colour is not premultiplied by the format, but the convention (and OpenEXR's) is that it is, so no conversion is made.
    FloatImage toFloatImage() const;

    // Throw std::runtime_error with the library's message on failure. Channels are written in alphabetical order, as the format requires.
    void write(const std::string& path, bool compress = true) const;
    static ExrImage read(const std::string& path);
};

}  // namespace jitnodes
