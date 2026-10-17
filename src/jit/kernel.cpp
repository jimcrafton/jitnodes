#include "jitnodes/jit/kernel.h"

#include <asmjit/x86.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

#include <algorithm>
#include <chrono>

#include "jitnodes/core/threadPool.h"
#include "jitnodes/jit/opEval.h"

namespace jitnodes {

struct JitBackend {
    asmjit::JitRuntime runtime;
    bool avx2 = false;
    JitBackend() { avx2 = runtime.arch() == asmjit::Arch::kX64 && runtime.cpu_features().x86().has_avx2(); }
};

namespace {

// vcmpps predicates (ordered, quiet).
constexpr uint32_t cmpEq = 0;
constexpr uint32_t cmpLt = 17;
constexpr uint32_t cmpLe = 18;
constexpr uint32_t cmpGe = 29;
constexpr uint32_t cmpGt = 30;
constexpr uint32_t cmpTrue = 15;  // TRUE_UQ: all ones, used to build the NOT mask

int32_t toInt32(float f) {
    return (f > -2147483648.0f && f < 2147483648.0f) ? static_cast<int32_t>(f) : INT32_MIN;
}

struct JitUnsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Marks expression nodes that influence a store; everything else is dead code and not emitted.
std::vector<bool> liveNodes(const KernelSpec& spec) {
    const auto& nodes = spec.graph.nodes();
    std::vector<bool> live(nodes.size(), false);
    for (const auto& s : spec.stores) live[static_cast<size_t>(s.second)] = true;
    for (size_t i = nodes.size(); i-- > 0;) {
        if (!live[i]) continue;
        for (int operand : {nodes[i].a, nodes[i].b, nodes[i].c})
            if (operand >= 0) live[static_cast<size_t>(operand)] = true;
    }
    return live;
}

}  // namespace

Kernel::Kernel(KernelSpec spec, std::shared_ptr<JitBackend> backend)
    : spec_(std::move(spec)), backend_(std::move(backend)) {}

Kernel::~Kernel() {
    if (fn_) backend_->runtime.release(fn_);
}

void Kernel::compileJit() {
    using namespace asmjit;
    const auto& nodes = spec_.graph.nodes();
    const std::vector<bool> live = liveNodes(spec_);

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!live[i]) continue;
        const Op op = nodes[i].op;
        if (op == Op::sin || op == Op::cos || op == Op::exp || op == Op::log || op == Op::pow)
            throw JitUnsupported("op has no JIT implementation");
    }

    constantSlot_.assign(nodes.size(), -1);
    bool needZero = false, needOnes = false;
    std::vector<bool> channelUsed(spec_.channels.size(), false);
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!live[i]) continue;
        const ExprNode& n = nodes[i];
        if (n.op == Op::constant) {
            constantSlot_[i] = static_cast<int>(constants_.size());
            constants_.push_back(n.value);
        }
        if (n.op == Op::load) channelUsed[static_cast<size_t>(n.index)] = true;
        if (n.op == Op::neg || n.op == Op::abs) needZero = true;
        if (n.op == Op::logicalNot) needZero = needOnes = true;
    }
    for (const auto& s : spec_.stores) channelUsed[static_cast<size_t>(s.first)] = true;

    CodeHolder code;
    Error err = code.init(backend_->runtime.environment(), backend_->runtime.cpu_features());
    if (err != Error::kOk) throw std::runtime_error("asmjit: CodeHolder init failed");
    FileLogger logger(stderr);
    if (std::getenv("JITNODES_JIT_LOG")) code.set_logger(&logger);
    x86::Compiler cc(&code);

    // void kernel(void* const* channels, const float* params, const float* consts, size_t count)
    FuncNode* func = cc.add_func(FuncSignature::build<void, void* const*, const float*, const float*, size_t>());
    func->frame().set_avx_enabled();
    func->frame().set_avx_cleanup();
    x86::Gp channelTable = cc.new_gp_ptr("channels");
    x86::Gp paramBlock = cc.new_gp_ptr("params");
    x86::Gp constPool = cc.new_gp_ptr("consts");
    x86::Gp count = cc.new_gp_ptr("count");
    func->set_arg(0, channelTable);
    func->set_arg(1, paramBlock);
    func->set_arg(2, constPool);
    func->set_arg(3, count);

    std::vector<x86::Gp> channelPtr(spec_.channels.size());
    for (size_t c = 0; c < channelPtr.size(); ++c) {
        if (!channelUsed[c]) continue;
        channelPtr[c] = cc.new_gp_ptr("ch");
        cc.mov(channelPtr[c], x86::ptr(channelTable, static_cast<int32_t>(c * sizeof(void*))));
    }

    Label done = cc.new_label();
    Label loop = cc.new_label();
    cc.test(count, count);
    cc.jz(done);

    std::vector<x86::Vec> value(nodes.size());
    x86::Vec zero, ones;
    if (needZero) {
        zero = cc.new_ymm_ps("zero");
        cc.vxorps(zero, zero, zero);
    }
    if (needOnes) {
        ones = cc.new_ymm_ps("ones");
        cc.vcmpps(ones, zero, zero, cmpTrue);
    }

    // Loop invariants (constants, runtime parameters) are broadcast once, before the loop.
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!live[i]) continue;
        const ExprNode& n = nodes[i];
        if (n.op == Op::constant) {
            value[i] = cc.new_ymm_ps("k");
            cc.vbroadcastss(value[i], x86::dword_ptr(constPool, constantSlot_[i] * 4));
        } else if (n.op == Op::param) {
            value[i] = cc.new_ymm_ps("p");
            cc.vbroadcastss(value[i], x86::dword_ptr(paramBlock, n.index * 4));
        }
    }

    x86::Gp index = cc.new_gp_ptr("i");
    cc.xor_(index, index);
    cc.bind(loop);

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!live[i]) continue;
        const ExprNode& n = nodes[i];
        if (n.op == Op::constant || n.op == Op::param) continue;
        x86::Vec d = cc.new_ymm_ps("v");
        value[i] = d;
        auto a = [&] { return value[static_cast<size_t>(n.a)]; };
        auto b = [&] { return value[static_cast<size_t>(n.b)]; };
        auto c = [&] { return value[static_cast<size_t>(n.c)]; };
        switch (n.op) {
            case Op::load:
                if (spec_.channelTypes[static_cast<size_t>(n.index)] == AttribType::int32) {
                    cc.vmovdqa(d, x86::ptr(channelPtr[static_cast<size_t>(n.index)], index, 2));
                    cc.vcvtdq2ps(d, d);
                } else {
                    cc.vmovaps(d, x86::ptr(channelPtr[static_cast<size_t>(n.index)], index, 2));
                }
                break;
            case Op::add: cc.vaddps(d, a(), b()); break;
            case Op::sub: cc.vsubps(d, a(), b()); break;
            case Op::mul: cc.vmulps(d, a(), b()); break;
            case Op::div: cc.vdivps(d, a(), b()); break;
            case Op::min: cc.vminps(d, a(), b()); break;
            case Op::max: cc.vmaxps(d, a(), b()); break;
            case Op::sqrt: cc.vsqrtps(d, a()); break;
            case Op::neg: cc.vsubps(d, zero, a()); break;
            case Op::abs: cc.vsubps(d, zero, a()); cc.vmaxps(d, a(), d); break;
            case Op::lt: cc.vcmpps(d, a(), b(), cmpLt); break;
            case Op::gt: cc.vcmpps(d, a(), b(), cmpGt); break;
            case Op::le: cc.vcmpps(d, a(), b(), cmpLe); break;
            case Op::ge: cc.vcmpps(d, a(), b(), cmpGe); break;
            case Op::eq: cc.vcmpps(d, a(), b(), cmpEq); break;
            case Op::logicalAnd: cc.vandps(d, a(), b()); break;
            case Op::logicalOr: cc.vorps(d, a(), b()); break;
            case Op::logicalNot: cc.vxorps(d, a(), ones); break;
            case Op::select: cc.vblendvps(d, c(), b(), a()); break;  // mask ? b : c
            case Op::floor: cc.vroundps(d, a(), 9); break;
            case Op::ceil: cc.vroundps(d, a(), 10); break;
            default: throw std::runtime_error("unhandled op in JIT");
        }
    }

    for (const auto& [channel, expr] : spec_.stores) {
        const x86::Mem dst = x86::ptr(channelPtr[static_cast<size_t>(channel)], index, 2);
        if (spec_.channelTypes[static_cast<size_t>(channel)] == AttribType::int32) {
            x86::Vec converted = cc.new_ymm_ps("cvt");
            cc.vcvttps2dq(converted, value[static_cast<size_t>(expr)]);
            cc.vmovdqa(dst, converted);
        } else {
            cc.vmovaps(dst, value[static_cast<size_t>(expr)]);
        }
    }

    cc.add(index, static_cast<int>(simdLanes));
    cc.cmp(index, count);
    cc.jb(loop);

    cc.bind(done);
    cc.end_func();
    err = cc.finalize();
    if (err != Error::kOk) throw std::runtime_error(std::string("asmjit finalize failed: ") + DebugUtils::error_as_string(err));
    err = backend_->runtime.add(&fn_, &code);
    if (err != Error::kOk) {
        fn_ = nullptr;
        throw std::runtime_error(std::string("asmjit runtime add failed: ") + DebugUtils::error_as_string(err));
    }
}

