#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "jitnodes/jitnodes.h"

using namespace jitnodes;

namespace {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& tests() {
    static std::vector<TestCase> t;
    return t;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { tests().push_back({name, std::move(fn)}); }
};

#define TEST(name)                                  \
    static void name();                             \
    static Registrar registrar_##name(#name, name); \
    static void name()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) throw std::runtime_error(std::string(__FILE__ ":") + std::to_string(__LINE__) + ": " #cond); \
    } while (0)

#define CHECK_NEAR(a, b) CHECK(std::fabs((a) - (b)) < 1e-4f)

TEST(alignedArrayPadsAndZeroFills) {
    AlignedArray<float> a(5);
    CHECK(a.paddedSize() == 8);
    CHECK(reinterpret_cast<uintptr_t>(a.data()) % simdAlignment == 0);
    for (size_t i = 0; i < a.paddedSize(); ++i) CHECK(a.data()[i] == 0.0f);
    a[4] = 3.0f;
    a.resize(20);
    CHECK(a[4] == 3.0f && a[19] == 0.0f);
}

TEST(arenaAlignsAndResets) {
    Arena arena(1024);
    void* p = arena.allocate(1);
    void* q = arena.allocate(1);
    CHECK(reinterpret_cast<uintptr_t>(p) % 64 == 0);
    CHECK(reinterpret_cast<uintptr_t>(q) % 64 == 0);
    arena.reset();
    CHECK(arena.used() == 0);
    bool threw = false;
    try { arena.allocate(4096); } catch (const std::bad_alloc&) { threw = true; }
    CHECK(threw);
}

TEST(geometryAppendMergesAttribs) {
    Geometry a, b;
    a.resizePoints(3);
    b.resizePoints(2);
    b.pointAttrib("w")[1] = 7.0f;
    b.primitives().push_back({0, 2, false});
    a.append(b);
    CHECK(a.pointCount() == 5);
    CHECK((*a.findPointAttrib("w"))[4] == 7.0f);
    CHECK(a.primitives().size() == 1 && a.primitives()[0].start == 3);
}

