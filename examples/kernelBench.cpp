#include <chrono>
#include <cstdio>

#include "jitnodes/jitnodes.h"

using namespace jitnodes;

namespace {

using Clock = std::chrono::steady_clock;

double timeRuns(const Kernel& kernel, Geometry& geo, const std::vector<float>& params, int runs, ThreadPool* pool) {
    auto t0 = Clock::now();
    for (int i = 0; i < runs; ++i) kernel.run(geo, params, AttribClass::point, pool);
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / runs;
}

double timeCook(Network& net, NodeId out, int rounds) {
    net.cook(out);
    auto t0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        net.node(net.findNode("wrangle3")).setFloat("a", static_cast<float>(i + 1));
        for (const char* name : {"wrangle6", "wrangle9", "wrangle12", "wrangle15", "wrangle18", "wrangle21", "wrangle24"})
            net.node(net.findNode(name)).setFloat("a", static_cast<float>(i + 1));
        net.cook(out);
    }
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / rounds;
}

}  // namespace

int main() {
    const size_t n = 1 << 22;
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
    std::printf("interpreter:     %8.3f ms/run\n", timeRuns(*ki, geo, params, 3, nullptr));
    std::printf("jit, 1 thread:   %8.3f ms/run\n", timeRuns(*kj, geo, params, 50, nullptr));
    for (size_t threads : {2, 4, 8}) {
        ThreadPool pool(threads - 1);
        std::printf("jit, %zu threads:  %8.3f ms/run\n", threads, timeRuns(*kj, geo, params, 50, &pool));
    }

    std::printf("\nnetwork with 8 independent branches (1M points each, fused transform+wrangle, balanced merge tree):\n");
    for (size_t threads : {1, 4, 8}) {
        Network net;
        net.setThreadCount(threads);
        std::vector<NodeId> tails;
        for (int b = 0; b < 8; ++b) {
            NodeId line = net.addNode("line");
            NodeId xf = net.addNode("transform");
            NodeId wr = net.addNode("wrangle");
            net.node(line).setFloat("count", 1000000.0f);
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
        std::printf("  %zu thread(s): %8.2f ms per full recook\n", net.threadCount(), timeCook(net, out, 5));
        if (threads == 4) std::printf("\n%s\n", net.profileReport().c_str());
    }
    return 0;
}