void Kernel::interpret(void* const* channels, const float* params, size_t paddedCount) const {
    const auto& nodes = spec_.graph.nodes();
    std::vector<float> v(nodes.size());
    for (size_t e = 0; e < paddedCount; ++e) {
        for (size_t i = 0; i < nodes.size(); ++i) {
            const ExprNode& n = nodes[i];
            auto a = [&] { return v[static_cast<size_t>(n.a)]; };
            auto b = [&] { return v[static_cast<size_t>(n.b)]; };
            auto c = [&] { return v[static_cast<size_t>(n.c)]; };
            switch (n.op) {
                case Op::constant: v[i] = n.value; break;
                case Op::param: v[i] = params[n.index]; break;
                case Op::load:
                    v[i] = spec_.channelTypes[static_cast<size_t>(n.index)] == AttribType::int32
                               ? static_cast<float>(static_cast<const int32_t*>(channels[n.index])[e])
                               : static_cast<const float*>(channels[n.index])[e];
                    break;
                default: v[i] = evalOp(n.op, n.a >= 0 ? a() : 0.0f, n.b >= 0 ? b() : 0.0f, n.c >= 0 ? c() : 0.0f); break;
            }
        }
        for (const auto& [channel, expr] : spec_.stores) {
            const float result = v[static_cast<size_t>(expr)];
            if (spec_.channelTypes[static_cast<size_t>(channel)] == AttribType::int32) static_cast<int32_t*>(channels[channel])[e] = toInt32(result);
            else static_cast<float*>(channels[channel])[e] = result;
        }
    }
}

