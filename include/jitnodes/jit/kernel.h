#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "jitnodes/geo/geometry.h"
#include "jitnodes/jit/expr.h"

namespace jitnodes {

struct JitBackend;  // owns the asmjit runtime; defined in kernel.cpp so asmjit stays out of headers

// A compiled per-point kernel. Executes the whole expression DAG for 8 points at a time with
// every intermediate value held in a register (loop fusion), reading and writing point channels
// once. Falls back to a scalar interpreter when the CPU has no AVX2 or JIT is disabled; both
// paths implement identical semantics.
class Kernel {
public:
    ~Kernel();
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    // channels[i] is the 32-byte aligned array for spec().channels[i]; paddedCount must be a
    // multiple of simdLanes. params holds spec().paramCount runtime values.
    void run(float* const* channels, const float* params, size_t paddedCount) const;

    // Convenience: binds channels by name on geo (creating missing ones) and runs over all points.
    void run(Geometry& geo, const std::vector<float>& params) const;

    bool isJitted() const { return fn_ != nullptr; }
    const KernelSpec& spec() const { return spec_; }

private:
    friend class KernelCompiler;
    using Fn = void (*)(float* const*, const float*, const float*, size_t);

    Kernel(KernelSpec spec, std::shared_ptr<JitBackend> backend);
    void compileJit();
    void interpret(float* const* channels, const float* params, size_t paddedCount) const;

    KernelSpec spec_;
    std::shared_ptr<JitBackend> backend_;
    std::vector<float> constants_;  // constant pool read by the generated code
    std::vector<int> constantSlot_; // expression node -> constants_ index (or -1)
    Fn fn_ = nullptr;
};

// Compiles KernelSpecs and caches the result by structure. Because tweakable values are
// runtime parameters rather than baked-in constants, editing a slider reuses the cached kernel.
class KernelCompiler {
public:
    KernelCompiler();
    ~KernelCompiler();

    std::shared_ptr<const Kernel> compile(const KernelSpec& spec);

    bool jitSupported() const { return jitSupported_; }
    void setJitEnabled(bool enabled);  // clears the cache; false forces the interpreter
    bool jitEnabled() const { return jitEnabled_; }

    size_t compileCount() const { return compileCount_; }  // actual compilations (cache misses)
    size_t cacheSize() const;

private:
    std::shared_ptr<JitBackend> backend_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const Kernel>> cache_;
    bool jitSupported_ = false;
    bool jitEnabled_ = true;
    size_t compileCount_ = 0;
};

}  // namespace jitnodes
