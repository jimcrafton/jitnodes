#include "jitnodes/jit/kernel.h"

#include <asmjit/x86.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

#ifdef _MSC_VER
#include <malloc.h>
#endif

#include "jitnodes/core/threadPool.h"
#include "jitnodes/jit/opEval.h"
#include "vecEmitter.h"

namespace jitnodes {

struct JitBackend {
    asmjit::JitRuntime runtime;
    bool sse2 = false;
    bool avx2 = false;
    bool avx512 = false;

    JitBackend() {
        if (runtime.arch() != asmjit::Arch::kX64) return;
        const auto& f = runtime.cpu_features().x86();
        sse2 = true;  // part of the x86-64 baseline
        avx2 = f.has_avx2();
        avx512 = f.has_avx512_f() && f.has_avx512_dq();
    }

    bool supports(Isa isa) const { return isa == Isa::sse2 ? sse2 : isa == Isa::avx2 ? avx2 : avx512; }
};

namespace {

int32_t toInt32(float f) {
    return (f > -2147483648.0f && f < 2147483648.0f) ? static_cast<int32_t>(f) : INT32_MIN;
}

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

namespace {

// Generated code allocates its spill area with a single stack adjustment and, unlike compiler output, does not probe the stack
// page by page. On Windows the stack grows only by touching its guard page, so a frame larger than a page (a kernel with
// thousands of live parameters spills tens of kilobytes) can skip the guard page on a thread whose stack has not grown yet and
// crash. Committing a generous part of the stack once per thread, before the first generated function runs there, avoids that.
constexpr size_t jitStackReserve = 256 * 1024;

#ifdef _MSC_VER
__declspec(noinline) void commitStack(size_t bytes) {
    volatile char* p = static_cast<volatile char*>(_alloca(bytes));
    for (size_t i = 0; i < bytes; i += 4096) p[i] = 0;
}
#endif

void ensureStackForGeneratedCode() {
#ifdef _MSC_VER
    thread_local bool committed = false;
    if (committed) return;
    committed = true;
    commitStack(jitStackReserve);
#endif
}

}  // namespace

Kernel::Kernel(KernelSpec spec, std::shared_ptr<JitBackend> backend, Isa isa)
    : spec_(std::move(spec)), backend_(std::move(backend)), isa_(isa) {}

Kernel::~Kernel() {
    if (fn_) backend_->runtime.release(fn_);
}

void Kernel::compileJit() {
    using namespace asmjit;
    const auto& nodes = spec_.graph.nodes();
    const std::vector<bool> live = liveNodes(spec_);

    std::vector<bool> channelUsed(spec_.channels.size(), false);
    for (size_t i = 0; i < nodes.size(); ++i)
        if (live[i] && nodes[i].op == Op::load) channelUsed[static_cast<size_t>(nodes[i].index)] = true;
    for (const auto& s : spec_.stores) channelUsed[static_cast<size_t>(s.first)] = true;

    CodeHolder code;
    Error err = code.init(backend_->runtime.environment(), backend_->runtime.cpu_features());
    if (err != Error::kOk) throw std::runtime_error("asmjit: CodeHolder init failed");

    FileLogger logger(stderr);
    if (std::getenv("JITNODES_JIT_LOG")) code.set_logger(&logger);
    x86::Compiler cc(&code);

    // void kernel(void* const* channels, const float* params, const float* consts, size_t count)
    FuncNode* func = cc.add_func(FuncSignature::build<void, void* const*, const float*, const float*, size_t>());
    if (isa_ != Isa::sse2) {
        // Without these asmjit spills wide registers with legacy SSE moves that only keep 128 bits.
        func->frame().set_avx_enabled();
        func->frame().set_avx_cleanup();
    }
    if (isa_ == Isa::avx512) func->frame().set_avx512_enabled();

    x86::Gp channelTable = cc.new_gp_ptr("channels");
    x86::Gp paramBlock = cc.new_gp_ptr("params");
    x86::Gp constPool = cc.new_gp_ptr("consts");
    x86::Gp count = cc.new_gp_ptr("count");
    func->set_arg(0, channelTable);
    func->set_arg(1, paramBlock);
    func->set_arg(2, constPool);
    func->set_arg(3, count);

    VecEmitter em(cc, isa_, constants_, constPool);

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
    // Loop invariants (constants, runtime parameters) are broadcast once, before the loop, while there are few enough to stay in
    // registers. A long fused chain has thousands of parameters; hoisting those would spill every one of them to a stack frame
    // of tens of kilobytes, so beyond the limit each is broadcast from memory where it is used (one load, no spill).
    constexpr size_t hoistLimit = 32;
    size_t invariants = 0;
    for (size_t i = 0; i < nodes.size(); ++i)
        if (live[i] && (nodes[i].op == Op::constant || nodes[i].op == Op::param)) ++invariants;
    const bool hoist = invariants <= hoistLimit;
    auto emitInvariant = [&](const ExprNode& n) { return n.op == Op::constant ? em.konst(n.value) : em.broadcast(x86::dword_ptr(paramBlock, n.index * 4)); };
    for (size_t i = 0; hoist && i < nodes.size(); ++i) {
        if (!live[i]) continue;
        const ExprNode& n = nodes[i];
        if (n.op == Op::constant || n.op == Op::param) value[i] = emitInvariant(n);
    }

    x86::Gp index = cc.new_gp_ptr("i");
    cc.xor_(index, index);
    cc.bind(loop);

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!live[i]) continue;
        const ExprNode& n = nodes[i];
        if (n.op == Op::constant || n.op == Op::param) {
            if (!hoist) value[i] = emitInvariant(n);
            continue;
        }
        if (n.op == Op::load) {
            x86::Vec d = em.newVec("ld");
            const x86::Mem mem = x86::ptr(channelPtr[static_cast<size_t>(n.index)], index, 2);
            if (spec_.channelTypes[static_cast<size_t>(n.index)] == AttribType::int32) em.loadInt(d, mem);
            else em.load(d, mem);
            value[i] = d;
            continue;
        }
        auto operand = [&](int idx) { return idx >= 0 ? value[static_cast<size_t>(idx)] : x86::Vec(); };
        value[i] = em.op(n.op, operand(n.a), operand(n.b), operand(n.c));
    }