TEST(parserRejectsBadInput) {
    bool threw = false;
    try { parseWrangle("x = (1 + ;", {}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { parseWrangle("a = 1;", {"a"}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

TEST(exprGraphSharesSubexpressions) {
    KernelSpec s = parseWrangle("x = (x + 1) * (1 + x);", {});
    CHECK(s.graph.size() == 4);  // load, const, add, mul
}

Geometry makeRamp(size_t n) {
    Geometry g;
    g.resizePoints(n);
    for (size_t i = 0; i < n; ++i) {
        g.x()[i] = static_cast<float>(i) - 10.0f;
        g.y()[i] = static_cast<float>(i % 7);
    }
    return g;
}

void expectJitMatchesInterpreter(const std::string& code, const std::vector<std::string>& names,
                                 const std::vector<float>& params) {
    KernelSpec spec = parseWrangle(code, names);
    KernelCompiler jit, interp;
    interp.setJitEnabled(false);
    Geometry a = makeRamp(37), b = makeRamp(37);
    auto kj = jit.compile(spec);
    auto ki = interp.compile(spec);
    CHECK(!ki->isJitted());
    if (jit.jitSupported()) CHECK(kj->isJitted());
    kj->run(a, params);
    ki->run(b, params);
    for (const auto& [name, arrayA] : a.pointAttribs()) {
        const auto& arrayB = *b.findPointAttrib(name);
        for (size_t i = 0; i < a.pointCount(); ++i) {
            if (std::fabs(arrayA[i] - arrayB[i]) >= 1e-4f) {
                std::string msg = name + "[" + std::to_string(i) + "] mismatch;";
                for (const auto& [n2, a2] : a.pointAttribs())
                    msg += " " + n2 + ": jit=" + std::to_string(a2[i]) + " interp=" + std::to_string((*b.findPointAttrib(n2))[i]);
                throw std::runtime_error(msg);
            }
        }
    }
}

TEST(jitMatchesInterpreterArithmetic) {
    expectJitMatchesInterpreter("x = x * s + t; y = -y / 2 + abs(x) - sqrt(y);", {"s", "t"}, {2.0f, 0.5f});
}

TEST(jitMatchesInterpreterConditionals) {
    expectJitMatchesInterpreter(
        "y = (x > th && y < 5) ? y + off : (x <= 0 || y == 3 ? -y : max(y, min(x, 4)));"
        "x = !(x >= 2) ? x * 2 : clamp(x, 0, 6); w = lerp(x, y, 0.25) + (x != y ? 1 : 0);",
        {"th", "off"}, {5.0f, 100.0f});
}

TEST(docExampleMaskThreshold) {
    KernelCompiler compiler;
    KernelSpec spec = parseWrangle("x = x * 2; y = y + (x > 5 ? 100 : 0);", {});
    Geometry g;
    g.resizePoints(8);
    for (size_t i = 0; i < 8; ++i) { g.x()[i] = static_cast<float>(i + 1); g.y()[i] = 10.0f; }
    compiler.compile(spec)->run(g, {});
    for (size_t i = 0; i < 8; ++i) {
        float x = static_cast<float>(i + 1) * 2;
        CHECK_NEAR(g.x()[i], x);
        CHECK_NEAR(g.y()[i], x > 5 ? 110.0f : 10.0f);
    }
}

TEST(kernelCacheIgnoresParameterValues) {
    KernelCompiler compiler;
    KernelSpec spec = parseWrangle("x = x * s;", {"s"});
    auto k1 = compiler.compile(spec);
    auto k2 = compiler.compile(spec);
    CHECK(k1 == k2 && compiler.compileCount() == 1);
    Geometry g = makeRamp(8);
    k1->run(g, {3.0f});
    CHECK_NEAR(g.x()[1], -27.0f);
    k1->run(g, {1.0f});
    CHECK_NEAR(g.x()[1], -27.0f);
}

TEST(threeDimensionalChannelsWork) {
    Network net;
    NodeId line = net.addNode("line");
    NodeId wr = net.addNode("wrangle");
    net.node(line).setFloat("count", 10.0f);
    net.connect(line, wr);
    net.node(wr).setString("code", "z = x * 2 + 1; x = x + z * 0;");
    auto geo = net.cook(wr);
    const auto* z = geo->findPointAttrib("z");
    CHECK(z != nullptr);
    CHECK_NEAR((*z)[9], 21.0f);
}

struct Chain {
    Network net;
    NodeId line, xf1, wr, xf2, out;
    explicit Chain(bool fusion) {
        net.setFusionEnabled(fusion);
        line = net.addNode("line");
        xf1 = net.addNode("transform");
        wr = net.addNode("wrangle");
        xf2 = net.addNode("transform");
        out = net.addNode("null");
        net.node(line).setFloat("count", 21.0f);
        net.node(line).setFloat("length", 20.0f);
        net.node(xf1).setFloat("sx", 0.5f);
        net.node(xf1).setFloat("tx", -3.0f);
        net.node(wr).setString("code", "y = (x > a ? x * b : -x) + y; w = y * 0.5;");
        net.node(wr).setFloat("a", 2.0f);
        net.node(wr).setFloat("b", 3.0f);
        net.node(xf2).setFloat("sy", 2.0f);
        net.node(xf2).setFloat("ty", 1.0f);
        net.connect(line, xf1);
        net.connect(xf1, wr);
        net.connect(wr, xf2);
        net.connect(xf2, out);
        net.setDisplayNode(out);
    }
};

void expectSameGeometry(const Geometry& a, const Geometry& b) {
    CHECK(a.pointCount() == b.pointCount());
    CHECK(a.pointAttribs().size() == b.pointAttribs().size());
    for (const auto& [name, array] : a.pointAttribs())
        for (size_t i = 0; i < a.pointCount(); ++i) CHECK_NEAR(array[i], (*b.findPointAttrib(name))[i]);
}

TEST(fusionMatchesUnfusedResult) {
    Chain fused(true), plain(false);
    expectSameGeometry(*fused.net.cookDisplay(), *plain.net.cookDisplay());
    CHECK(fused.net.fusedChainCount() == 1);
    CHECK(fused.net.compiler().compileCount() == 1);
    CHECK(plain.net.compiler().compileCount() == 2);
    CHECK(fused.net.node(fused.xf1).cookCount() == 0);
    CHECK(fused.net.node(fused.wr).cookCount() == 0);
    CHECK(fused.net.node(fused.xf2).cookCount() == 1);
    CHECK(plain.net.node(plain.wr).cookCount() == 1);
}

TEST(fusionRespondsToParameterChangesWithoutRecompiling) {
    Chain fused(true), plain(false);
    fused.net.cookDisplay();
    for (Chain* c : {&fused, &plain}) {
        c->net.node(c->xf1).setFloat("tx", 4.0f);
        c->net.node(c->wr).setFloat("b", -1.0f);
        c->net.node(c->xf2).setFloat("ty", 9.0f);
    }
    expectSameGeometry(*fused.net.cookDisplay(), *plain.net.cookDisplay());
    CHECK(fused.net.compiler().compileCount() == 1);
}

TEST(fusionStopsAtFanOutAndDisplayNode) {
    Chain c(true);
    NodeId extra = c.net.addNode("null");
    c.net.connect(c.wr, extra);
    c.net.cook(c.out);
    CHECK(c.net.node(c.wr).cookCount() == 1);
    CHECK(c.net.node(c.xf1).cookCount() == 0);
    CHECK(c.net.fusedChainCount() == 1);

    Chain d(true);
    d.net.setDisplayNode(d.wr);
    d.net.cook(d.xf2);
    CHECK(d.net.node(d.wr).cookCount() == 1);
    CHECK(d.net.node(d.xf1).cookCount() == 0);
}

TEST(fusionReusesCachedUpstreamAndEditingMiddlePropagates) {
    Chain c(true);
    c.net.cook(c.wr);
    CHECK(c.net.node(c.wr).cookCount() == 1);
    auto before = c.net.cookDisplay();
    CHECK(c.net.node(c.wr).cookCount() == 1);
    c.net.node(c.wr).setFloat("a", -100.0f);
    auto after = c.net.cookDisplay();
    CHECK(before.get() != after.get());
    Chain plain(false);
    plain.net.node(plain.wr).setFloat("a", -100.0f);
    expectSameGeometry(*after, *plain.net.cookDisplay());
}

TEST(emptyGeometryIsSafe) {
    KernelCompiler compiler;
    Geometry g;
    compiler.compile(parseWrangle("x = x + 1;", {}))->run(g, {});
}

TEST(networkCooksAndCaches) {
    Network net;
    NodeId line = net.addNode("line", "line");
    NodeId xf = net.addNode("transform", "xf");
    NodeId out = net.addNode("null", "out");
    net.node(line).setFloat("count", 16.0f);
    net.node(line).setFloat("length", 15.0f);
    net.connect(line, xf);
    net.connect(xf, out);
    net.setDisplayNode(out);
    net.node(xf).setFloat("sx", 2.0f);
    net.node(xf).setFloat("ty", 1.0f);

    auto geo = net.cookDisplay();
    CHECK(geo->pointCount() == 16);
    CHECK_NEAR(geo->x()[15], 30.0f);
    CHECK_NEAR(geo->y()[0], 1.0f);

    net.cookDisplay();
    CHECK(net.node(line).cookCount() == 1 && net.node(xf).cookCount() == 1);

    net.node(xf).setFloat("tx", 5.0f);
    geo = net.cookDisplay();
    CHECK_NEAR(geo->x()[0], 5.0f);
    CHECK(net.node(line).cookCount() == 1);
    CHECK(net.node(xf).cookCount() == 2 && net.node(out).cookCount() == 2);
    CHECK(net.compiler().compileCount() == 1);

    net.node(line).setFloat("count", 8.0f);
    CHECK(net.cookDisplay()->pointCount() == 8);
    CHECK(net.node(line).cookCount() == 2);
}

TEST(networkRejectsCyclesAndMissingInputs) {
    Network net;
    NodeId a = net.addNode("null");
    NodeId b = net.addNode("null");
    net.connect(a, b);
    bool threw = false;
    try { net.connect(b, a); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { net.cook(b); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

TEST(networkMergeAndWrangle) {
    Network net;
    NodeId l1 = net.addNode("line");
    NodeId l2 = net.addNode("line");
    NodeId merge = net.addNode("merge");
    NodeId wr = net.addNode("wrangle");
    net.node(l1).setFloat("count", 8.0f);
    net.node(l2).setFloat("count", 5.0f);
    net.node(l2).setFloat("y", 3.0f);
    net.connect(l1, merge, 0);
    net.connect(l2, merge, 1);
    net.connect(merge, wr);
    net.node(wr).setString("code", "y = y * a + 1;");
    net.node(wr).setFloat("a", 2.0f);

    auto geo = net.cook(wr);
    CHECK(geo->pointCount() == 13 && geo->primitives().size() == 2);
    CHECK_NEAR(geo->y()[0], 1.0f);
    CHECK_NEAR(geo->y()[8], 7.0f);

    net.node(wr).setString("code", "y = y + undefinedChannel;");
    CHECK(net.cook(wr)->pointCount() == 13);
    CHECK(net.executionOrder(wr).size() == 4);
}

}  // namespace

int main() {
    int failed = 0;
    for (const auto& t : tests()) {
        try {
            t.fn();
            std::printf("[ ok ] %s\n", t.name);
        } catch (const std::exception& e) {
            ++failed;
            std::printf("[FAIL] %s: %s\n", t.name, e.what());
        }
    }
    std::printf("%zu tests, %d failed\n", tests().size(), failed);
    return failed ? 1 : 0;
}
