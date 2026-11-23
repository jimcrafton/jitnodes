#include "jitnodes/geo/floatImage.h"

#include <algorithm>
#include <cmath>

namespace jitnodes {

float FloatImage::srgbToLinear(float value) {
    if (value <= 0.04045f) {
        return value / 12.92f;
    }
    return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float FloatImage::linearToSrgb(float value) {
    if (value <= 0.0031308f) {
        return value * 12.92f;
    }
    return 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

FloatImage FloatImage::fromArgb8(const Image& image) {
    FloatImage out;
    out.width = image.width;
    out.height = image.height;
    out.linear = false;
    out.rgba.resize(image.pixels.size() * 4u);
    for (size_t i = 0; i < image.pixels.size(); ++i) {
        const uint32_t px = image.pixels[i];
        out.rgba[i * 4 + 0] = static_cast<float>((px >> 16) & 255u) / 255.0f;
        out.rgba[i * 4 + 1] = static_cast<float>((px >> 8) & 255u) / 255.0f;
        out.rgba[i * 4 + 2] = static_cast<float>(px & 255u) / 255.0f;
        out.rgba[i * 4 + 3] = static_cast<float>((px >> 24) & 255u) / 255.0f;
    }
    return out;
}

Image FloatImage::toArgb8() const {
    const FloatImage encoded = asSrgb();
    Image out;
    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<size_t>(std::max(width, 0)) * static_cast<size_t>(std::max(height, 0)));
    const auto byteOf = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    for (size_t i = 0; i < out.pixels.size() && i * 4 + 3 < encoded.rgba.size(); ++i) {
        out.pixels[i] = (byteOf(encoded.rgba[i * 4 + 3]) << 24) | (byteOf(encoded.rgba[i * 4 + 0]) << 16) | (byteOf(encoded.rgba[i * 4 + 1]) << 8) |
                        byteOf(encoded.rgba[i * 4 + 2]);
    }
    return out;
}

FloatImage FloatImage::asLinear() const { return linear ? *this : converted(true); }

FloatImage FloatImage::asSrgb() const { return linear ? converted(false) : *this; }

FloatImage FloatImage::converted(bool toLinear) const {
    FloatImage out = *this;
    out.linear = toLinear;
    for (size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
        const float alpha = out.rgba[i + 3];
        if (alpha <= 0.0f) {
            continue;
        }
        for (size_t c = 0; c < 3; ++c) {
            const float straight = out.rgba[i + c] / alpha;
            const float sign = straight < 0.0f ? -1.0f : 1.0f;
            const float magnitude = std::fabs(straight);
            const float mapped = toLinear ? srgbToLinear(magnitude) : linearToSrgb(magnitude);
            out.rgba[i + c] = sign * mapped * alpha;
        }
    }
    return out;
}

}  // namespace jitnodes
