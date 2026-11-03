#pragma once

#include <asmjit/x86.h>

#include <cstdint>
#include <vector>

#include "jitnodes/jit/expr.h"
#include "jitnodes/jit/isa.h"

namespace jitnodes {

// Emits the kernel IR for one instruction-set tier. Every method returns its result in a fresh
// virtual register, so callers work in SSA style and the register allocator does the rest.
// Constants used by emitted code (including the math polynomials) go into `pool`, which the
// generated function reads through `poolBase`.
class VecEmitter {
public:
    VecEmitter(asmjit::x86::Compiler& cc, Isa isa, std::vector<float>& pool, const asmjit::x86::Gp& poolBase);

    Isa isa() const { return isa_; }
    size_t lanes() const { return isaLanes(isa_); }

    asmjit::x86::Vec newVec(const char* name = "v");
    asmjit::x86::Vec broadcast(const asmjit::x86::Mem& scalar);
    asmjit::x86::Vec konst(float value);
    asmjit::x86::Vec konstBits(uint32_t bits);

    void load(const asmjit::x86::Vec& dst, const asmjit::x86::Mem& mem);
    void store(const asmjit::x86::Mem& mem, const asmjit::x86::Vec& v);
    void loadInt(const asmjit::x86::Vec& dst, const asmjit::x86::Mem& mem);  // int32 lanes -> float
    void storeInt(const asmjit::x86::Mem& mem, const asmjit::x86::Vec& v);   // float -> int32 (truncating)

    // Lane positions 0..lanes-1 as floats, and the in-place step that moves them on to the next iteration.
    asmjit::x86::Vec laneIndices();
    void advance(const asmjit::x86::Vec& v, const asmjit::x86::Vec& step);

    // Computational IR ops (everything except constant/param/load).
    asmjit::x86::Vec op(Op op, const asmjit::x86::Vec& a, const asmjit::x86::Vec& b, const asmjit::x86::Vec& c);

private:
    using Vec = asmjit::x86::Vec;
    enum class Pred { eq, lt, le, gt, ge };

    bool vex() const { return isa_ != Isa::sse2; }
    Vec zero();
    Vec bin(asmjit::InstId sse, asmjit::InstId avx, const Vec& a, const Vec& b);
    Vec un(asmjit::InstId sse, asmjit::InstId avx, const Vec& a);
    Vec shift(asmjit::InstId sse, asmjit::InstId avx, const Vec& a, int count);
    Vec add(const Vec& a, const Vec& b);
    Vec sub(const Vec& a, const Vec& b);
    Vec mul(const Vec& a, const Vec& b);
    Vec minv(const Vec& a, const Vec& b);
    Vec maxv(const Vec& a, const Vec& b);
    Vec vand(const Vec& a, const Vec& b);
    Vec vor(const Vec& a, const Vec& b);
    Vec vxor(const Vec& a, const Vec& b);
    Vec vandn(const Vec& a, const Vec& b);  // ~a & b
    Vec paddd(const Vec& a, const Vec& b);
    Vec psubd(const Vec& a, const Vec& b);
    Vec cvtt(const Vec& a);  // float -> int32 (truncate)
    Vec cvt(const Vec& a);   // int32 -> float
    Vec cmp(Pred p, const Vec& a, const Vec& b);
    Vec blend(const Vec& mask, const Vec& whenTrue, const Vec& whenFalse);
    Vec floorv(const Vec& a);
    Vec ceilv(const Vec& a);

    Vec expv(const Vec& x);
    Vec logv(const Vec& x);
    Vec sincos(const Vec& x, bool cosine);

    asmjit::x86::Compiler& cc_;
    Isa isa_;
    std::vector<float>& pool_;
    asmjit::x86::Gp poolBase_;
};

}  // namespace jitnodes