    for (const auto& [channel, expr] : spec_.stores) {
        const x86::Mem dst = x86::ptr(channelPtr[static_cast<size_t>(channel)], index, 2);
        if (spec_.channelTypes[static_cast<size_t>(channel)] == AttribType::int32) em.storeInt(dst, value[static_cast<size_t>(expr)]);
        else em.store(dst, value[static_cast<size_t>(expr)]);
    }

    cc.add(index, static_cast<int>(em.lanes()));
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
            auto operand = [&](int idx) { return idx >= 0 ? v[static_cast<size_t>(idx)] : 0.0f; };
            switch (n.op) {
                case Op::constant: v[i] = n.value; break;
                case Op::param: v[i] = params[n.index]; break;
                case Op::load:
                    v[i] = spec_.channelTypes[static_cast<size_t>(n.index)] == AttribType::int32
                               ? static_cast<float>(static_cast<const int32_t*>(channels[n.index])[e])
                               : static_cast<const float*>(channels[n.index])[e];
                    break;
                default: v[i] = evalOp(n.op, operand(n.a), operand(n.b), operand(n.c)); break;
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
    if (fn_) {
        ensureStackForGeneratedCode();
        fn_(channels, params, constants_.data(), paddedCount);
    } else {
        interpret(channels, params, paddedCount);
    }
}

void Kernel::run(void* const* channels, const float* params, size_t paddedCount, ThreadPool* pool) const {
    constexpr size_t grain = size_t(1) << 15;
    const size_t chunks = pool ? std::min(pool->concurrency(), paddedCount / grain) : 1;
    if (chunks <= 1) {
        runSerial(channels, params, paddedCount);
        return;
    }

    // Channel elements are 4 bytes and chunks are multiples of simdLanes, so every chunk keeps the alignment.
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
    std::vector<char> written(spec_.channels.size(), 0);
    for (const auto& store : spec_.stores) written[static_cast<size_t>(store.first)] = 1;
    std::vector<void*> ptrs;
    ptrs.reserve(spec_.channels.size());
    for (size_t c = 0; c < spec_.channels.size(); ++c) {
        AttribArray& a = geo.attrib(cls, spec_.channels[c], spec_.channelTypes[c]);
        ptrs.push_back(written[c] ? a.data() : const_cast<void*>(std::as_const(a).data()));
    }
    run(ptrs.data(), params.data(), geo.paddedCount(cls), pool);
}

KernelCompiler::KernelCompiler() : backend_(std::make_shared<JitBackend>()) {
    jitSupported_ = backend_->sse2;
    // Conservative default: AVX2. AVX-512 is available through setIsa (see docs/performance.md).
    isa_ = backend_->avx2 ? Isa::avx2 : Isa::sse2;
}

KernelCompiler::~KernelCompiler() = default;

void KernelCompiler::setJitEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
    if (jitEnabled_ != enabled) cache_.clear();
    jitEnabled_ = enabled;
}

bool KernelCompiler::isaSupported(Isa isa) const { return backend_->supports(isa); }

std::vector<Isa> KernelCompiler::supportedIsas() const {
    std::vector<Isa> result;
    for (Isa isa : {Isa::sse2, Isa::avx2, Isa::avx512})
        if (backend_->supports(isa)) result.push_back(isa);
    return result;
}

void KernelCompiler::setIsa(Isa isa) {
    if (!isaSupported(isa)) throw std::runtime_error(std::string("instruction set '") + isaName(isa) + "' is not supported on this CPU");
    std::lock_guard lock(mutex_);
    if (isa_ != isa) cache_.clear();
    isa_ = isa;
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
    std::lock_guard lock(mutex_);
    const std::string key = spec.signature() + "#" + isaName(isa_);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        it->second.lastUse = ++useTick_;
        return it->second.kernel;
    }

    const auto compileStart = std::chrono::steady_clock::now();
    std::shared_ptr<Kernel> kernel(new Kernel(spec, backend_, isa_));
    if (jitSupported_ && jitEnabled_) kernel->compileJit();
    ++compileCount_;
    compileMillis_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compileStart).count();
    if (cache_.size() >= maxCachedKernels) {
        std::vector<uint64_t> uses;
        uses.reserve(cache_.size());
        for (const auto& entry : cache_) uses.push_back(entry.second.lastUse);
        std::nth_element(uses.begin(), uses.begin() + static_cast<std::ptrdiff_t>(uses.size() / 4), uses.end());
        const uint64_t cutoff = uses[uses.size() / 4];
        for (auto entry = cache_.begin(); entry != cache_.end();) entry = entry->second.lastUse < cutoff ? cache_.erase(entry) : std::next(entry);
    }
    cache_.emplace(key, CacheEntry{kernel, ++useTick_});
    return kernel;
}

}  // namespace jitnodes
