#pragma once

#include <cmath>
#include <cstdint>

#include "nodeUtil.h"

namespace jitnodes {

inline float noiseFade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

inline float noiseGradient(uint32_t h, float x, float y, float z) {
    const uint32_t k = h & 15u;
    const float u = k < 8 ? x : y;
    const float v = k < 4 ? y : (k == 12 || k == 14 ? x : z);
    return ((k & 1u) ? -u : u) + ((k & 2u) ? -v : v);
}

// Improved Perlin noise in roughly [-1, 1]; the lattice is hashed with the seed, so there is no table to share between threads.
inline float perlin(float x, float y, float z, uint32_t seed) {
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int xi = static_cast<int>(fx), yi = static_cast<int>(fy), zi = static_cast<int>(fz);
    x -= fx;
    y -= fy;
    z -= fz;
    const float u = noiseFade(x), v = noiseFade(y), w = noiseFade(z);
    auto corner = [&](int dx, int dy, int dz) {
        const uint32_t h = hash32(seed, static_cast<uint32_t>(xi + dx) * 0x1B873593u ^ static_cast<uint32_t>(yi + dy) * 0xCC9E2D51u, static_cast<uint32_t>(zi + dz));
        return noiseGradient(h >> 7, x - static_cast<float>(dx), y - static_cast<float>(dy), z - static_cast<float>(dz));
    };
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    return lerp(lerp(lerp(corner(0, 0, 0), corner(1, 0, 0), u), lerp(corner(0, 1, 0), corner(1, 1, 0), u), v),
                lerp(lerp(corner(0, 0, 1), corner(1, 0, 1), u), lerp(corner(0, 1, 1), corner(1, 1, 1), u), v), w);
}

enum class FractalMode { fbm, ridged, turbulence };

struct Fractal {
    FractalMode mode = FractalMode::fbm;
    int octaves = 5;
    float lacunarity = 2.0f;
    float gain = 0.5f;
    uint32_t seed = 1;

    // fbm: signed, about [-0.8, 0.8]; ridged: [0, 1] with bright creases; turbulence: [0, 0.7].
    float sample(float x, float y, float z) const {
        float sum = 0.0f, amp = 1.0f, norm = 0.0f;
        for (int o = 0; o < octaves; ++o) {
            const float n = perlin(x, y, z, seed + static_cast<uint32_t>(o) * 0x9E3779B1u);
            float v = n;
            if (mode == FractalMode::ridged) {
                v = 1.0f - std::fabs(n);
                v *= v;
            } else if (mode == FractalMode::turbulence) {
                v = std::fabs(n);
            }
            sum += amp * v;
            norm += amp;
            amp *= gain;
            x *= lacunarity;
            y *= lacunarity;
            z *= lacunarity;
        }
        return norm > 0.0f ? sum / norm : 0.0f;
    }
};

// The curl of a scalar fractal potential at (x, y): a flow that neither compresses nor stretches. `eps` is a fraction of a noise wavelength.
inline void curlOf(const Fractal& f, float x, float y, float z, float eps, float& outX, float& outY) {
    const float dx = (f.sample(x + eps, y, z) - f.sample(x - eps, y, z)) / (2.0f * eps);
    const float dy = (f.sample(x, y + eps, z) - f.sample(x, y - eps, z)) / (2.0f * eps);
    outX = dy;
    outY = -dx;
}

}  // namespace jitnodes
