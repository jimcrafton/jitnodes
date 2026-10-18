#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "jitnodes/geo/geometry.h"
#include "jitnodes/jit/expr.h"
#include "jitnodes/jit/isa.h"

namespace jitnodes {

class ThreadPool;
struct JitBackend;  // owns the asmjit runtime; defined in kernel.cpp so asmjit stays out of headers

// A compiled per-point kernel. Executes the whole expression DAG for 4, 8 or 16 points at a time with
// every intermediate value held in a register (loop fusion), reading and writing point channels
// once. Falls back to a scalar interpreter when the CPU has no AVX2 or JIT is disabled; both
// paths implement identical semantics.
class Kernel {
public:
    ~Kernel();
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    // channels[i] is the simdAlignment-aligned array for spec().channels[i]; paddedCount must be a
    // multiple of simdLanes. params holds spec().paramCount runtime values.
    // With a pool, large ranges are split into 8-lane aligned chunks that run concurrently.
    void run(void* const* channels, const float* params, size_t paddedCount, ThreadPool* pool = nullptr) const;

    // Binds channels by name on geo (creating missing float ones) and runs over every element of cls.
    void run(Geometry& geo, const std::vector<float>& params, AttribClass cls = AttribClass::point,
             ThreadPool* pool = nullptr) const;

    bool isJitted() const { return fn_ != nullptr; }
    Isa isa() const { return isa_; }  // instruction set the code was generated for (when jitted)
    const KernelSpec& spec() const { return spec_; }

private:
    friend class KernelCompiler;
    using Fn = void (*)(void* const*, const float*, const float*, size_t);

    Kernel(KernelSpec spec, std::shared_ptr<JitBackend> backend, Isa isa);
    void compileJit();
    void runSerial(void* const* channels, const float* params, size_t paddedCount) const;
    void interpret(void* const* channels, const float* params, size_t paddedCount) const;

    KernelSpec spec_;
    std::shared_ptr<JitBackend> backend_;
    Isa isa_;
    std::vector<float> constants_;  // constant pool read by the generated code
    Fn fn_ = nullptr;
};

// Compiles KernelSpecs and caches the result by structure. Because tweakable values are
// runtime parameters rather than baked-in constants, editing a slider reuses the cached kernel.
class KernelCompiler {
public:
    KernelCompiler();
    ~KernelCompiler();

    std::shared_ptr<const Kernel> compile(const KernelSpec& spec);

    // Takes channel types from the attributes that already exist on geo (new channels are float).
    std::shared_ptr<const Kernel> compileFor(KernelSpec spec, const Geometry& geo, AttribClass cls);

    bool jitSupported() const { return jitSupported_; }

    // Instruction set used for new kernels: AVX2 when available, else SSE2. AVX-512 is opt-in. Changing it
    // clears the cache; selecting an unsupported set throws.
    Isa isa() const { return isa_; }
    void setIsa(Isa isa);
    bool isaSupported(Isa isa) const;
    std::vector<Isa> supportedIsas() const;
    void setJitEnabled(bool enabled);  // clears the cache; false forces the interpreter
    bool jitEnabled() const { return jitEnabled_; }

    size_t compileCount() const { return compileCount_; }  // actual compilations (cache misses)
    double compileMillis() const { return compileMillis_; }  // total time spent compiling
    size_t cacheSize() const;
    // Kernels are cached by structure; beyond this many the least recently used quarter is dropped (kernels still in use by a cook
    // stay alive through their shared_ptr). Typing code into a wrangle makes a new program per keystroke.
    static constexpr size_t maxCachedKernels = 4096;

private:
    std::shared_ptr<JitBackend> backend_;
    mutable std::mutex mutex_;
    struct CacheEntry {
        std::shared_ptr<const Kernel> kernel;
        uint64_t lastUse = 0;
    };
    std::map<std::string, CacheEntry> cache_;
    uint64_t useTick_ = 0;
    bool jitSupported_ = false;
    Isa isa_ = Isa::sse2;
    bool jitEnabled_ = true;
    size_t compileCount_ = 0;
    double compileMillis_ = 0.0;
};

}  // namespace jitnodes
