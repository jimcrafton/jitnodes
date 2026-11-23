#include "jitnodes/geo/exrFile.h"

#include <algorithm>
#include <cstring>
#include <numeric>
#include <stdexcept>

// miniz and tinyexr are third-party code vendored under 3rdparty/tinyexr. miniz keeps its zlib-compatible names off so it cannot clash with
// the zlib an application links.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 1
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "miniz.c"
#include "tinyexr.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace jitnodes {

namespace {

[[noreturn]] void fail(const std::string& what, const char* detail) {
    std::string message = what;
    if (detail != nullptr) {
        message += ": ";
        message += detail;
        FreeEXRErrorMessage(detail);
    }
    throw std::runtime_error(message);
}

}  // namespace

const ExrChannel* ExrImage::find(const std::string& name) const {
    for (const ExrChannel& channel : channels) {
        if (channel.name == name) {
            return &channel;
        }
    }
    return nullptr;
}

ExrImage ExrImage::fromFloatImage(const FloatImage& image, bool half) {
    ExrImage out;
    out.width = image.width;
    out.height = image.height;
    const size_t count = static_cast<size_t>(std::max(image.width, 0)) * static_cast<size_t>(std::max(image.height, 0));
    const char* names[4] = {"R", "G", "B", "A"};
    for (size_t c = 0; c < 4; ++c) {
        ExrChannel channel;
        channel.name = names[c];
        channel.type = half ? ExrSampleType::float16 : ExrSampleType::float32;
        channel.values.resize(count);
        for (size_t i = 0; i < count && i * 4 + c < image.rgba.size(); ++i) {
            channel.values[i] = image.rgba[i * 4 + c];
        }
        out.channels.push_back(std::move(channel));
    }
    return out;
}

FloatImage ExrImage::toFloatImage() const {
    const ExrChannel* r = find("R");
    const ExrChannel* g = find("G");
    const ExrChannel* b = find("B");
    const ExrChannel* a = find("A");
    if (r == nullptr || g == nullptr || b == nullptr) {
        throw std::runtime_error("the EXR picture has no R, G and B channels");
    }
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    const auto sample = [&](const ExrChannel* channel, size_t i) {
        return channel->type == ExrSampleType::uint32 ? static_cast<float>(channel->ids[i]) : channel->values[i];
    };
    FloatImage out;
    out.width = width;
    out.height = height;
    out.linear = true;
    out.rgba.resize(count * 4u);
    for (size_t i = 0; i < count; ++i) {
        out.rgba[i * 4 + 0] = sample(r, i);
        out.rgba[i * 4 + 1] = sample(g, i);
        out.rgba[i * 4 + 2] = sample(b, i);
        out.rgba[i * 4 + 3] = a != nullptr ? sample(a, i) : 1.0f;
    }
    return out;
}

