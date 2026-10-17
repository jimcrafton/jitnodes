// Performance benchmark: interpreter (JIT off) vs JIT, single vs multi threaded, kernel level and network level.
// Prints markdown tables. See docs/performance.md.
#include <algorithm>
#include <asmjit/x86.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#include "jitnodes/jitnodes.h"

using namespace jitnodes;

namespace {

using Clock = std::chrono::steady_clock;

// Median over `rounds` of the mean time of `iterations` calls, after one warm-up call.
double measureMs(const std::function<void()>& fn, int iterations, int rounds = 5) {
    fn();
    std::vector<double> samples;
    for (int r = 0; r < rounds; ++r) {
        const auto t0 = Clock::now();
        for (int i = 0; i < iterations; ++i) fn();
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / iterations);
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

using NativeFn = void (*)(float* __restrict x, float* __restrict y, size_t n, const float* p);

struct KernelCase {
    const char* name;
    const char* code;
    std::vector<std::string> paramNames;
    std::vector<float> params;
    NativeFn native = nullptr;  // hand-written C++ equivalent, compiled by the host compiler (/O2, SSE2 baseline)
};

void nativeArithmetic(float* __restrict x, float* __restrict y, size_t n, const float* p) {
    for (size_t i = 0; i < n; ++i) {
        const float nx = x[i] * p[0] + p[1];
        y[i] = y[i] * p[0] - nx * 0.5f;
        x[i] = nx;
    }
}

void nativeConditional(float* __restrict x, float* __restrict y, size_t n, const float* p) {
    for (size_t i = 0; i < n; ++i) {
        const float nx = x[i] * p[0] + p[1];
        y[i] = y[i] + (nx > p[2] ? p[3] : 0.0f) + std::sqrt(std::fabs(nx));
        x[i] = nx;
    }
}

Geometry makeGeometry(size_t n) {
    Geometry geo;
    geo.resizePoints(n);
    for (size_t i = 0; i < n; ++i) {
        geo.x()[i] = static_cast<float>(i % 1000);
        geo.y()[i] = static_cast<float>(i % 13);
    }
    return geo;
}

void kernelSection(size_t n, size_t threads) {
    const std::vector<KernelCase> cases = {
        {"arithmetic", "x = x * s + t; y = y * s - x * 0.5;", {"s", "t"}, {1.0001f, 0.5f}, nativeArithmetic},
        {"conditional", "x = x * s + t; y = y + (x > th ? off : 0) + sqrt(abs(x));", {"s", "t", "th", "off"}, {1.0001f, 0.5f, 100.0f, 2.0f}, nativeConditional},
        {"nested masks",
         "y = (x > a && y < 5) ? y + b : ((x <= 0 || y == 3) ? -y : max(y, min(x, 4))); x = !(x >= 2) ? x * 2 : clamp(x, 0, 6);",
         {"a", "b"},
         {100.0f, 2.0f}},
        {"sin + cos", "x = sin(x * 0.01) + cos(y * 0.1);", {}, {}},
        {"exp + log + pow", "x = exp(x * -0.001) + log(y + 1) + pow(y + 1, 0.5);", {}, {}},
    };

    KernelCompiler probe;
    const std::vector<Isa> isas = probe.supportedIsas();
    ThreadPool pool(threads - 1);
    std::printf("### Kernels, %zu points (ms per run, median of 5)\n\n", n);
    std::printf("| kernel | JIT off | native C++ |");
    for (Isa isa : isas) std::printf(" %s |", isaName(isa));
    std::printf(" best, %zu threads | best vs off | compile (avx2) |\n|---|---:|---:|", threads);
    for (size_t i = 0; i < isas.size(); ++i) std::printf("---:|");
    std::printf("---:|---:|---:|\n");

    for (const KernelCase& c : cases) {
        const KernelSpec spec = parseWrangle(c.code, c.paramNames);
        KernelCompiler interp;
        interp.setJitEnabled(false);
        Geometry geo = makeGeometry(n);
        const auto ki = interp.compile(spec);
        const double off = measureMs([&] { ki->run(geo, c.params); }, 1, 3);

        char nativeCell[32] = "-";
        if (c.native) {
            const double native = measureMs([&] { c.native(geo.x(), geo.y(), geo.paddedPointCount(), c.params.data()); }, 20, 5);
            std::snprintf(nativeCell, sizeof(nativeCell), "%.3f", native);
        }

        std::printf("| %s | %.2f | %s |", c.name, off, nativeCell);
        double best = 1e30;
        std::shared_ptr<const Kernel> bestKernel;
        double compileAvx2 = 0.0;
        for (Isa isa : isas) {
            KernelCompiler jit;
            jit.setIsa(isa);
            const auto kj = jit.compile(spec);
            if (isa == Isa::avx2) compileAvx2 = jit.compileMillis();
            const double ms = measureMs([&] { kj->run(geo, c.params); }, 20, 5);
            std::printf(" %.3f |", ms);
            if (ms < best) {
                best = ms;
                bestKernel = kj;
            }
        }
        const double bestMt = measureMs([&] { bestKernel->run(geo, c.params, AttribClass::point, &pool); }, 20, 5);
        std::printf(" %.3f | %.0fx | %.3f ms |\n", bestMt, off / best, compileAvx2);
    }
    std::printf("\n");
}

struct ChainNet {
    Network net;
    NodeId wrangle = 0;
    NodeId out = 0;
    explicit ChainNet(size_t threads, bool fusion, bool jit) : net(threads) {
        net.setFusionEnabled(fusion);
        net.compiler().setJitEnabled(jit);
        NodeId line = net.addNode("line");
        NodeId xf1 = net.addNode("transform");
        wrangle = net.addNode("wrangle");
        NodeId xf2 = net.addNode("transform");
        out = net.addNode("null");
        net.node(line).setFloat("count", 1000000.0f);
        net.node(line).setFloat("length", 100.0f);
        net.node(xf1).setFloat("sx", 2.0f);
        net.node(wrangle).setString("code", "y = y + (x > a ? x * b : -x) + sqrt(abs(x)); w = y * 0.5;");
        net.node(wrangle).setFloat("a", 50.0f);
        net.node(wrangle).setFloat("b", 0.25f);
        net.node(xf2).setFloat("ty", 1.0f);
        net.connect(line, xf1);
        net.connect(xf1, wrangle);
        net.connect(wrangle, xf2);
        net.connect(xf2, out);
        net.setDisplayNode(out);
        net.cook(out);
    }
    double recookMs() {
        int flip = 0;
        return measureMs(
            [&] {
                net.node(wrangle).setFloat("a", 50.0f + static_cast<float>(++flip % 7));
                net.cook(out);
            },
            3, 5);
    }
};

NodeId buildBranches(Network& net, int branches, float points) {
    std::vector<NodeId> tails;
    for (int b = 0; b < branches; ++b) {
        NodeId line = net.addNode("line");
        NodeId xf = net.addNode("transform");
        NodeId wr = net.addNode("wrangle");
        net.node(line).setFloat("count", points);
        net.node(xf).setFloat("sx", 1.0f + static_cast<float>(b));
        net.node(wr).setString("code", "y = y + x * a + sqrt(abs(x));");
        net.connect(line, xf);
        net.connect(xf, wr);
        tails.push_back(wr);
    }
    while (tails.size() > 1) {
        std::vector<NodeId> next;
        for (size_t i = 0; i + 1 < tails.size(); i += 2) {
            NodeId merge = net.addNode("merge");
            net.connect(tails[i], merge, 0);
            net.connect(tails[i + 1], merge, 1);
            next.push_back(merge);
        }
        tails = next;
    }
    NodeId out = net.addNode("null");
    net.connect(tails[0], out);
    net.setDisplayNode(out);
    return out;
}

double branchRecookMs(size_t threads, bool jit) {
    Network net(threads);
    net.compiler().setJitEnabled(jit);
    const NodeId out = buildBranches(net, 8, 1000000.0f);
    net.cook(out);
    std::vector<NodeId> wranglers;
    for (NodeId id : net.nodeIds())
        if (net.node(id).typeName() == "wrangle") wranglers.push_back(id);
    int flip = 0;
    return measureMs(
        [&] {
            ++flip;
            for (NodeId id : wranglers) net.node(id).setFloat("a", static_cast<float>(flip));
            net.cook(out);
        },
        2, 3);
}

}  // namespace

int main() {
    const size_t hardware = std::max(1u, std::thread::hardware_concurrency());
    const size_t threads = std::min<size_t>(4, hardware);
    std::printf("# jitnodes benchmark\n\n");
    std::printf("- cpu: %s\n- hardware threads: %zu (multi-threaded rows use %zu)\n", asmjit::CpuInfo::host().brand(), hardware, threads);
#ifdef NDEBUG
    std::printf("- build: optimised (NDEBUG)\n\n");
#else
    std::printf("- build: NOT optimised - numbers are not meaningful\n\n");
#endif

    kernelSection(size_t(1) << 22, threads);

    std::printf("### Network recook: line(1M) -> transform -> wrangle -> transform, one parameter edited per recook (ms, median of 5)\n\n");
    std::printf("| configuration | ms per recook |\n|---|---:|\n");
    for (const auto& [label, fusion, jit, t] : std::vector<std::tuple<const char*, bool, bool, size_t>>{
             {"no fusion, JIT off", false, false, 1},
             {"fusion, JIT off", true, false, 1},
             {"no fusion, JIT on", false, true, 1},
             {"fusion, JIT on", true, true, 1},
             {"fusion, JIT on, threads", true, true, threads}}) {
        ChainNet chain(t, fusion, jit);
        std::printf("| %s%s | %.2f |\n", label, std::string(label).find("threads") != std::string::npos ? (" (" + std::to_string(t) + ")").c_str() : "",
                    chain.recookMs());
    }

    std::printf("\n### Network recook: 8 branches x 1M points, fused transform+wrangle, balanced merge tree (ms, median of 3)\n\n");
    std::printf("| configuration | ms per recook |\n|---|---:|\n");
    for (const auto& [label, jit, t] : std::vector<std::tuple<const char*, bool, size_t>>{
             {"JIT off, 1 thread", false, 1},
             {"JIT on, 1 thread", true, 1},
             {"JIT off, threads", false, threads},
             {"JIT on, threads", true, threads}}) {
        std::printf("| %s%s | %.1f |\n", label, std::string(label).find("threads") != std::string::npos ? (" (" + std::to_string(t) + ")").c_str() : "",
                    branchRecookMs(t, jit));
    }

    std::printf("\n### Profile of one 4-thread recook (fused chain per branch)\n\n```\n");
    {
        Network net(threads);
        const NodeId out = buildBranches(net, 4, 1000000.0f);
        net.cook(out);
        net.resetProfile();
        for (NodeId id : net.nodeIds())
            if (net.node(id).typeName() == "wrangle") net.node(id).setFloat("a", 3.0f);
        net.cook(out);
        std::printf("%s", net.profileReport().c_str());
    }
    std::printf("```\n");
    return 0;
}
