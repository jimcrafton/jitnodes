#include "vecEmitter.h"

#include <bit>
#include <cstring>
#include <stdexcept>

namespace jitnodes {

using namespace asmjit;
namespace Inst = asmjit::x86::Inst;

VecEmitter::VecEmitter(x86::Compiler& cc, Isa isa, std::vector<float>& pool, const x86::Gp& poolBase)
    : cc_(cc), isa_(isa), pool_(pool), poolBase_(poolBase) {}

x86::Vec VecEmitter::newVec(const char* name) {
    switch (isa_) {
        case Isa::sse2: return cc_.new_xmm_ps(name);
        case Isa::avx2: return cc_.new_ymm_ps(name);
        case Isa::avx512: return cc_.new_zmm_ps(name);
    }
    throw std::logic_error("bad isa");
}

x86::Vec VecEmitter::broadcast(const x86::Mem& scalar) {
    Vec d = newVec("bc");
    if (vex()) {
        cc_.vbroadcastss(d, scalar);
    } else {
        cc_.movss(d, scalar);
        cc_.shufps(d, d, imm(0));
    }
    return d;
}

x86::Vec VecEmitter::konstBits(uint32_t bits) {
    const float f = std::bit_cast<float>(bits);
    size_t slot = pool_.size();
    for (size_t i = 0; i < pool_.size(); ++i) {
        if (std::bit_cast<uint32_t>(pool_[i]) == bits) {
            slot = i;
            break;
        }
    }
    if (slot == pool_.size()) pool_.push_back(f);
    return broadcast(x86::dword_ptr(poolBase_, static_cast<int32_t>(slot * sizeof(float))));
}

x86::Vec VecEmitter::konst(float value) { return konstBits(std::bit_cast<uint32_t>(value)); }

void VecEmitter::load(const Vec& dst, const x86::Mem& mem) {
    if (vex()) cc_.vmovaps(dst, mem);
    else cc_.movaps(dst, mem);
}

void VecEmitter::store(const x86::Mem& mem, const Vec& v) {
    if (vex()) cc_.vmovaps(mem, v);
    else cc_.movaps(mem, v);
}

void VecEmitter::loadInt(const Vec& dst, const x86::Mem& mem) {
    switch (isa_) {
        case Isa::sse2: cc_.movdqa(dst, mem); cc_.cvtdq2ps(dst, dst); break;
        case Isa::avx2: cc_.vmovdqa(dst, mem); cc_.vcvtdq2ps(dst, dst); break;
        case Isa::avx512: cc_.vmovdqa32(dst, mem); cc_.vcvtdq2ps(dst, dst); break;
    }
}

void VecEmitter::storeInt(const x86::Mem& mem, const Vec& v) {
    Vec t = cvtt(v);
    switch (isa_) {
        case Isa::sse2: cc_.movdqa(mem, t); break;
        case Isa::avx2: cc_.vmovdqa(mem, t); break;
        case Isa::avx512: cc_.vmovdqa32(mem, t); break;
    }
}

x86::Vec VecEmitter::zero() {
    Vec d = newVec("zero");
    if (vex()) cc_.vxorps(d, d, d);
    else cc_.xorps(d, d);
    return d;
}

x86::Vec VecEmitter::bin(InstId sse, InstId avx, const Vec& a, const Vec& b) {
    Vec d = newVec();
    if (vex()) {
        cc_.emit(avx, d, a, b);
    } else {
        cc_.movaps(d, a);
        cc_.emit(sse, d, b);
    }
    return d;
}

x86::Vec VecEmitter::un(InstId sse, InstId avx, const Vec& a) {
    Vec d = newVec();
    cc_.emit(vex() ? avx : sse, d, a);
    return d;
}

x86::Vec VecEmitter::shift(InstId sse, InstId avx, const Vec& a, int count) {
    Vec d = newVec();
    if (vex()) {
        cc_.emit(avx, d, a, imm(count));
    } else {
        cc_.movaps(d, a);
        cc_.emit(sse, d, imm(count));
    }
    return d;
}

x86::Vec VecEmitter::add(const Vec& a, const Vec& b) { return bin(Inst::kIdAddps, Inst::kIdVaddps, a, b); }
x86::Vec VecEmitter::sub(const Vec& a, const Vec& b) { return bin(Inst::kIdSubps, Inst::kIdVsubps, a, b); }
x86::Vec VecEmitter::mul(const Vec& a, const Vec& b) { return bin(Inst::kIdMulps, Inst::kIdVmulps, a, b); }
x86::Vec VecEmitter::minv(const Vec& a, const Vec& b) { return bin(Inst::kIdMinps, Inst::kIdVminps, a, b); }
x86::Vec VecEmitter::maxv(const Vec& a, const Vec& b) { return bin(Inst::kIdMaxps, Inst::kIdVmaxps, a, b); }
x86::Vec VecEmitter::vand(const Vec& a, const Vec& b) { return bin(Inst::kIdAndps, Inst::kIdVandps, a, b); }
x86::Vec VecEmitter::vor(const Vec& a, const Vec& b) { return bin(Inst::kIdOrps, Inst::kIdVorps, a, b); }
x86::Vec VecEmitter::vxor(const Vec& a, const Vec& b) { return bin(Inst::kIdXorps, Inst::kIdVxorps, a, b); }
x86::Vec VecEmitter::vandn(const Vec& a, const Vec& b) { return bin(Inst::kIdAndnps, Inst::kIdVandnps, a, b); }
x86::Vec VecEmitter::paddd(const Vec& a, const Vec& b) { return bin(Inst::kIdPaddd, Inst::kIdVpaddd, a, b); }
x86::Vec VecEmitter::psubd(const Vec& a, const Vec& b) { return bin(Inst::kIdPsubd, Inst::kIdVpsubd, a, b); }
x86::Vec VecEmitter::cvtt(const Vec& a) { return un(Inst::kIdCvttps2dq, Inst::kIdVcvttps2dq, a); }
x86::Vec VecEmitter::cvt(const Vec& a) { return un(Inst::kIdCvtdq2ps, Inst::kIdVcvtdq2ps, a); }

x86::Vec VecEmitter::cmp(Pred p, const Vec& a, const Vec& b) {
    Vec d = newVec("mask");
    if (isa_ == Isa::sse2) {
        const bool swap = p == Pred::gt || p == Pred::ge;
        const int predicate = p == Pred::eq ? 0 : (p == Pred::lt || p == Pred::gt) ? 1 : 2;
        cc_.movaps(d, swap ? b : a);
        cc_.cmpps(d, swap ? a : b, imm(predicate));
        return d;
    }
    const int predicate = p == Pred::eq ? 0 : p == Pred::lt ? 17 : p == Pred::le ? 18 : p == Pred::gt ? 30 : 29;
    if (isa_ == Isa::avx2) {
        cc_.vcmpps(d, a, b, imm(predicate));
    } else {
        x86::KReg k = cc_.new_k16("k");
        cc_.vcmpps(k, a, b, imm(predicate));
        cc_.vpmovm2d(d, k);
    }
    return d;
}

x86::Vec VecEmitter::blend(const Vec& mask, const Vec& whenTrue, const Vec& whenFalse) {
    Vec d = newVec("sel");
    switch (isa_) {
        case Isa::avx2: cc_.vblendvps(d, whenFalse, whenTrue, mask); break;
        case Isa::avx512:
            cc_.vmovaps(d, mask);
            cc_.vpternlogd(d, whenTrue, whenFalse, imm(0xCA));  // mask ? true : false
            break;
        case Isa::sse2: {
            Vec t = vand(mask, whenTrue);
            Vec f = vandn(mask, whenFalse);
            return vor(t, f);
        }
    }
    return d;
}

x86::Vec VecEmitter::floorv(const Vec& a) {
    if (isa_ == Isa::avx2) {
        Vec d = newVec();
        cc_.vroundps(d, a, imm(9));
        return d;
    }
    if (isa_ == Isa::avx512) {
        Vec d = newVec();
        cc_.vrndscaleps(d, a, imm(9));
        return d;
    }
    // SSE2 has no roundps: truncate through int32, correct downwards, keep values that are already integral.
    Vec small = cmp(Pred::lt, vand(a, konstBits(0x7fffffffu)), konst(8388608.0f));
    Vec t = cvt(cvtt(a));
    Vec fl = sub(t, vand(cmp(Pred::gt, t, a), konst(1.0f)));
    return blend(small, fl, a);
}

x86::Vec VecEmitter::ceilv(const Vec& a) {
    if (isa_ == Isa::avx2) {
        Vec d = newVec();
        cc_.vroundps(d, a, imm(10));
        return d;
    }
    if (isa_ == Isa::avx512) {
        Vec d = newVec();
        cc_.vrndscaleps(d, a, imm(10));
        return d;
    }
    Vec small = cmp(Pred::lt, vand(a, konstBits(0x7fffffffu)), konst(8388608.0f));
    Vec t = cvt(cvtt(a));
    Vec ce = add(t, vand(cmp(Pred::lt, t, a), konst(1.0f)));
    return blend(small, ce, a);
}

// The approximations below follow the Cephes single-precision routines (as popularised by sse_mathfun).
// Documented accuracy and limits are in docs/design.md.
x86::Vec VecEmitter::expv(const Vec& xIn) {
    Vec x = maxv(minv(xIn, konst(88.3762626647949f)), konst(-88.3762626647949f));
    Vec fx = floorv(add(mul(x, konst(1.44269504088896341f)), konst(0.5f)));
    x = sub(x, mul(fx, konst(0.693359375f)));
    x = sub(x, mul(fx, konst(-2.12194440e-4f)));
    Vec z = mul(x, x);
    Vec y = konst(1.9875691500E-4f);
    y = add(mul(y, x), konst(1.3981999507E-3f));
    y = add(mul(y, x), konst(8.3334519073E-3f));
    y = add(mul(y, x), konst(4.1665795894E-2f));
    y = add(mul(y, x), konst(1.6666665459E-1f));
    y = add(mul(y, x), konst(5.0000001201E-1f));
    y = add(add(mul(y, z), x), konst(1.0f));
    Vec n = shift(Inst::kIdPslld, Inst::kIdVpslld, paddd(cvtt(fx), konstBits(127)), 23);
    return mul(y, n);
}

x86::Vec VecEmitter::logv(const Vec& xIn) {
    Vec z0 = zero();
    Vec invalid = cmp(Pred::le, xIn, z0);
    Vec isZero = cmp(Pred::eq, xIn, z0);
    Vec x = maxv(xIn, konstBits(0x00800000u));
    Vec exponent = shift(Inst::kIdPsrld, Inst::kIdVpsrld, x, 23);
    x = vor(vand(x, konstBits(0x807fffffu)), konst(0.5f));
    Vec e = add(cvt(psubd(exponent, konstBits(0x7f))), konst(1.0f));
    Vec below = cmp(Pred::lt, x, konst(0.707106781186547524f));
    Vec tmp = vand(x, below);
    x = sub(x, konst(1.0f));
    e = sub(e, vand(konst(1.0f), below));
    x = add(x, tmp);
    Vec z = mul(x, x);
    Vec y = konst(7.0376836292E-2f);
    y = add(mul(y, x), konst(-1.1514610310E-1f));
    y = add(mul(y, x), konst(1.1676998740E-1f));
    y = add(mul(y, x), konst(-1.2420140846E-1f));
    y = add(mul(y, x), konst(1.4249322787E-1f));
    y = add(mul(y, x), konst(-1.6668057665E-1f));
    y = add(mul(y, x), konst(2.0000714765E-1f));
    y = add(mul(y, x), konst(-2.4999993993E-1f));
    y = add(mul(y, x), konst(3.3333331174E-1f));
    y = mul(mul(y, x), z);
    y = add(y, mul(e, konst(-2.12194440e-4f)));
    y = sub(y, mul(z, konst(0.5f)));
    x = add(add(x, y), mul(e, konst(0.693359375f)));
    Vec result = vor(x, invalid);  // x < 0 -> NaN (all ones)
    return blend(isZero, konstBits(0xff800000u), result);  // log(0) = -inf
}

x86::Vec VecEmitter::sincos(const Vec& xIn, bool cosine) {
    Vec signBit = cosine ? Vec() : vand(xIn, konstBits(0x80000000u));
    Vec x = vand(xIn, konstBits(0x7fffffffu));
    Vec j = vand(paddd(cvtt(mul(x, konst(1.27323954473516f))), konstBits(1)), konstBits(~1u));
    Vec y = cvt(j);
    Vec signXor;
    if (cosine) {
        j = psubd(j, konstBits(2));
        signXor = shift(Inst::kIdPslld, Inst::kIdVpslld, vandn(j, konstBits(4)), 29);
    } else {
        signXor = vxor(signBit, shift(Inst::kIdPslld, Inst::kIdVpslld, vand(j, konstBits(4)), 29));
    }
    // All ones where (j & 2) != 0, i.e. where the cosine polynomial applies.
    Vec useCos = shift(Inst::kIdPsrad, Inst::kIdVpsrad, shift(Inst::kIdPslld, Inst::kIdVpslld, vand(j, konstBits(2)), 30), 31);

    x = add(x, mul(y, konst(-0.78515625f)));
    x = add(x, mul(y, konst(-2.4187564849853515625e-4f)));
    x = add(x, mul(y, konst(-3.77489497744594108e-8f)));
    Vec z = mul(x, x);

    Vec yc = konst(2.443315711809948E-005f);
    yc = add(mul(yc, z), konst(-1.388731625493765E-003f));
    yc = add(mul(yc, z), konst(4.166664568298827E-002f));
    yc = mul(mul(yc, z), z);
    yc = add(sub(yc, mul(z, konst(0.5f))), konst(1.0f));

    Vec ys = konst(-1.9515295891E-4f);
    ys = add(mul(ys, z), konst(8.3321608736E-3f));
    ys = add(mul(ys, z), konst(-1.6666654611E-1f));
    ys = add(mul(mul(ys, z), x), x);

    return vxor(blend(useCos, yc, ys), signXor);
}

x86::Vec VecEmitter::op(Op o, const Vec& a, const Vec& b, const Vec& c) {
    switch (o) {
        case Op::add: return add(a, b);
        case Op::sub: return sub(a, b);
        case Op::mul: return mul(a, b);
        case Op::div: return bin(Inst::kIdDivps, Inst::kIdVdivps, a, b);
        case Op::min: return minv(a, b);
        case Op::max: return maxv(a, b);
        case Op::sqrt: return un(Inst::kIdSqrtps, Inst::kIdVsqrtps, a);
        case Op::neg: return sub(zero(), a);
        case Op::abs: return maxv(a, sub(zero(), a));
        case Op::lt: return cmp(Pred::lt, a, b);
        case Op::gt: return cmp(Pred::gt, a, b);
        case Op::le: return cmp(Pred::le, a, b);
        case Op::ge: return cmp(Pred::ge, a, b);
        case Op::eq: return cmp(Pred::eq, a, b);
        case Op::logicalAnd: return vand(a, b);
        case Op::logicalOr: return vor(a, b);
        case Op::logicalNot: {
            Vec z = zero();
            return vxor(a, cmp(Pred::eq, z, z));
        }
        case Op::select: return blend(a, b, c);
        case Op::floor: return floorv(a);
        case Op::ceil: return ceilv(a);
        case Op::exp: return expv(a);
        case Op::log: return logv(a);
        case Op::sin: return sincos(a, false);
        case Op::cos: return sincos(a, true);
        case Op::pow: return expv(mul(b, logv(a)));
        default: throw std::logic_error("VecEmitter: unexpected op");
    }
}

}  // namespace jitnodes
