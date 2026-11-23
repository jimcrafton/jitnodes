#pragma once

#include <vector>

#include "jitnodes/geo/raster.h"

namespace jitnodes {

// A picture of 32-bit float colour: four floats per pixel (red, green, blue, alpha), colour premultiplied by alpha, row 0 at the top. Values are
// not limited to 0..1, so it can hold what an 8-bit picture cannot (HDR light, values below zero).
//
// `linear` says how the colour is encoded: true is linear light (what an EXR file normally holds), false is display-encoded sRGB (what an 8-bit
// picture holds, so a float image made from one starts out false). asLinear and asSrgb convert; alpha is never changed by them.
struct FloatImage {
    int width = 0;
    int height = 0;
    bool linear = false;
    std::vector<float> rgba;

    bool empty() const { return width <= 0 || height <= 0 || rgba.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4u; }

    // Premultiplied 0xAARRGGBB pixels to floats in 0..1; the result is display-encoded (linear = false).
    static FloatImage fromArgb8(const Image& image);
    // Clamps to 0..1 and rounds to 8 bits per channel; colour is converted to sRGB first when the image is linear.
    Image toArgb8() const;
    // The same picture with its colour in the other encoding (a copy when it already is). The conversion works on straight colour, so a pixel
    // is divided by its alpha, converted and multiplied back.
    FloatImage asLinear() const;
    FloatImage asSrgb() const;

    static float srgbToLinear(float value);
    static float linearToSrgb(float value);

private:
    FloatImage converted(bool toLinear) const;
};

}  // namespace jitnodes