void ExrImage::write(const std::string& path, bool compress) const {
    if (width <= 0 || height <= 0 || channels.empty()) {
        throw std::runtime_error("cannot write an empty EXR picture");
    }
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (const ExrChannel& channel : channels) {
        const size_t have = channel.type == ExrSampleType::uint32 ? channel.ids.size() : channel.values.size();
        if (channel.name.empty() || channel.name.size() > 255 || have != count) {
            throw std::runtime_error("EXR channel '" + channel.name + "' has no name or the wrong number of samples");
        }
    }

    std::vector<size_t> order(channels.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return channels[a].name < channels[b].name; });

    const int n = static_cast<int>(channels.size());
    std::vector<EXRChannelInfo> infos(channels.size());
    std::vector<int> pixelTypes(channels.size());
    std::vector<int> requestedTypes(channels.size());
    std::vector<unsigned char*> pointers(channels.size());
    for (size_t slot = 0; slot < order.size(); ++slot) {
        const ExrChannel& channel = channels[order[slot]];
        std::memset(&infos[slot], 0, sizeof(EXRChannelInfo));
        std::memcpy(infos[slot].name, channel.name.c_str(), channel.name.size());
        infos[slot].x_sampling = 1;
        infos[slot].y_sampling = 1;
        if (channel.type == ExrSampleType::uint32) {
            pixelTypes[slot] = TINYEXR_PIXELTYPE_UINT;
            requestedTypes[slot] = TINYEXR_PIXELTYPE_UINT;
            pointers[slot] = reinterpret_cast<unsigned char*>(const_cast<uint32_t*>(channel.ids.data()));
        } else {
            pixelTypes[slot] = TINYEXR_PIXELTYPE_FLOAT;
            requestedTypes[slot] = channel.type == ExrSampleType::float16 ? TINYEXR_PIXELTYPE_HALF : TINYEXR_PIXELTYPE_FLOAT;
            pointers[slot] = reinterpret_cast<unsigned char*>(const_cast<float*>(channel.values.data()));
        }
    }

    EXRHeader header;
    InitEXRHeader(&header);
    header.num_channels = n;
    header.channels = infos.data();
    header.pixel_types = pixelTypes.data();
    header.requested_pixel_types = requestedTypes.data();
    header.compression_type = compress ? TINYEXR_COMPRESSIONTYPE_ZIP : TINYEXR_COMPRESSIONTYPE_NONE;

    EXRImage image;
    InitEXRImage(&image);
    image.num_channels = n;
    image.width = width;
    image.height = height;
    image.images = pointers.data();

    const char* error = nullptr;
    if (SaveEXRImageToFile(&image, &header, path.c_str(), &error) != TINYEXR_SUCCESS) {
        fail("cannot write '" + path + "'", error);
    }
}

ExrImage ExrImage::read(const std::string& path) {
    EXRVersion version;
    const char* error = nullptr;
    if (ParseEXRVersionFromFile(&version, path.c_str()) != TINYEXR_SUCCESS) {
        throw std::runtime_error("cannot read '" + path + "': not an EXR file");
    }
    if (version.multipart || version.non_image || version.tiled) {
        throw std::runtime_error("cannot read '" + path + "': tiled, deep and multipart EXR files are not supported");
    }

    EXRHeader header;
    InitEXRHeader(&header);
    if (ParseEXRHeaderFromFile(&header, &version, path.c_str(), &error) != TINYEXR_SUCCESS) {
        fail("cannot read '" + path + "'", error);
    }
    // Half channels are widened to float on load; uint channels stay as they are.
    std::vector<ExrSampleType> storedAs(static_cast<size_t>(header.num_channels));
    for (int c = 0; c < header.num_channels; ++c) {
        const int stored = header.pixel_types[c];
        storedAs[static_cast<size_t>(c)] = stored == TINYEXR_PIXELTYPE_UINT ? ExrSampleType::uint32
                                           : stored == TINYEXR_PIXELTYPE_HALF ? ExrSampleType::float16
                                                                              : ExrSampleType::float32;
        if (stored != TINYEXR_PIXELTYPE_UINT) {
            header.requested_pixel_types[c] = TINYEXR_PIXELTYPE_FLOAT;
        }
    }

    EXRImage image;
    InitEXRImage(&image);
    if (LoadEXRImageFromFile(&image, &header, path.c_str(), &error) != TINYEXR_SUCCESS) {
        FreeEXRHeader(&header);
        fail("cannot read '" + path + "'", error);
    }

    ExrImage out;
    out.width = image.width;
    out.height = image.height;
    const size_t count = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    for (int c = 0; c < header.num_channels; ++c) {
        ExrChannel channel;
        channel.name = header.channels[c].name;
        channel.type = storedAs[static_cast<size_t>(c)];
        if (image.images == nullptr || image.images[c] == nullptr) {
            FreeEXRImage(&image);
            FreeEXRHeader(&header);
            throw std::runtime_error("cannot read '" + path + "': the picture has no scanline data");
        }
        if (channel.type == ExrSampleType::uint32) {
            channel.ids.resize(count);
            std::memcpy(channel.ids.data(), image.images[c], count * sizeof(uint32_t));
        } else {
            channel.values.resize(count);
            std::memcpy(channel.values.data(), image.images[c], count * sizeof(float));
        }
        out.channels.push_back(std::move(channel));
    }
    FreeEXRImage(&image);
    FreeEXRHeader(&header);
    return out;
}

}  // namespace jitnodes
