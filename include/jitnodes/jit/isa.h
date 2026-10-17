#pragma once

#include <cstddef>

namespace jitnodes {

// Instruction-set tiers the JIT can target on x86-64.
enum class Isa { sse2, avx2, avx512 };

inline const char* isaName(Isa isa) {
    switch (isa) {
        case Isa::sse2: return "sse2";
        case Isa::avx2: return "avx2";
        case Isa::avx512: return "avx512";
    }
    return "?";
}

inline size_t isaLanes(Isa isa) {
    switch (isa) {
        case Isa::sse2: return 4;
        case Isa::avx2: return 8;
        case Isa::avx512: return 16;
    }
    return 1;
}

}  // namespace jitnodes