void Kernel::runSerial(void* const* channels, const float* params, size_t paddedCount) const {
    if (fn_) fn_(channels, params, constants_.data(), paddedCount);
    else interpret(channels, params, paddedCount);
}

void Kernel::run(void* const* channels, const float* params, size_t paddedCount, ThreadPool* pool) const {
    constexpr size_t grain = size_t(1) << 15;
    const size_t chunks = pool ? std::min(pool->concurrency(), paddedCount / grain) : 1;
    if (chunks <= 1) {
        runSerial(channels, params, paddedCount);
        return;
    }

    // Channel elements are 4 bytes and chunks are multiples of simdLanes, so every chunk stays 32-byte aligned.
    const size_t chunkSize = padToLanes((paddedCount + chunks - 1) / chunks);
    const size_t channelCount = spec_.channels.size();
    pool->parallelFor(chunks, [&](size_t i) {
        const size_t start = i * chunkSize;
        if (start >= paddedCount) return;
        std::vector<void*> shifted(channelCount);
        for (size_t c = 0; c < channelCount; ++c) shifted[c] = static_cast<char*>(channels[c]) + start * 4;
        runSerial(shifted.data(), params, std::min(chunkSize, paddedCount - start));
    });
}

void Kernel::run(Geometry& geo, const std::vector<float>& params, AttribClass cls, ThreadPool* pool) const {
    if (params.size() < static_cast<size_t>(spec_.paramCount))
        throw std::runtime_error("kernel: not enough parameters supplied");
    std::vector<void*> ptrs;
    ptrs.reserve(spec_.channels.size());
    for (size_t c = 0; c < spec_.channels.size(); ++c) ptrs.push_back(geo.attrib(cls, spec_.channels[c], spec_.channelTypes[c]).data());
    run(ptrs.data(), params.data(), geo.paddedCount(cls), pool);
}

KernelCompiler::KernelCompiler() : backend_(std::make_shared<JitBackend>()) { jitSupported_ = backend_->avx2; }

KernelCompiler::~KernelCompiler() = default;

void KernelCompiler::setJitEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
    if (jitEnabled_ != enabled) cache_.clear();
    jitEnabled_ = enabled;
}

size_t KernelCompiler::cacheSize() const {
    std::lock_guard lock(mutex_);
    return cache_.size();
}

std::shared_ptr<const Kernel> KernelCompiler::compileFor(KernelSpec spec, const Geometry& geo, AttribClass cls) {
    for (size_t c = 0; c < spec.channels.size(); ++c)
        if (const AttribArray* existing = geo.findAttrib(cls, spec.channels[c])) spec.channelTypes[c] = existing->type();
    return compile(spec);
}

std::shared_ptr<const Kernel> KernelCompiler::compile(const KernelSpec& spec) {
    const std::string key = spec.signature();
    std::lock_guard lock(mutex_);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;

    const auto compileStart = std::chrono::steady_clock::now();
    std::shared_ptr<Kernel> kernel(new Kernel(spec, backend_));
    if (jitSupported_ && jitEnabled_) {
        try {
            kernel->compileJit();
        } catch (const JitUnsupported&) {
        }
    }
    ++compileCount_;
    compileMillis_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compileStart).count();
    cache_.emplace(key, kernel);
    return kernel;
}

}  // namespace jitnodes
