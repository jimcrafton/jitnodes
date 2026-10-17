#include <chrono>
#include <cstdio>

#include "jitnodes/jitnodes.h"

using namespace jitnodes;

namespace {

double timeRuns(const Kernel& kernel, Geometry& geo, const std::vector<float>& params, int runs) {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    for (int i = 0; i < runs; ++i) kernel.run(geo, params);
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / runs;
}

}  // namespace

int main() {
    const size_t n = 1 << 20;
    const char* code = "x = x * s + t; y = y + (x > th ? off : 0) + sqrt(abs(x));";
    KernelSpec spec = parseWrangle(code, {"s", "t", "th", "off"});
    const std::vector<float> params = {1.0001f, 0.5f, 100.0f, 2.0f};

    KernelCompiler jit, interp;
    interp.setJitEnabled(false);
    Geometry geo;
    geo.resizePoints(n);
    for (size_t i = 0; i < n; ++i) geo.x()[i] = static_cast<float>(i % 1000);

    auto kj = jit.compile(spec);
    auto ki = interp.compile(spec);
    std::printf("points: %zu, jit: %s\n", n, kj->isJitted() ? "yes" : "no");
    std::printf("interpreter: %.3f ms/run\n", timeRuns(*ki, geo, params, 5));
    std::printf("jit:         %.3f ms/run\n", timeRuns(*kj, geo, params, 50));
    return 0;
}
