#pragma once

#include "jitnodes/core/bitCast.h"
#include <cmath>
#include <cstdint>

#include "jitnodes/jit/expr.h"

namespace jitnodes {

inline float maskBits(bool v) { return bitCast<float>(v ? 0xFFFFFFFFu : 0u); }
inline uint32_t bitsOf(float f) { return bitCast<uint32_t>(f); }

// Scalar semantics of every computational op; shared by the kernel interpreter and the parameter
// expression evaluator so both agree with the JIT.
inline float evalOp(Op op, float a, float b, float c) {
    switch (op) {
        case Op::add: return a + b;
        case Op::sub: return a - b;
        case Op::mul: return a * b;
        case Op::div: return a / b;
        case Op::min: return a < b ? a : b;
        case Op::max: return a > b ? a : b;
        case Op::sqrt: return std::sqrt(a);
        case Op::neg: return 0.0f - a;
        case Op::abs: {
            const float t = 0.0f - a;
            return a > t ? a : t;
        }
        case Op::lt: return maskBits(a < b);
        case Op::gt: return maskBits(a > b);
        case Op::le: return maskBits(a <= b);
        case Op::ge: return maskBits(a >= b);
        case Op::eq: return maskBits(a == b);
        case Op::logicalAnd: return bitCast<float>(bitsOf(a) & bitsOf(b));
        case Op::logicalOr: return bitCast<float>(bitsOf(a) | bitsOf(b));
        case Op::logicalNot: return bitCast<float>(~bitsOf(a));
        case Op::select: return (bitsOf(a) & 0x80000000u) ? b : c;
        case Op::sin: return std::sin(a);
        case Op::cos: return std::cos(a);
        case Op::exp: return std::exp(a);
        case Op::log: return std::log(a);
        case Op::pow: return std::pow(a, b);
        case Op::floor: return std::floor(a);
        case Op::ceil: return std::ceil(a);
        default: return 0.0f;
    }
}

}  // namespace jitnodes
