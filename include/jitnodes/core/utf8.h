#pragma once

#include <cstdint>
#include <string_view>

namespace jitnodes {

// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF.
inline bool isValidUtf8(std::string_view s) {
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const uint8_t c = p[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t extra;
        uint32_t code, least;
        if (c >= 0xc2 && c <= 0xdf) extra = 1, code = c & 0x1f, least = 0x80;
        else if (c >= 0xe0 && c <= 0xef) extra = 2, code = c & 0x0f, least = 0x800;
        else if (c >= 0xf0 && c <= 0xf4) extra = 3, code = c & 0x07, least = 0x10000;
        else return false;
        if (n - i <= extra) return false;
        for (size_t k = 1; k <= extra; ++k) {
            if ((p[i + k] & 0xc0) != 0x80) return false;
            code = (code << 6) | (p[i + k] & 0x3f);
        }
        if (code < least || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
        i += extra + 1;
    }
    return true;
}

}  // namespace jitnodes
