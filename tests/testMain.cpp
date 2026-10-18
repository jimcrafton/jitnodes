#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
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
    CHECK(a.paddedSize() == simdLanes);
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
    b.addPrimitive({0, 2, false});
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
    for (const auto& [name, arrayA] : a.attribs(AttribClass::point)) {
        const auto& arrayB = *b.findAttrib(AttribClass::point, name);
        for (size_t i = 0; i < a.pointCount(); ++i) {
            if (std::fabs(arrayA.at(i) - arrayB.at(i)) >= 1e-4f) {
                std::string msg = name + "[" + std::to_string(i) + "] mismatch;";
                for (const auto& [n2, a2] : a.attribs(AttribClass::point))
                    msg += " " + n2 + ": jit=" + std::to_string(a2.at(i)) + " interp=" + std::to_string(b.findAttrib(AttribClass::point, n2)->at(i));
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
    CHECK(a.attribs(AttribClass::point).size() == b.attribs(AttribClass::point).size());
    for (const auto& [name, array] : a.attribs(AttribClass::point))
        for (size_t i = 0; i < a.pointCount(); ++i) CHECK_NEAR(array.at(i), b.findAttrib(AttribClass::point, name)->at(i));
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

TEST(intAttributeKernelsConvertAtBoundaries) {
    Geometry g;
    g.resizePoints(21);
    AttribArray& id = g.attrib(AttribClass::point, "id", AttribType::int32);
    for (size_t i = 0; i < 21; ++i) {
        id.ints()[i] = static_cast<int32_t>(i) - 5;
        g.x()[i] = static_cast<float>(i) * 0.75f;
    }
    KernelSpec spec = parseWrangle("id = id * 2 + 1; x = x + id; w = id / 2;", {});
    for (bool jit : {true, false}) {
        Geometry copy = g;
        KernelCompiler compiler;
        compiler.setJitEnabled(jit);
        compiler.compileFor(spec, copy, AttribClass::point)->run(copy, {});
        const AttribArray* out = copy.findAttrib(AttribClass::point, "id");
        CHECK(out->type() == AttribType::int32);
        for (size_t i = 0; i < 21; ++i) {
            const int32_t expected = (static_cast<int32_t>(i) - 5) * 2 + 1;
            CHECK(out->ints()[i] == expected);
            CHECK_NEAR(copy.x()[i], static_cast<float>(i) * 0.75f + static_cast<float>(expected));
            CHECK_NEAR(copy.findAttrib(AttribClass::point, "w")->at(i), static_cast<float>(expected) / 2.0f);
        }
    }
}

TEST(intStoreTruncatesTowardZero) {
    Geometry g;
    g.resizePoints(8);
    g.attrib(AttribClass::point, "n", AttribType::int32);
    for (size_t i = 0; i < 8; ++i) g.x()[i] = (static_cast<float>(i) - 3.5f) * 0.9f;
    KernelSpec spec = parseWrangle("n = x;", {});
    for (bool jit : {true, false}) {
        Geometry copy = g;
        KernelCompiler compiler;
        compiler.setJitEnabled(jit);
        compiler.compileFor(spec, copy, AttribClass::point)->run(copy, {});
        for (size_t i = 0; i < 8; ++i) CHECK(copy.findAttrib(AttribClass::point, "n")->ints()[i] == static_cast<int32_t>(g.x()[i]));
    }
}

TEST(kernelRejectsTypeMismatch) {
    Geometry g;
    g.resizePoints(8);
    g.attrib(AttribClass::point, "n", AttribType::int32);
    KernelCompiler compiler;
    auto floatKernel = compiler.compile(parseWrangle("n = n + 1;", {}));
    bool threw = false;
    try { floatKernel->run(g, {}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

TEST(primitiveAttributesAndKernels) {
    Network net;
    NodeId l1 = net.addNode("line");
    NodeId l2 = net.addNode("line");
    NodeId merge = net.addNode("merge");
    NodeId create = net.addNode("attribCreate");
    NodeId wr = net.addNode("wrangle");
    net.node(l1).setFloat("count", 5.0f);
    net.node(l2).setFloat("count", 3.0f);
    net.connect(l1, merge, 0);
    net.connect(l2, merge, 1);
    net.connect(merge, create);
    net.node(create).setString("name", "weight");
    net.node(create).setString("class", "primitive");
    net.node(create).setFloat("value", 4.0f);
    net.connect(create, wr);
    net.node(wr).setString("class", "primitive");
    net.node(wr).setString("code", "weight = weight * 2 + a;");
    net.node(wr).setFloat("a", 1.0f);

    auto geo = net.cook(wr);
    CHECK(geo->primitiveCount() == 2);
    const AttribArray* w = geo->findAttrib(AttribClass::primitive, "weight");
    CHECK(w && w->size() == 2);
    CHECK_NEAR(w->at(0), 9.0f);
    CHECK_NEAR(w->at(1), 9.0f);
    CHECK(geo->pointCount() == 8);
}

TEST(appendMergesPrimitiveAttributesAndGroups) {
    Geometry a, b;
    a.resizePoints(2);
    a.addPrimitive({0, 2, false});
    a.attrib(AttribClass::primitive, "tag").floats()[0] = 1.0f;
    b.resizePoints(3);
    b.addPrimitive({0, 3, true});
    b.attrib(AttribClass::primitive, "tag").floats()[0] = 2.0f;
    b.createGroup(AttribClass::point, "top").ints()[2] = 1;
    a.append(b);
    CHECK(a.primitiveCount() == 2);
    CHECK_NEAR(a.findAttrib(AttribClass::primitive, "tag")->at(1), 2.0f);
    CHECK(a.inGroup(AttribClass::point, "top", 4));
    CHECK(!a.inGroup(AttribClass::point, "top", 3));
    bool threw = false;
    Geometry c;
    c.attrib(AttribClass::point, "tag", AttribType::int32);
    Geometry d;
    d.attrib(AttribClass::point, "tag");
    try { c.append(d); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

TEST(groupsAndVectorAttributesInKernels) {
    Network net;
    NodeId line = net.addNode("line");
    NodeId create = net.addNode("attribCreate");
    NodeId wr = net.addNode("wrangle");
    net.node(line).setFloat("count", 10.0f);
    net.connect(line, create);
    net.node(create).setString("name", "group.right");
    net.node(create).setString("type", "int");
    net.connect(create, wr);
    net.node(wr).setString("code", "group.right = x > a; N.x = x * 0.5; N.y = 1;");
    net.node(wr).setFloat("a", 4.0f);
    auto geo = net.cook(wr);
    int members = 0;
    for (size_t i = 0; i < geo->pointCount(); ++i) {
        const bool expected = geo->x()[i] > 4.0f;
        CHECK(geo->inGroup(AttribClass::point, "right", i) == expected);
        members += expected;
        CHECK_NEAR(geo->findAttrib(AttribClass::point, "N.x")->at(i), geo->x()[i] * 0.5f);
    }
    CHECK(members == 6);
    CHECK(geo->findAttrib(AttribClass::point, "group.right")->ints()[9] == 1);
    Geometry g;
    g.resizePoints(3);
    g.addVectorAttrib(AttribClass::point, "v", 3);
    CHECK(g.hasAttrib(AttribClass::point, "v.z") && !g.hasAttrib(AttribClass::point, "v.w"));
}

TEST(fusionDoesNotCrossAttributeClasses) {
    Network net;
    NodeId line = net.addNode("line");
    NodeId create = net.addNode("attribCreate");
    NodeId pointWr = net.addNode("wrangle");
    NodeId primWr = net.addNode("wrangle");
    NodeId out = net.addNode("null");
    net.node(create).setString("name", "k");
    net.node(create).setString("class", "primitive");
    net.node(pointWr).setString("code", "x = x + 1;");
    net.node(primWr).setString("class", "primitive");
    net.node(primWr).setString("code", "k = k + 2;");
    net.connect(line, create);
    net.connect(create, pointWr);
    net.connect(pointWr, primWr);
    net.connect(primWr, out);
    net.setDisplayNode(out);
    auto geo = net.cookDisplay();
    CHECK(net.fusedChainCount() == 0);
    CHECK(net.node(pointWr).cookCount() == 1);
    CHECK_NEAR(geo->findAttrib(AttribClass::primitive, "k")->at(0), 2.0f);
    CHECK_NEAR(geo->x()[0], 1.0f);
}

TEST(fusedChainKeepsIntegerAttributeTypes) {
    Network net;
    NodeId line = net.addNode("line");
    NodeId create = net.addNode("attribCreate");
    NodeId w1 = net.addNode("wrangle");
    NodeId w2 = net.addNode("wrangle");
    NodeId out = net.addNode("null");
    net.node(line).setFloat("count", 9.0f);
    net.node(create).setString("name", "id");
    net.node(create).setString("type", "int");
    net.node(create).setFloat("value", 3.0f);
    net.node(w1).setString("code", "id = id * 3 + 1;");
    net.node(w2).setString("code", "id = id / 2; w = id;");
    net.connect(line, create);
    net.connect(create, w1);
    net.connect(w1, w2);
    net.connect(w2, out);
    net.setDisplayNode(out);
    auto geo = net.cookDisplay();
    CHECK(net.fusedChainCount() == 1);
    const AttribArray* id = geo->findAttrib(AttribClass::point, "id");
    CHECK(id->type() == AttribType::int32 && id->ints()[0] == 5);
    CHECK_NEAR(geo->findAttrib(AttribClass::point, "w")->at(0), 5.0f);
}

TEST(resampleOpenLineKeepsEndpointsAndInterpolatesAttributes) {
    Network net;
    NodeId line = net.addNode("line");
    NodeId create = net.addNode("attribCreate");
    NodeId wr = net.addNode("wrangle");
    NodeId rs = net.addNode("resample");
    net.node(line).setFloat("count", 5.0f);
    net.node(line).setFloat("length", 4.0f);
    net.node(create).setString("name", "id");
    net.node(create).setString("type", "int");
    net.node(wr).setString("code", "w = x * 10; id = x;");
    net.node(rs).setFloat("count", 9.0f);
    net.connect(line, create);
    net.connect(create, wr);
    net.connect(wr, rs);
    auto geo = net.cook(rs);
    CHECK(geo->pointCount() == 9 && geo->primitiveCount() == 1);
    CHECK(geo->primitives()[0].count == 9 && !geo->primitives()[0].closed);
    for (size_t i = 0; i < 9; ++i) {
        CHECK_NEAR(geo->x()[i], static_cast<float>(i) * 0.5f);
        CHECK_NEAR(geo->findAttrib(AttribClass::point, "w")->at(i), static_cast<float>(i) * 5.0f);
    }
    const AttribArray* id = geo->findAttrib(AttribClass::point, "id");
    CHECK(id->type() == AttribType::int32 && id->ints()[8] == 4 && id->ints()[1] == 1);

    net.node(rs).setFloat("spacing", 1.0f);
    CHECK(net.cook(rs)->pointCount() == 5);
}

TEST(resampleAndSubdivideClosedCurves) {
    Network net;
    NodeId circle = net.addNode("circle");
    NodeId rs = net.addNode("resample");
    NodeId sub = net.addNode("subdivide");
    net.node(circle).setFloat("count", 8.0f);
    net.node(circle).setFloat("radius", 2.0f);
    net.node(rs).setFloat("count", 20.0f);
    net.node(sub).setFloat("iterations", 2.0f);
    net.connect(circle, rs);
    net.connect(rs, sub);

    auto resampled = net.cook(rs);
    CHECK(resampled->pointCount() == 20 && resampled->primitives()[0].closed);
    CHECK_NEAR(resampled->x()[0], 2.0f);
    CHECK(std::fabs(resampled->x()[19] - 2.0f) > 0.1f);

    auto subdivided = net.cook(sub);
    CHECK(subdivided->pointCount() == 80 && subdivided->primitives()[0].count == 80);
    for (size_t i = 0; i < 80; ++i) {
        const float r = std::hypot(subdivided->x()[i], subdivided->y()[i]);
        CHECK(r > 1.5f && r < 2.01f);
    }
}

TEST(subdivideOpenLineUsesCornerCutting) {
    Network net;
    NodeId line = net.addNode("line");
    NodeId sub = net.addNode("subdivide");
    net.node(line).setFloat("count", 3.0f);
    net.node(line).setFloat("length", 2.0f);
    net.connect(line, sub);
    auto geo = net.cook(sub);
    CHECK(geo->pointCount() == 4);
    const float expected[4] = {0.0f, 0.75f, 1.25f, 2.0f};
    for (size_t i = 0; i < 4; ++i) CHECK_NEAR(geo->x()[i], expected[i]);
}

TEST(copyToPointsInstancesWithTransform) {
    Network net;
    NodeId source = net.addNode("line");
    NodeId targets = net.addNode("line");
    NodeId copy = net.addNode("copyToPoints");
    net.node(source).setFloat("count", 3.0f);
    net.node(source).setFloat("length", 2.0f);
    net.node(targets).setFloat("count", 4.0f);
    net.node(targets).setFloat("length", 3.0f);
    net.connect(source, copy, 0);
    net.connect(targets, copy, 1);
    auto geo = net.cook(copy);
    CHECK(geo->pointCount() == 12 && geo->primitiveCount() == 4);
    CHECK(geo->primitives()[2].start == 6);
    CHECK_NEAR(geo->x()[2 * 3 + 2], 2.0f + 2.0f);
    CHECK(geo->findAttrib(AttribClass::point, "copyIndex")->ints()[11] == 3);
    CHECK(geo->findAttrib(AttribClass::primitive, "copyIndex")->ints()[3] == 3);

    net.node(copy).setFloat("angle", 1.57079632679f);
    net.node(copy).setFloat("scale", 2.0f);
    geo = net.cook(copy);
    CHECK_NEAR(geo->x()[3 * 1 + 2], 1.0f);
    CHECK_NEAR(geo->y()[3 * 1 + 2], 4.0f);
}

TEST(copyToPointsReadsTargetScaleAttribute) {
    Network net;
    NodeId source = net.addNode("line");
    NodeId targets = net.addNode("line");
    NodeId wr = net.addNode("wrangle");
    NodeId copy = net.addNode("copyToPoints");
    net.node(source).setFloat("count", 2.0f);
    net.node(source).setFloat("length", 1.0f);
    net.node(targets).setFloat("count", 3.0f);
    net.node(targets).setFloat("length", 10.0f);
    net.node(wr).setString("code", "scale = x * 0.1 + 1;");
    net.connect(targets, wr);
    net.connect(source, copy, 0);
    net.connect(wr, copy, 1);
    auto geo = net.cook(copy);
    CHECK_NEAR(geo->x()[5], 10.0f + 2.0f);
    CHECK_NEAR(geo->x()[3], 5.0f + 1.5f);
}

TEST(blastDeletesPointGroupsAndCompactsPrimitives) {
    Network net;
    NodeId line = net.addNode("line");
    NodeId create = net.addNode("attribCreate");
    NodeId wr = net.addNode("wrangle");
    NodeId blast = net.addNode("blast");
    net.node(line).setFloat("count", 10.0f);
    net.node(create).setString("name", "group.cut");
    net.node(create).setString("type", "int");
    net.node(wr).setString("code", "group.cut = x > 4;");
    net.node(blast).setString("group", "cut");
    net.connect(line, create);
    net.connect(create, wr);
    net.connect(wr, blast);
    auto geo = net.cook(blast);
    CHECK(geo->pointCount() == 4 && geo->primitiveCount() == 1 && geo->primitives()[0].count == 4);
    CHECK(geo->x()[3] < 4.0f);

    net.node(blast).setFloat("keepGroup", 1.0f);
    geo = net.cook(blast);
    CHECK(geo->pointCount() == 6 && geo->x()[0] > 4.0f);
}

TEST(blastDeletesPrimitivesWithTheirPoints) {
    Network net;
    NodeId l1 = net.addNode("line");
    NodeId l2 = net.addNode("line");
    NodeId c1 = net.addNode("attribCreate");
    NodeId c2 = net.addNode("attribCreate");
    NodeId merge = net.addNode("merge");
    NodeId blast = net.addNode("blast");
    net.node(l1).setFloat("count", 5.0f);
    net.node(l2).setFloat("count", 3.0f);
    net.node(l2).setFloat("y", 7.0f);
    for (NodeId c : {c1, c2}) {
        net.node(c).setString("name", "group.drop");
        net.node(c).setString("class", "primitive");
        net.node(c).setString("type", "int");
    }
    net.node(c1).setFloat("value", 1.0f);
    net.connect(l1, c1);
    net.connect(l2, c2);
    net.connect(c1, merge, 0);
    net.connect(c2, merge, 1);
    net.connect(merge, blast);
    net.node(blast).setString("group", "drop");
    net.node(blast).setString("class", "primitive");
    auto geo = net.cook(blast);
    CHECK(geo->primitiveCount() == 1 && geo->pointCount() == 3);
    CHECK_NEAR(geo->y()[0], 7.0f);
    CHECK(geo->primitives()[0].start == 0 && geo->primitives()[0].count == 3);
}

TEST(topologyNodesWorkWithFusedKernelsDownstream) {
    auto build = [](Network& net) {
        NodeId circle = net.addNode("circle");
        NodeId sub = net.addNode("subdivide");
        NodeId w1 = net.addNode("wrangle");
        NodeId w2 = net.addNode("transform");
        net.node(sub).setFloat("iterations", 3.0f);
        net.node(w1).setString("code", "x = x * 2;");
        net.node(w2).setFloat("tx", 1.0f);
        net.connect(circle, sub);
        net.connect(sub, w1);
        net.connect(w1, w2);
        return w2;
    };
    Network fused, plain;
    plain.setFusionEnabled(false);
    auto a = fused.cook(build(fused));
    auto b = plain.cook(build(plain));
    CHECK(fused.fusedChainCount() == 1 && plain.fusedChainCount() == 0);
    CHECK(a->pointCount() == 32 * 8);
    expectSameGeometry(*a, *b);
}

TEST(keyframeInterpolationModes) {
    std::vector<Keyframe> keys = {{2.0, 10.0f, KeyInterp::linear}, {0.0, 0.0f, KeyInterp::linear},
                                  {4.0, 20.0f, KeyInterp::constant}, {6.0, 0.0f, KeyInterp::smooth}};
    sortKeyframes(keys);
    CHECK(keys.front().time == 0.0);
    CHECK_NEAR(evaluateKeyframes(keys, -3.0), 0.0f);
    CHECK_NEAR(evaluateKeyframes(keys, 1.0), 5.0f);
    CHECK_NEAR(evaluateKeyframes(keys, 3.0), 15.0f);
    CHECK_NEAR(evaluateKeyframes(keys, 5.0), 20.0f);
    CHECK_NEAR(evaluateKeyframes(keys, 9.0), 0.0f);
    CHECK_NEAR(evaluateKeyframes({{0.0, 0.0f, KeyInterp::smooth}, {1.0, 8.0f, KeyInterp::linear}}, 0.5), 4.0f);
    CHECK_NEAR(evaluateKeyframes({{0.0, 0.0f, KeyInterp::smooth}, {1.0, 8.0f, KeyInterp::linear}}, 0.25), 8.0f * 0.15625f);
    CHECK(evaluateKeyframes({}, 1.0) == 0.0f);
}

TEST(scalarMathOpsMatchBetweenJitAndInterpreter) {
    expectJitMatchesInterpreter("x = floor(x * 0.7) + ceil(y * 0.3); w = floor(-x) - ceil(-y * 1.5);", {}, {});
    KernelSpec spec = parseWrangle("x = sin(x * 0.1) + cos(y) * exp(0.1 * x) + pow(abs(x) + 1, 0.5) + log(abs(x) + 1);", {});
    KernelCompiler compiler;
    Geometry g = makeRamp(16), ref = makeRamp(16);
    auto kernel = compiler.compile(spec);
    CHECK(!compiler.jitSupported() || kernel->isJitted());
    kernel->run(g, {});
    for (size_t i = 0; i < 16; ++i) {
        const float x = ref.x()[i], y = ref.y()[i];
        const float expected = std::sin(x * 0.1f) + std::cos(y) * std::exp(0.1f * x) + std::pow(std::fabs(x) + 1.0f, 0.5f) +
                               std::log(std::fabs(x) + 1.0f);
        CHECK_NEAR(g.x()[i], expected);
    }
}

TEST(expressionParametersFollowTime) {
    Network net;
    NodeId line = net.addNode("line");
    NodeId xf = net.addNode("transform");
    net.node(line).setFloat("count", 4.0f);
    net.node(line).setFloat("length", 3.0f);
    net.node(xf).setExpression("tx", "time * 2 + sin(pi / 2)");
    net.connect(line, xf);

    CHECK_NEAR(net.cook(xf)->x()[0], 1.0f);
    net.setTime(1.5);
    CHECK_NEAR(net.cook(xf)->x()[0], 4.0f);
    CHECK(net.node(line).cookCount() == 1);
    CHECK(net.node(xf).cookCount() == 2);
    CHECK(net.dependsOnTime(xf) && !net.dependsOnTime(line));

    net.node(xf).setExpression("sx", "frame / 12");
    net.setFps(24.0);
    net.setTime(0.5);
    CHECK_NEAR(net.cook(xf)->x()[3], 3.0f * 1.0f + 2.0f * 0.5f + 1.0f);
    CHECK_NEAR(net.node(xf).getFloat("sx"), 1.0f);

    net.setTime(0.5);
    net.cook(xf);
    CHECK(net.node(xf).cookCount() == 3);
}

TEST(keyframedParametersAnimateAndClear) {
    Network net;
    NodeId line = net.addNode("line");
    NodeId xf = net.addNode("transform");
    net.node(line).setFloat("count", 2.0f);
    net.node(line).setFloat("length", 1.0f);
    net.connect(line, xf);
    net.node(xf).setKeyframes("ty", {{0.0, 0.0f, KeyInterp::linear}, {2.0, 10.0f, KeyInterp::linear}});
    CHECK(net.node(xf).isAnimated("ty"));
    net.setTime(1.0);
    CHECK_NEAR(net.cook(xf)->y()[0], 5.0f);
    net.setTime(5.0);
    CHECK_NEAR(net.cook(xf)->y()[1], 10.0f);

    net.node(xf).setFloat("ty", 3.0f);
    CHECK_NEAR(net.cook(xf)->y()[0], 10.0f);
    net.node(xf).clearAnimation("ty");
    CHECK(!net.node(xf).isAnimated("ty"));
    CHECK_NEAR(net.cook(xf)->y()[0], 3.0f);
    net.setTime(0.0);
    CHECK(!net.dependsOnTime(xf));
}

TEST(expressionsReferenceOtherNodesAndPropagateChanges) {
    Network net;
    NodeId line = net.addNode("line", "src");
    NodeId a = net.addNode("transform", "a");
    NodeId b = net.addNode("transform", "b");
    net.node(line).setFloat("count", 2.0f);
    net.node(line).setFloat("length", 1.0f);
    net.connect(line, a);
    net.connect(a, b);
    net.node(a).setFloat("tx", 2.0f);
    net.node(b).setExpression("tx", "a.tx * 10 + sx");
    net.node(b).setFloat("sx", 1.0f);

    CHECK_NEAR(net.cook(b)->x()[0], 2.0f + 21.0f);
    net.node(a).setFloat("tx", 3.0f);
    CHECK_NEAR(net.cook(b)->x()[0], 3.0f + 31.0f);

    NodeId c = net.addNode("transform", "c");
    NodeId d = net.addNode("transform", "d");
    net.connect(line, c);
    net.connect(line, d);
    net.node(c).setFloat("tx", 1.0f);
    net.node(d).setExpression("ty", "c.tx + 4");
    CHECK_NEAR(net.cook(d)->y()[0], 5.0f);
    const size_t before = net.node(d).cookCount();
    net.node(c).setFloat("tx", 6.0f);
    CHECK_NEAR(net.cook(d)->y()[0], 10.0f);
    CHECK(net.node(d).cookCount() == before + 1);
}

TEST(timeDependenceIsTransitiveThroughReferences) {
    Network net;
    NodeId line = net.addNode("line", "src");
    NodeId a = net.addNode("transform", "a");
    NodeId b = net.addNode("transform", "b");
    net.connect(line, a);
    net.connect(line, b);
    net.node(a).setExpression("tx", "time * 3");
    net.node(b).setExpression("tx", "a.tx + 1");
    CHECK(net.dependsOnTime(b));
    net.cook(b);
    net.setTime(2.0);
    CHECK_NEAR(net.cook(b)->x()[0], 7.0f);
    CHECK(!net.dependsOnTime(line));
}

TEST(expressionErrorsAreReported) {
    Network net;
    NodeId line = net.addNode("line", "src");
    NodeId a = net.addNode("transform", "a");
    NodeId b = net.addNode("transform", "b");
    net.connect(line, a);
    net.connect(line, b);

    bool threw = false;
    try { net.node(a).setExpression("tx", "1 +"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && !net.node(a).isAnimated("tx"));

    net.node(a).setExpression("tx", "nosuchparam + 1");
    threw = false;
    try { net.cook(a); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    net.node(a).setExpression("tx", "ghost.tx");
    threw = false;
    try { net.cook(a); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    net.node(a).setExpression("tx", "b.tx");
    net.node(b).setExpression("tx", "a.tx");
    threw = false;
    try { net.cook(b); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    net.node(b).clearAnimation("tx");
    CHECK_NEAR(net.cook(b)->x()[0], 0.0f);
}

TEST(animatedParametersFuseLikeStaticOnes) {
    Chain fused(true), plain(false);
    for (Chain* c : {&fused, &plain}) {
        c->net.node(c->xf1).setExpression("tx", "time * 2");
        c->net.node(c->wr).setKeyframes("b", {{0.0, 1.0f, KeyInterp::linear}, {4.0, 5.0f, KeyInterp::linear}});
        c->net.node(c->xf2).setExpression("ty", c->net.node(c->xf1).name() + ".tx + 1");
    }
    for (double t : {0.0, 1.0, 2.5}) {
        fused.net.setTime(t);
        plain.net.setTime(t);
        expectSameGeometry(*fused.net.cookDisplay(), *plain.net.cookDisplay());
    }
    CHECK(fused.net.fusedChainCount() == 3);
    CHECK(fused.net.compiler().compileCount() == 1);
}

TEST(threadPoolRunsEveryIndexNestsAndPropagatesErrors) {
    ThreadPool pool(3);
    std::atomic<int> sum{0};
    pool.parallelFor(1000, [&](size_t i) { sum += static_cast<int>(i); });
    CHECK(sum == 999 * 1000 / 2);

    std::atomic<int> nested{0};
    pool.parallelFor(8, [&](size_t) { pool.parallelFor(50, [&](size_t) { ++nested; }); });
    CHECK(nested == 400);

    ThreadPool inlinePool(0);
    int count = 0;
    inlinePool.parallelFor(5, [&](size_t) { ++count; });
    CHECK(count == 5 && inlinePool.concurrency() == 1);

    bool threw = false;
    std::atomic<int> ran{0};
    try {
        pool.parallelFor(64, [&](size_t i) {
            ++ran;
            if (i == 10) throw std::runtime_error("boom");
        });
    } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && ran == 64);
    pool.parallelFor(4, [&](size_t) {});
}

TEST(parallelKernelMatchesSerialExecution) {
    const size_t n = 200003;
    Geometry base;
    base.resizePoints(n);
    AttribArray& ids = base.attrib(AttribClass::point, "id", AttribType::int32);
    for (size_t i = 0; i < n; ++i) {
        base.x()[i] = static_cast<float>(i % 977) * 0.25f;
        base.y()[i] = static_cast<float>(i % 13);
        ids.ints()[i] = static_cast<int32_t>(i % 100);
    }
    KernelSpec spec = parseWrangle("y = (x > a ? y * 2 : -y) + id; id = id + 1; x = x * 0.5 + floor(y);", {"a"});
    KernelCompiler compiler;
    auto kernel = compiler.compileFor(spec, base, AttribClass::point);
    ThreadPool pool(3);
    Geometry serial = base, parallel = base;
    kernel->run(serial, {100.0f}, AttribClass::point, nullptr);
    kernel->run(parallel, {100.0f}, AttribClass::point, &pool);
    for (const auto& [name, array] : serial.attribs(AttribClass::point)) {
        const AttribArray* other = parallel.findAttrib(AttribClass::point, name);
        CHECK(std::memcmp(array.data(), other->data(), n * 4) == 0);
    }
}

namespace {

struct Branches {
    Network net;
    NodeId out;
    Branches(size_t threads, size_t branches, float points) {
        net.setThreadCount(threads);
        std::vector<NodeId> tails;
        for (size_t b = 0; b < branches; ++b) {
            NodeId line = net.addNode("line");
            NodeId xf = net.addNode("transform");
            NodeId wr = net.addNode("wrangle");
            net.node(line).setFloat("count", points + static_cast<float>(b));
            net.node(line).setFloat("length", 10.0f);
            net.node(line).setFloat("y", static_cast<float>(b));
            net.node(xf).setFloat("sx", 1.0f + static_cast<float>(b));
            net.node(wr).setString("code", "y = y + x * a; w = sqrt(abs(x)) + b;");
            net.node(wr).setFloat("a", 0.5f);
            net.node(wr).setFloat("b", static_cast<float>(b));
            net.connect(line, xf);
            net.connect(xf, wr);
            tails.push_back(wr);
        }
        NodeId current = tails[0];
        for (size_t b = 1; b < branches; ++b) {
            NodeId merge = net.addNode("merge");
            net.connect(current, merge, 0);
            net.connect(tails[b], merge, 1);
            current = merge;
        }
        out = net.addNode("null");
        net.connect(current, out);
        net.setDisplayNode(out);
    }
};

}  // namespace

TEST(parallelBranchCookingMatchesSingleThread) {
    Branches single(1, 6, 70000.0f), threaded(4, 6, 70000.0f);
    CHECK(single.net.threadCount() == 1 && threaded.net.threadCount() == 4);
    expectSameGeometry(*single.net.cookDisplay(), *threaded.net.cookDisplay());
    for (size_t round = 0; round < 5; ++round) {
        for (Branches* b : {&single, &threaded}) {
            const NodeId wr = b->net.findNode("wrangle" + std::to_string(3 * (round + 1)));
            b->net.node(wr).setFloat("a", 1.0f + static_cast<float>(round));
        }
        expectSameGeometry(*single.net.cookDisplay(), *threaded.net.cookDisplay());
    }
    CHECK(threaded.net.compiler().compileCount() == single.net.compiler().compileCount());
}

TEST(profilingRecordsCooksAndFusion) {
    Chain c(true);
    c.net.cookDisplay();
    const NodeStats& tail = c.net.node(c.xf2).stats();
    CHECK(tail.cooks == 1 && tail.lastChainLength == 3 && tail.fusedInto == invalidNodeId);
    CHECK(tail.lastCookMs >= tail.lastKernelMs && tail.totalCookMs >= tail.lastCookMs);
    CHECK(c.net.node(c.xf1).stats().fusedInto == c.xf2);
    CHECK(c.net.node(c.wr).stats().fusedInto == c.xf2);
    CHECK(c.net.node(c.line).stats().cooks == 1);
    CHECK(c.net.compiler().compileMillis() > 0.0);

    const std::string report = c.net.profileReport();
    CHECK(report.find("fused into") != std::string::npos);
    CHECK(report.find("fused chain of 3") != std::string::npos);
    CHECK(report.find("kernels compiled: 1") != std::string::npos);

    c.net.resetProfile();
    CHECK(c.net.node(c.xf2).stats().cooks == 0);
    CHECK(c.net.node(c.xf2).cookCount() == 1);
}

namespace {

class OffsetXNode : public Node {
public:
    OffsetXNode() : Node("offsetX", 1) { declareFloat("amount", 1.0f); }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        auto geo = std::make_shared<Geometry>(*in[0]);
        for (size_t i = 0; i < geo->pointCount(); ++i) geo->x()[i] += getFloat("amount");
        return geo;
    }
};

bool throwsRuntimeError(const std::function<void()>& fn, std::string* message = nullptr) {
    try {
        fn();
    } catch (const std::runtime_error& e) {
        if (message) *message = e.what();
        return true;
    }
    return false;
}

struct Showcase {
    Network net;
    NodeId line, create, wr, xf, out;
    Showcase() {
        line = net.addNode("line", "src");
        create = net.addNode("attribCreate", "tag");
        NodeId removed = net.addNode("null", "scratch");
        wr = net.addNode("wrangle", "wr");
        xf = net.addNode("transform", "xf");
        out = net.addNode("null", "out");
        net.removeNode(removed);
        net.node(line).setFloat("count", 17.0f);
        net.node(line).setFloat("length", 0.123456789f);
        net.node(create).setString("name", "id");
        net.node(create).setString("type", "int");
        net.node(create).setFloat("value", 3.0f);
        net.node(wr).setString("code", "y = y + id * a; w = sin(x * b);");
        net.node(wr).setKeyframes("a", {{0.0, 0.0f, KeyInterp::smooth}, {2.0, 1.0f / 3.0f, KeyInterp::constant}, {4.0, 0.1f, KeyInterp::linear}});
        net.node(wr).setFloat("b", 0.1f);
        net.node(xf).setExpression("tx", "time * 2 + src.length");
        net.node(xf).setFloat("sy", 2.0f);
        net.connect(line, create);
        net.connect(create, wr);
        net.connect(wr, xf);
        net.connect(xf, out);
        net.setDisplayNode(out);
    }
};

}  // namespace

TEST(saveAndLoadRoundTripPreservesGraphAndResults) {
    NodeRegistry::instance().registerType("offsetX", [] { return std::make_unique<OffsetXNode>(); });
    Showcase original;
    original.net.setTime(0.75);
    const std::string text = saveNetwork(original.net);

    Network loaded(1);
    loadNetwork(loaded, text);
    CHECK(loaded.nodeIds() == original.net.nodeIds());
    CHECK(loaded.displayNode() == original.out && loaded.time() == 0.75);
    CHECK(loaded.findNode("wr") == original.wr);
    CHECK(loaded.node(original.wr).baseFloat("b") == 0.1f);
    CHECK(loaded.node(original.line).baseFloat("length") == 0.123456789f);
    CHECK(loaded.node(original.wr).keyframes("a").size() == 3);
    CHECK(loaded.node(original.wr).keyframes("a")[1].value == 1.0f / 3.0f);
    CHECK(loaded.node(original.xf).expression("tx") == "time * 2 + src.length");
    CHECK(saveNetwork(loaded) == text);

    for (double t : {0.0, 1.25, 3.0}) {
        original.net.setTime(t);
        loaded.setTime(t);
        expectSameGeometry(*original.net.cookDisplay(), *loaded.cookDisplay());
        CHECK(loaded.cookDisplay()->findAttrib(AttribClass::point, "id")->type() == AttribType::int32);
    }
    CHECK(text.find("\"format\": \"jitnodes\"") != std::string::npos);
}

TEST(loadAcceptsJson5AndReportsProblems) {
    Network net(1);
    const std::string json5Text = R"(// comment
{ format: 'jitnodes', version: 1, /* block */ display: 2,
  nodes: [ { id: 1, type: "line", name: "a", floats: { count: 4, }, inputs: [], },
           { id: 2, type: "null", name: "b", inputs: [1], }, ], })";
    loadNetwork(net, json5Text);
    CHECK(net.nodeIds().size() == 2 && net.cookDisplay()->pointCount() == 4);

    auto expectError = [&](const std::string& text, const std::string& needle) {
        std::string message;
        CHECK(throwsRuntimeError([&] { loadNetwork(net, text); }, &message));
        CHECK(message.find(needle) != std::string::npos);
        CHECK(net.nodeIds().size() == 2 && net.findNode("a") != invalidNodeId);
    };
    expectError("{ format: 'jitnodes', version: 1, nodes: [ {", "parse error");
    expectError("[1, 2]", "root must be an object");
    expectError("{ format: 'other', version: 1, nodes: [] }", "not a jitnodes graph");
    expectError("{ format: 'jitnodes', version: 7, nodes: [] }", "unsupported version 7");
    expectError("{ format: 'jitnodes', version: 1, nodes: [ { id: 1, type: 'nosuchtype', name: 'x', inputs: [] } ] }", "nosuchtype");
    expectError("{ format: 'jitnodes', version: 1, nodes: [ { id: 1, type: 'line', name: 'x', floats: { bogus: 1 }, inputs: [] } ] }", "bogus");
    expectError("{ format: 'jitnodes', version: 1, nodes: [ { id: 1, type: 'line', name: 'x', inputs: [] }, { id: 1, type: 'line', name: 'y', inputs: [] } ] }", "already in use");
    expectError("{ format: 'jitnodes', version: 1, nodes: [ { id: 1, type: 'null', name: 'x', inputs: [9] } ] }", "invalid node id");
    expectError("{ format: 'jitnodes', version: 1, nodes: [ { id: 1, type: 'null', name: 'x', inputs: [2] }, { id: 2, type: 'null', name: 'y', inputs: [1] } ] }", "cycle");
    expectError("{ format: 'jitnodes', version: 1, nodes: [ { id: 1, type: 'transform', name: 'x', expressions: { tx: '1 +' }, inputs: [0] } ] }", "'x'");
    expectError("{ format: 'jitnodes', version: 1, display: 5, nodes: [] }", "display");
}

TEST(historyUndoRedoRestoresGraphState) {
    Network net;
    NodeId line = net.addNode("line", "src");
    NodeId xf = net.addNode("transform", "xf");
    net.connect(line, xf);
    net.setDisplayNode(xf);
    History history(net);
    CHECK(!history.canUndo() && !history.canRedo());

    net.node(xf).setFloat("tx", 5.0f);
    CHECK(history.commit("move"));
    CHECK(!history.commit("no change"));
    NodeId wr = net.addNode("wrangle", "wr");
    net.connect(xf, wr);
    net.setDisplayNode(wr);
    net.node(wr).setString("code", "y = x;");
    CHECK(history.commit("add wrangle"));
    net.setTime(2.5);
    CHECK(history.size() == 3 && history.undoLabel() == "add wrangle");

    CHECK(history.undo());
    CHECK(net.nodeIds().size() == 2);
    CHECK(net.displayNode() == xf);
    CHECK(net.time() == 2.5);
    CHECK_NEAR(net.cookDisplay()->x()[0], 5.0f);
    CHECK(history.redoLabel() == "add wrangle" && history.undoLabel() == "move");
    CHECK(history.undo() && net.node(xf).baseFloat("tx") == 0.0f);
    CHECK(!history.undo());

    CHECK(history.redo() && history.redo() && !history.redo());
    CHECK(net.findNode("wr") == wr && net.node(wr).getString("code") == "y = x;");
    CHECK(net.inputOf(wr, 0) == xf);

    CHECK(history.undo());
    net.node(xf).setFloat("tx", 9.0f);
    CHECK(history.commit("branch"));
    CHECK(!history.canRedo() && history.size() == 3);

    History small(net, 3);
    for (int i = 0; i < 6; ++i) {
        net.node(xf).setFloat("tx", static_cast<float>(100 + i));
        small.commit("edit " + std::to_string(i));
    }
    CHECK(small.size() == 3);
    CHECK(small.undo() && small.undo() && !small.undo());
    CHECK(net.node(xf).baseFloat("tx") == 103.0f);
}

TEST(customNodeTypesRegisterAndSerialize) {
    NodeRegistry::instance().registerType("offsetX", [] { return std::make_unique<OffsetXNode>(); });
    Network net;
    NodeId line = net.addNode("line");
    NodeId custom = net.addNode("offsetX", "shift");
    net.connect(line, custom);
    net.node(custom).setFloat("amount", 2.5f);
    net.node(custom).setExpression("amount", "time + 2.5");
    net.setDisplayNode(custom);

    Network copy(1);
    loadNetwork(copy, saveNetwork(net));
    copy.setTime(1.0);
    CHECK_NEAR(copy.cookDisplay()->x()[0], 3.5f);
    const auto types = NodeRegistry::instance().typeNames();
    CHECK(std::find(types.begin(), types.end(), "offsetX") != types.end());
}

TEST(svgExportDrawsPrimitivesWithYUp) {
    Network net;
    NodeId circle = net.addNode("circle");
    NodeId line = net.addNode("line");
    NodeId merge = net.addNode("merge");
    net.node(circle).setFloat("count", 6.0f);
    net.node(line).setFloat("count", 3.0f);
    net.connect(circle, merge, 0);
    net.connect(line, merge, 1);
    const std::string svg = geometryToSvg(*net.cook(merge));
    CHECK(svg.find("<svg") == 0 && svg.find("</svg>") != std::string::npos);
    CHECK(svg.find("<polygon") != std::string::npos && svg.find("<polyline") != std::string::npos);
    SvgOptions options;
    options.drawPoints = true;
    options.background = "#fff";
    const std::string withPoints = geometryToSvg(*net.cook(merge), options);
    size_t circles = 0;
    for (size_t pos = 0; (pos = withPoints.find("<circle", pos)) != std::string::npos; ++pos) ++circles;
    CHECK(circles == 9 && withPoints.find("fill=\"#fff\"") != std::string::npos);
    CHECK(geometryToSvg(Geometry()).find("<svg") == 0);
}

TEST(exampleGraphLoadsAndAnimates) {
    Network net(1);
    loadNetworkFromFile(net, std::string(JITNODES_SOURCE_DIR) + "/examples/graphs/wobble.json");
    CHECK(net.nodeIds().size() == 5 && net.displayNode() == net.findNode("out"));
    net.setTime(0.0);
    Geometry atZero = *net.cookDisplay();
    net.setTime(1.5);
    auto later = net.cookDisplay();
    CHECK(atZero.pointCount() == 96 && later->pointCount() == 96);
    float maxDelta = 0.0f;
    for (size_t i = 0; i < 96; ++i) maxDelta = std::max(maxDelta, std::fabs(atZero.x()[i] - later->x()[i]));
    CHECK(maxDelta > 0.05f);
    CHECK(!throwsRuntimeError([&] { loadNetworkFromFile(net, std::string(JITNODES_SOURCE_DIR) + "/examples/graphs/wobble.json"); }));
    CHECK(throwsRuntimeError([&] { loadNetworkFromFile(net, "does/not/exist.json"); }));
}

namespace {

bool nearBoth(float a, float b, float absTol, float relTol) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    if (std::isinf(a) || std::isinf(b)) return a == b;
    const float diff = std::fabs(a - b);
    return diff <= absTol || diff <= relTol * std::max(std::fabs(a), std::fabs(b));
}

Geometry wideRamp(size_t n, float xScale) {
    Geometry g;
    g.resizePoints(n);
    for (size_t i = 0; i < n; ++i) {
        g.x()[i] = (static_cast<float>(i) - static_cast<float>(n) / 2) * xScale;
        g.y()[i] = static_cast<float>(i % 50) * 0.1f;
    }
    return g;
}

void expectIsaMatchesInterpreter(Isa isa, const std::string& code, const std::vector<std::string>& names,
                                 const std::vector<float>& params, float absTol, float relTol, size_t points = 203) {
    KernelCompiler jit, interp;
    jit.setIsa(isa);
    interp.setJitEnabled(false);
    KernelSpec spec = parseWrangle(code, names);
    Geometry a = wideRamp(points, 0.37f), b = a;
    AttribArray& idA = a.attrib(AttribClass::point, "id", AttribType::int32);
    AttribArray& idB = b.attrib(AttribClass::point, "id", AttribType::int32);
    for (size_t i = 0; i < points; ++i) idA.ints()[i] = idB.ints()[i] = static_cast<int32_t>(i) - 20;
    auto kj = jit.compileFor(spec, a, AttribClass::point);
    auto ki = interp.compileFor(spec, b, AttribClass::point);
    CHECK(kj->isJitted() && kj->isa() == isa);
    kj->run(a, params);
    ki->run(b, params);
    for (const auto& [name, arrayA] : a.attribs(AttribClass::point)) {
        const AttribArray& arrayB = *b.findAttrib(AttribClass::point, name);
        for (size_t i = 0; i < points; ++i) {
            if (!nearBoth(arrayA.at(i), arrayB.at(i), absTol, relTol)) {
                throw std::runtime_error(std::string(isaName(isa)) + ": " + code + " -> " + name + "[" + std::to_string(i) + "] jit=" +
                                         std::to_string(arrayA.at(i)) + " interp=" + std::to_string(arrayB.at(i)));
            }
        }
    }
}

}  // namespace

TEST(everySupportedInstructionSetMatchesInterpreter) {
    KernelCompiler probe;
    const auto isas = probe.supportedIsas();
    CHECK(!isas.empty() && probe.isaSupported(Isa::sse2));
    for (Isa isa : isas) {
        expectIsaMatchesInterpreter(isa, "x = x * s + t; y = -y / 2 + abs(x) - sqrt(abs(y));", {"s", "t"}, {2.0f, 0.5f}, 1e-4f, 1e-5f);
        expectIsaMatchesInterpreter(isa,
            "y = (x > th && y < 3) ? y + off : (x <= 0 || y == 3 ? -y : max(y, min(x, 4)));"
            "x = !(x >= 2) ? x * 2 : clamp(x, 0, 6); w = lerp(x, y, 0.25) + (x != y ? 1 : 0);",
            {"th", "off"}, {5.0f, 100.0f}, 1e-4f, 1e-5f);
        expectIsaMatchesInterpreter(isa, "id = id * 3 + 1; x = x + id; w = id / 2; z = x >= y; q = x < y; r = x <= y; p = x > y; e = x == x;", {}, {}, 1e-4f, 1e-5f);
        expectIsaMatchesInterpreter(isa, "x = floor(x * 0.7) + ceil(y * 0.3); w = floor(-x) - ceil(-y * 1.5); z = floor(x * 100000) + ceil(x * -100000);", {}, {}, 1e-4f, 1e-5f);
        expectIsaMatchesInterpreter(isa, "x = x ? 1 : 2; y = (x - 1) ? y : -y;", {}, {}, 1e-4f, 1e-5f);
    }
}

TEST(vectorMathFunctionsAreAccurateOnEveryInstructionSet) {
    KernelCompiler probe;
    for (Isa isa : probe.supportedIsas()) {
        expectIsaMatchesInterpreter(isa, "w = sin(x); z = cos(x);", {}, {}, 5e-6f, 0.0f, 1601);
        expectIsaMatchesInterpreter(isa, "w = exp(x * 0.1); z = exp(-y);", {}, {}, 0.0f, 1e-5f, 1601);
        expectIsaMatchesInterpreter(isa, "w = log(abs(x) + 0.001); z = log(y + 1);", {}, {}, 1e-5f, 1e-5f, 1601);
        expectIsaMatchesInterpreter(isa, "w = pow(abs(x) + 1, y * 0.1 + 0.5); z = pow(2, x * 0.05);", {}, {}, 0.0f, 1e-4f, 1601);
    }
}

TEST(vectorMathEdgeCases) {
    KernelCompiler probe;
    for (Isa isa : probe.supportedIsas()) {
        KernelCompiler compiler;
        compiler.setIsa(isa);
        Geometry g;
        g.resizePoints(16);
        const float inputs[16] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 100000.0f, -100000.0f, 3.14159265f, 1e-10f, 88.0f, -88.0f, 120.0f, -120.0f, 1e30f, 2.0f, 10.0f};
        for (size_t i = 0; i < 16; ++i) g.x()[i] = inputs[i];
        compiler.compileFor(parseWrangle("y = log(x); z = exp(x); w = sin(x);", {}), g, AttribClass::point)->run(g, {});
        const float* y = g.findPointAttrib("y")->data();
        const float* z = g.findPointAttrib("z")->data();
        CHECK(std::isinf(y[0]) && y[0] < 0 && std::isinf(y[1]) && y[1] < 0);
        CHECK(std::isnan(y[3]) && std::isnan(y[6]));
        CHECK_NEAR(y[2], 0.0f);
        CHECK(nearBoth(y[5], std::log(100000.0f), 1e-4f, 1e-6f));
        CHECK(nearBoth(z[0], 1.0f, 1e-6f, 1e-6f) && nearBoth(z[9], std::exp(88.0f), 0.0f, 2e-5f));
        CHECK(z[11] > 1e37f && std::isfinite(z[11]) && z[12] < 1e-37f && z[12] >= 0.0f);
        CHECK(nearBoth(g.findPointAttrib("w")->data()[7], std::sin(3.14159265f), 2e-6f, 0.0f));
    }
}

TEST(networkResultsDoNotDependOnInstructionSet) {
    auto build = [](Network& net, Isa isa) {
        net.compiler().setIsa(isa);
        NodeId line = net.addNode("line");
        NodeId wr = net.addNode("wrangle");
        NodeId xf = net.addNode("transform");
        net.node(line).setFloat("count", 1001.0f);
        net.node(line).setFloat("length", 30.0f);
        net.node(wr).setString("code", "y = sin(x * 0.3) * a + cos(x) * 0.1; w = exp(-abs(x) * 0.05);");
        net.node(wr).setFloat("a", 2.0f);
        net.node(xf).setFloat("sx", 1.5f);
        net.connect(line, wr);
        net.connect(wr, xf);
        return xf;
    };
    KernelCompiler probe;
    Network reference(1);
    reference.compiler().setJitEnabled(false);
    const NodeId refOut = build(reference, Isa::sse2);
    auto expected = reference.cook(refOut);
    for (Isa isa : probe.supportedIsas()) {
        Network net(2);
        const NodeId out = build(net, isa);
        auto geo = net.cook(out);
        CHECK(geo->pointCount() == expected->pointCount());
        for (const auto& [name, array] : geo->attribs(AttribClass::point))
            for (size_t i = 0; i < geo->pointCount(); ++i)
                CHECK(nearBoth(array.at(i), expected->findAttrib(AttribClass::point, name)->at(i), 5e-6f, 2e-5f));
        CHECK(net.compiler().isa() == isa);
    }
}

TEST(paddingAndAlignmentCoverWidestVectors) {
    Geometry g;
    g.resizePoints(5);
    CHECK(g.paddedPointCount() == simdLanes && simdLanes == 16 && simdAlignment == 64);
    CHECK(reinterpret_cast<uintptr_t>(g.x()) % simdAlignment == 0);
    KernelCompiler compiler;
    for (Isa isa : compiler.supportedIsas()) CHECK(simdLanes % isaLanes(isa) == 0);
}

namespace {

SubnetNode& subnetOf(Network& net, NodeId id) { return dynamic_cast<SubnetNode&>(net.node(id)); }

// line(3, length 2) -> subnet -> null; inside: subnetInput -> transform. Returns the ids.
struct SubnetRig {
    Network net;
    NodeId line, sub, out, in, xf;
    SubnetRig() {
        line = net.addNode("line", "src");
        sub = net.addNode("subnet", "sub");
        out = net.addNode("null", "out");
        net.node(line).setFloat("count", 3.0f);
        net.node(line).setFloat("length", 2.0f);
        Network& inner = subnetOf(net, sub).network();
        in = inner.addNode("subnetInput", "in");
        xf = inner.addNode("transform", "xf");
        inner.connect(in, xf);
        inner.setDisplayNode(xf);
        net.connect(line, sub);
        net.connect(sub, out);
        net.setDisplayNode(out);
    }
    Network& inner() { return subnetOf(net, sub).network(); }
};

}  // namespace

TEST(subnetCooksItsChildNetworkAndCachesIt) {
    SubnetRig rig;
    rig.inner().node(rig.xf).setFloat("tx", 10.0f);
    auto geo = rig.net.cookDisplay();
    CHECK(geo->pointCount() == 3);
    CHECK_NEAR(geo->x()[2], 12.0f);
    rig.net.cookDisplay();
    CHECK(rig.net.node(rig.sub).cookCount() == 1 && rig.inner().node(rig.xf).cookCount() == 1);
    CHECK(&rig.inner().compiler() == &rig.net.compiler());
    CHECK(rig.net.compiler().compileCount() == 1);

    rig.net.node(rig.line).setFloat("count", 5.0f);
    CHECK(rig.net.cookDisplay()->pointCount() == 5);
    CHECK(rig.inner().node(rig.in).cookCount() == 2 && rig.inner().node(rig.xf).cookCount() == 2);
    rig.inner().node(rig.xf).setFloat("tx", 1.0f);
    CHECK_NEAR(rig.net.cookDisplay()->x()[0], 1.0f);
    CHECK(rig.inner().node(rig.in).cookCount() == 2);
}

TEST(promotedParametersAreVisibleInsideAsParentReferences) {
    SubnetRig rig;
    subnetOf(rig.net, rig.sub).addSpareFloat("amount", 2.0f);
    rig.inner().node(rig.xf).setExpression("tx", "parent.amount * 3");
    CHECK_NEAR(rig.net.cookDisplay()->x()[0], 6.0f);
    rig.net.node(rig.sub).setFloat("amount", 5.0f);
    CHECK_NEAR(rig.net.cookDisplay()->x()[0], 15.0f);
    CHECK(rig.inner().node(rig.in).cookCount() == 1);
    CHECK(rig.net.node(rig.sub).isSpare("amount") && rig.net.node(rig.sub).spareNames().size() == 1);

    rig.net.node(rig.sub).setKeyframes("amount", {{0.0, 0.0f, KeyInterp::linear}, {2.0, 4.0f, KeyInterp::linear}});
    CHECK(rig.net.dependsOnTime(rig.sub) && !rig.net.dependsOnTime(rig.line));
    rig.net.setTime(1.0);
    CHECK_NEAR(rig.net.cookDisplay()->x()[0], 6.0f);
    CHECK(rig.net.node(rig.line).cookCount() == 1);
}

TEST(subnetsFollowTimeAndNest) {
    SubnetRig rig;
    rig.inner().node(rig.xf).setExpression("tx", "time * 2");
    rig.net.cookDisplay();
    CHECK(rig.net.dependsOnTime(rig.sub) && !rig.net.dependsOnTime(rig.line));
    rig.net.setTime(1.5);
    CHECK_NEAR(rig.net.cookDisplay()->x()[0], 3.0f);
    CHECK(rig.net.node(rig.line).cookCount() == 1);

    Network net;
    NodeId line = net.addNode("line", "src");
    NodeId outer = net.addNode("subnet", "outer");
    net.connect(line, outer);
    net.setDisplayNode(outer);
    SubnetNode& outerNode = subnetOf(net, outer);
    outerNode.addSpareFloat("a", 0.0f);
    outerNode.network();
    NodeId in1 = outerNode.network().addNode("subnetInput", "in");
    NodeId middle = outerNode.network().addNode("subnet", "middle");
    outerNode.network().connect(in1, middle);
    outerNode.network().setDisplayNode(middle);
    SubnetNode& middleNode = subnetOf(outerNode.network(), middle);
    middleNode.addSpareFloat("b", 0.0f);
    outerNode.network().node(middle).setExpression("b", "parent.a * 2");
    NodeId in2 = middleNode.network().addNode("subnetInput", "in");
    NodeId xf = middleNode.network().addNode("transform", "xf");
    middleNode.network().connect(in2, xf);
    middleNode.network().setDisplayNode(xf);
    middleNode.network().node(xf).setExpression("tx", "parent.b + 1");

    net.node(outer).setFloat("a", 3.0f);
    CHECK_NEAR(net.cook(outer)->x()[0], 7.0f);
    net.node(outer).setFloat("a", 10.0f);
    CHECK_NEAR(net.cook(outer)->x()[0], 21.0f);
}

TEST(subnetsSupportSeveralInputsAndReportErrors) {
    Network net;
    NodeId a = net.addNode("line", "a");
    NodeId b = net.addNode("line", "b");
    NodeId sub = net.addNode("subnet2", "sub");
    net.node(a).setFloat("count", 3.0f);
    net.node(b).setFloat("count", 5.0f);
    net.connect(a, sub, 0);
    net.connect(b, sub, 1);
    Network& inner = subnetOf(net, sub).network();
    NodeId in0 = inner.addNode("subnetInput", "in0");
    NodeId in1 = inner.addNode("subnetInput", "in1");
    inner.node(in1).setFloat("index", 1.0f);
    NodeId merge = inner.addNode("merge", "merge");
    inner.connect(in0, merge, 0);
    inner.connect(in1, merge, 1);
    std::string message;
    CHECK(throwsRuntimeError([&] { net.cook(sub); }, &message));
    CHECK(message.find("no output") != std::string::npos);
    inner.setDisplayNode(merge);
    CHECK(net.cook(sub)->pointCount() == 8);

    NodeId loose = net.addNode("transform", "loose");
    net.connect(a, loose);
    net.node(loose).setExpression("tx", "parent.x");
    CHECK(throwsRuntimeError([&] { net.cook(loose); }, &message));
    CHECK(message.find("only valid inside a subnet") != std::string::npos);
}

TEST(subnetsRoundTripThroughFiles) {
    SubnetRig rig;
    subnetOf(rig.net, rig.sub).addSpareFloat("amount", 2.0f);
    rig.net.node(rig.sub).setKeyframes("amount", {{0.0, 1.0f, KeyInterp::linear}, {4.0, 5.0f, KeyInterp::smooth}});
    rig.inner().node(rig.xf).setExpression("tx", "parent.amount + time");
    rig.inner().node(rig.xf).setFloat("sy", 3.0f);
    const std::string text = saveNetwork(rig.net);
    CHECK(text.find("\"graph\"") != std::string::npos && text.find("\"spare\"") != std::string::npos);

    Network copy(1);
    loadNetwork(copy, text);
    CHECK(saveNetwork(copy) == text);
    CHECK(subnetOf(copy, copy.findNode("sub")).network().nodeIds().size() == 2);
    for (double t : {0.0, 1.0, 3.5}) {
        rig.net.setTime(t);
        copy.setTime(t);
        expectSameGeometry(*rig.net.cookDisplay(), *copy.cookDisplay());
    }
    CHECK(!throwsRuntimeError([&] { loadNetwork(copy, text); }));
    CHECK(throwsRuntimeError([&] {
        loadNetwork(copy, R"({ format: "jitnodes", version: 1, nodes: [ { id: 1, type: "null", name: "x", graph: { nodes: [] }, inputs: [0] } ] })");
    }));
}

TEST(assetsRegisterAsNodeTypesAndInstancesAreIndependent) {
    SubnetRig rig;
    subnetOf(rig.net, rig.sub).addSpareFloat("amount", 0.5f);
    rig.inner().node(rig.xf).setExpression("tx", "parent.amount * 10");
    const std::string definition = saveAsset(rig.net, rig.sub, "scaleAssetT");
    registerAsset(definition);
    CHECK(NodeRegistry::instance().assetText("scaleAssetT") != nullptr);

    Network net;
    NodeId line = net.addNode("line");
    NodeId one = net.addNode("scaleAssetT", "one");
    NodeId two = net.addNode("scaleAssetT", "two");
    net.node(line).setFloat("count", 3.0f);
    net.connect(line, one);
    net.connect(line, two);
    net.node(two).setFloat("amount", 1.0f);
    CHECK_NEAR(net.cook(one)->x()[0], 5.0f);
    CHECK_NEAR(net.cook(two)->x()[0], 10.0f);
    CHECK(net.node(one).isAssetInstance() && net.node(one).inputCount() == 1);

    const std::string graphText = saveNetwork(net);
    CHECK(graphText.find("\"assets\"") != std::string::npos);
    size_t graphKeys = 0;
    for (size_t pos = 0; (pos = graphText.find("\"graph\"", pos)) != std::string::npos; ++pos) ++graphKeys;
    CHECK(graphKeys == 1);  // only the embedded definition, not the instances

    Network copy(1);
    loadNetwork(copy, graphText);
    CHECK(saveNetwork(copy) == graphText);
    CHECK_NEAR(copy.cook(copy.findNode("two"))->x()[0], 10.0f);

    registerAsset(definition.substr(0, definition.find("\"amount\": 0.5")) + "\"amount\": 0.1" + definition.substr(definition.find("\"amount\": 0.5") + 13));
    NodeId fresh = net.addNode("scaleAssetT", "fresh");
    net.connect(line, fresh);
    CHECK_NEAR(net.cook(fresh)->x()[0], 1.0f);
    CHECK_NEAR(net.cook(one)->x()[0], 5.0f);
}

TEST(embeddedAssetsMakeGraphFilesSelfContained) {
    SubnetRig rig;
    subnetOf(rig.net, rig.sub).addSpareFloat("amount", 2.0f);
    rig.inner().node(rig.xf).setExpression("tx", "parent.amount + 100");
    registerAsset(saveAsset(rig.net, rig.sub, "embedSourceAsset"));

    Network net;
    NodeId line = net.addNode("line");
    NodeId inst = net.addNode("embedSourceAsset", "inst");
    net.connect(line, inst);
    net.setDisplayNode(inst);
    std::string text = saveNetwork(net);
    for (size_t pos = 0; (pos = text.find("embedSourceAsset", pos)) != std::string::npos; pos += 16) text.replace(pos, 16, "embedLoadedAsset");

    CHECK(NodeRegistry::instance().assetText("embedLoadedAsset") == nullptr);
    Network loaded(1);
    loadNetwork(loaded, text);
    CHECK(NodeRegistry::instance().assetText("embedLoadedAsset") != nullptr);
    CHECK_NEAR(loaded.cookDisplay()->x()[0], 102.0f);

    std::string message;
    CHECK(throwsRuntimeError([&] { registerAsset("{ format: 'jitnodes-asset', version: 1, type: 'line', inputs: 1, promote: {}, graph: { nodes: [] } }"); }, &message));
    CHECK(message.find("conflicts") != std::string::npos);
    CHECK(throwsRuntimeError([&] { registerAsset("{ format: 'wrong', version: 1 }"); }));
    CHECK(throwsRuntimeError([&] { registerAsset("{ format: 'jitnodes-asset', version: 1, type: 'x', inputs: 99, promote: {}, graph: { nodes: [] } }"); }));
    CHECK(throwsRuntimeError([&] { saveAsset(rig.net, rig.line, "notASubnet"); }));
}

TEST(assetFilesRoundTripAndExamplesWork) {
    const std::string dir = std::string(JITNODES_SOURCE_DIR) + "/examples/";
    registerAssetFromFile(dir + "assets/wobble.asset.json");
    Network net(1);
    loadNetworkFromFile(net, dir + "graphs/assetDemo.json");
    CHECK(net.nodeIds().size() == 5 && net.node(net.findNode("wobbleA")).isAssetInstance());
    net.setTime(0.0);
    Geometry atZero = *net.cookDisplay();
    net.setTime(1.0);
    auto later = net.cookDisplay();
    float maxDelta = 0.0f;
    for (size_t i = 0; i < atZero.pointCount(); ++i) maxDelta = std::max(maxDelta, std::fabs(atZero.x()[i] - later->x()[i]));
    CHECK(atZero.pointCount() == 96 && maxDelta > 0.05f);

    const std::string path = (std::filesystem::temp_directory_path() / "jitnodes_test_asset.json").string();
    saveAssetToFile(net, net.findNode("wobbleA"), "wobbleAssetCopy", path);
    registerAssetFromFile(path);
    std::filesystem::remove(path);
    Network other(1);
    NodeId line = other.addNode("line");
    NodeId inst = other.addNode("wobbleAssetCopy");
    other.connect(line, inst);
    CHECK(other.cook(inst)->pointCount() == 100);
}

TEST(parallelSubnetBranchesMatchSingleThreadAndProfileNests) {
    auto build = [](Network& net) {
        std::vector<NodeId> tails;
        for (int b = 0; b < 6; ++b) {
            NodeId line = net.addNode("line");
            NodeId sub = net.addNode("subnet", "sub" + std::to_string(b));
            net.node(line).setFloat("count", 70000.0f + static_cast<float>(b));
            net.node(line).setFloat("length", 10.0f);
            SubnetNode& sn = dynamic_cast<SubnetNode&>(net.node(sub));
            sn.addSpareFloat("k", static_cast<float>(b));
            Network& inner = sn.network();
            NodeId in = inner.addNode("subnetInput", "in");
            NodeId xf = inner.addNode("transform", "innerXf");
            NodeId wr = inner.addNode("wrangle", "innerWr");
            inner.node(xf).setExpression("tx", "parent.k * 2 + time");
            inner.node(wr).setString("code", "y = y + x * a + sin(x);");
            inner.node(wr).setFloat("a", 0.5f);
            inner.connect(in, xf);
            inner.connect(xf, wr);
            inner.setDisplayNode(wr);
            net.connect(line, sub);
            tails.push_back(sub);
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
        net.setDisplayNode(tails[0]);
    };
    Network single(1), threaded(4);
    build(single);
    build(threaded);
    expectSameGeometry(*single.cookDisplay(), *threaded.cookDisplay());
    for (double t : {0.5, 2.0}) {
        single.setTime(t);
        threaded.setTime(t);
        for (Network* net : {&single, &threaded}) net->node(net->findNode("sub3")).setFloat("k", static_cast<float>(t) * 3.0f);
        expectSameGeometry(*single.cookDisplay(), *threaded.cookDisplay());
    }
    CHECK(threaded.compiler().compileCount() == single.compiler().compileCount());

    const std::string report = threaded.profileReport();
    CHECK(report.find("  innerXf") != std::string::npos && report.find("fused into innerWr") != std::string::npos);
    threaded.resetProfile();
    CHECK(dynamic_cast<SubnetNode&>(threaded.node(threaded.findNode("sub0"))).network().node(1).stats().cooks == 0);
}

TEST(copyOnWriteSharesUntouchedAttributes) {
    Network net(1);
    NodeId line = net.addNode("line");
    NodeId wr = net.addNode("wrangle");
    net.node(line).setFloat("count", 1000.0f);
    net.node(wr).setString("code", "y = y + 1;");
    net.connect(line, wr);
    net.setDisplayNode(wr);
    GeometryHandle in = net.cook(line);
    GeometryHandle out = net.cook(wr);
    CHECK(out->findAttrib(AttribClass::point, "x")->sharesStorageWith(*in->findAttrib(AttribClass::point, "x")));
    CHECK(!out->findAttrib(AttribClass::point, "y")->sharesStorageWith(*in->findAttrib(AttribClass::point, "y")));
    CHECK_NEAR(in->y()[10], 0.0f);
    CHECK_NEAR(out->y()[10], 1.0f);

    Geometry a = *in;
    Geometry b = a;
    b.x()[3] = 99.0f;
    CHECK_NEAR(a.x()[3], in->x()[3]);
    CHECK_NEAR(b.x()[3], 99.0f);
    b.resizePoints(2000);
    CHECK_NEAR(a.x()[999], in->x()[999]);
    CHECK_NEAR(b.x()[999], in->x()[999]);
    CHECK(a.pointCount() == 1000 && b.pointCount() == 2000);
}

TEST(concatMatchesSequentialAppend) {
    auto make = [](size_t n, float base, const char* extra, bool prim) {
        Geometry g;
        g.resizePoints(n);
        for (size_t i = 0; i < n; ++i) {
            g.x()[i] = base + static_cast<float>(i);
            g.y()[i] = -base;
        }
        if (n) g.addPrimitive({0, static_cast<uint32_t>(n), false});
        if (extra) {
            AttribArray& a = g.attrib(AttribClass::point, extra);
            for (size_t i = 0; i < n; ++i) a.floats()[i] = base * 2;
        }
        if (prim) g.attrib(AttribClass::primitive, "tag", AttribType::int32).ints()[0] = 7;
        g.setDetail("d", base);
        return g;
    };
    Geometry g1 = make(5, 1, nullptr, false), g2 = make(0, 2, "q", false), g3 = make(37, 3, "q", true), g4 = make(20, 4, nullptr, true);
    Geometry sequential;
    for (const Geometry* g : {&g1, &g2, &g3, &g4}) sequential.append(*g);
    ThreadPool pool(3);
    for (ThreadPool* p : {static_cast<ThreadPool*>(nullptr), &pool}) {
        Geometry merged = Geometry::concat({&g1, &g2, &g3, &g4}, p);
        CHECK(merged.pointCount() == sequential.pointCount());
        CHECK(merged.primitiveCount() == sequential.primitiveCount());
        for (size_t i = 0; i < merged.primitiveCount(); ++i)
            CHECK(merged.primitives()[i].start == sequential.primitives()[i].start && merged.primitives()[i].count == sequential.primitives()[i].count);
        for (AttribClass cls : {AttribClass::point, AttribClass::primitive}) {
            CHECK(merged.attribs(cls).size() == sequential.attribs(cls).size());
            for (const auto& [name, array] : sequential.attribs(cls)) {
                const AttribArray* m = merged.findAttrib(cls, name);
                CHECK(m && m->type() == array.type());
                for (size_t i = 0; m && i < array.size(); ++i) CHECK_NEAR(m->at(i), array.at(i));
            }
        }
        CHECK_NEAR(merged.detail("d"), 1.0f);
    }
}

TEST(renamingANodeRewritesExpressionsThatReferToIt) {
    Network net(2);
    NodeId a = net.addNode("transform", "a");
    NodeId b = net.addNode("transform", "b");
    NodeId ab = net.addNode("transform", "ab");
    net.node(b).setExpression("tx", "a.sx * 2 + ab.sx");
    net.node(ab).setFloat("sx", 5.0f);
    net.renameNode(a, "shift");
    CHECK(net.node(a).name() == "shift" && net.findNode("a") == invalidNodeId && net.findNode("shift") == a);
    CHECK(net.node(b).expression("tx") == "shift.sx * 2 + ab.sx");
    CHECK(throwsRuntimeError([&] { net.renameNode(a, "b"); }));
    CHECK(throwsRuntimeError([&] { net.renameNode(a, "2fast"); }));
    CHECK(throwsRuntimeError([&] { net.renameNode(a, "has space"); }));
    CHECK(throwsRuntimeError([&] { net.renameNode(a, ""); }));
    CHECK(net.uniqueName("fresh") == "fresh" && net.uniqueName("b") == "b1" && net.uniqueName("b7") == "b7" && net.uniqueName("b1") == "b1" && net.uniqueName("") == "node1");
    net.setDisplayNode(b);
    net.connect(net.addNode("line"), a, 0);
    net.connect(a, b, 0);
    CHECK(net.cookDisplay()->pointCount() > 0);
}

TEST(copyAndPasteDuplicateANodeWithItsContents) {
    Network net(2);
    NodeId line = net.addNode("line", "src");
    NodeId wr = net.addNode("wrangle", "wave");
    net.node(wr).setString("code", "y = x * c;");
    net.node(wr).setFloat("c", 3.0f);
    net.node(wr).setKeyframes("b", {{0.0, 1.0f, KeyInterp::linear}, {2.0, 4.0f, KeyInterp::linear}});
    net.connect(line, wr, 0);
    NodeId sub = net.addNode("subnet", "box");
    net.node(sub).addSpareFloat("size", 2.0f);
    NodeId inner = net.node(sub).childNetwork()->addNode("null", "inside");

    const NodeClip clip = copyNode(net, wr);
    NodeId copy = pasteNode(net, clip);
    CHECK(copy != wr && net.node(copy).name() == "wave1" && net.node(copy).getString("code") == "y = x * c;");
    CHECK(net.node(copy).getFloat("c") == 3.0f && net.node(copy).keyframes("b").size() == 2);
    CHECK(net.inputOf(copy, 0) == line);
    NodeId second = pasteNode(net, clip);
    CHECK(net.node(second).name() == "wave2");

    NodeId boxCopy = pasteNode(net, copyNode(net, sub));
    CHECK(net.node(boxCopy).name() == "box1" && net.node(boxCopy).isSpare("size") && net.node(boxCopy).childNetwork()->findNode("inside") != invalidNodeId);
    (void)inner;

    Network other(2);
    NodeId lone = pasteNode(other, clip);
    CHECK(other.node(lone).name() == "wave" && other.inputOf(lone, 0) == invalidNodeId);

    NodeClip bad = clip;
    bad.type = "noSuchType";
    const size_t before = net.nodeIds().size();
    CHECK(throwsRuntimeError([&] { pasteNode(net, bad); }) && net.nodeIds().size() == before);
}

TEST(pasteingSeveralNodesRemapsTheirNamesWiresAndExpressions) {
    Network net(2);
    NodeId a = net.addNode("transform", "a");
    NodeId a1 = net.addNode("transform", "a1");
    net.connect(a, a1, 0);
    net.node(a1).setExpression("tx", "a.sx + a1.sy");
    const std::vector<NodeClip> clips = {copyNode(net, a), copyNode(net, a1)};
    const std::vector<NodeId> pasted = pasteNodes(net, clips);
    CHECK(pasted.size() == 2 && net.node(pasted[0]).name() == "a2" && net.node(pasted[1]).name() == "a3");
    CHECK(net.inputOf(pasted[1], 0) == pasted[0] && net.inputOf(pasted[0], 0) == invalidNodeId);
    CHECK(net.node(pasted[1]).expression("tx") == "a2.sx + a3.sy");
    CHECK(net.node(a1).expression("tx") == "a.sx + a1.sy" && net.inputOf(a1, 0) == a);

    std::vector<NodeClip> broken = clips;
    broken[1].type = "noSuchType";
    CHECK(throwsRuntimeError([&] { pasteNodes(net, broken); }) && net.nodeIds().size() == 4);
}

TEST(clipsRoundTripThroughTextAndOtherTextIsIgnored) {
    Network net(2);
    NodeId a = net.addNode("transform", "a");
    NodeId a1 = net.addNode("transform", "a1");
    net.connect(a, a1, 0);
    net.node(a).setFloat("tx", 2.5f);
    net.node(a1).setExpression("tx", "a.tx * 2");
    const std::vector<NodeClip> clips = {copyNode(net, a), copyNode(net, a1)};
    const std::string text = clipsToText(clips);

    const std::vector<NodeClip> back = clipsFromText(text);
    CHECK(back.size() == 2 && back[0].name == "a" && back[1].type == "transform" && back[1].inputNames == clips[1].inputNames);
    Network other(2);
    const std::vector<NodeId> pasted = pasteNodes(other, back);
    CHECK(pasted.size() == 2 && other.node(pasted[0]).baseFloat("tx") == 2.5f && other.node(pasted[1]).expression("tx") == "a.tx * 2");
    CHECK(other.inputOf(pasted[1], 0) == pasted[0]);

    CHECK(clipsFromText("").empty() && clipsFromText("hello world").empty() && clipsFromText("[1, 2]").empty());
    CHECK(clipsFromText("{ format: \"jitnodes\", version: 1 }").empty());
    CHECK(throwsRuntimeError([&] { clipsFromText("{ format: \"jitnodes-nodes\", version: 1 }"); }));
    CHECK(throwsRuntimeError([&] { clipsFromText("{ format: \"jitnodes-nodes\", version: 9, nodes: [] }"); }));
    CHECK(throwsRuntimeError([&] { clipsFromText("{ format: \"jitnodes-nodes\", version: 1, nodes: [ { type: \"line\" } ] }"); }));
    CHECK(clipsFromText(clipsToText({})).empty());
}

TEST(fusionTruncatesIntegerAttributesBetweenNodesLikeUnfusedCooking) {
    // A value written into an int attribute is truncated at the store; a later node must see the truncated value whether or
    // not the two nodes were fused into one kernel.
    auto cook = [](bool fusion, bool jit, const std::string& second) {
        Network net(1);
        net.setFusionEnabled(fusion);
        net.compiler().setJitEnabled(jit);
        NodeId line = net.addNode("line");
        net.node(line).setFloat("count", 6.0f);
        net.node(line).setFloat("length", 5.0f);
        NodeId ac = net.addNode("attribCreate");
        net.node(ac).setString("name", "i");
        net.node(ac).setString("type", "int");
        NodeId w1 = net.addNode("wrangle");
        net.node(w1).setString("code", "i = x * 0.7 - 1.5;");
        NodeId w2 = net.addNode("wrangle");
        net.node(w2).setString("code", second);
        net.connect(line, ac);
        net.connect(ac, w1);
        net.connect(w1, w2);
        net.setDisplayNode(w2);
        GeometryHandle geo = net.cookDisplay();
        std::vector<float> out(geo->y(), geo->y() + geo->pointCount());
        return std::make_pair(out, net.fusedChainCount());
    };
    for (bool jit : {true, false}) {
        const auto fused = cook(true, jit, "y = i;");
        const auto unfused = cook(false, jit, "y = i;");
        CHECK(fused.second == 1 && unfused.second == 0);
        CHECK(fused.first == unfused.first);
        // x = 0..5 gives 0.7 * x - 1.5 = -1.5 -0.8 -0.1 0.6 1.3 2.0, truncated toward zero: -1 0 0 0 1 2
        CHECK(fused.first == std::vector<float>({-1, 0, 0, 0, 1, 2}));
        const auto sum = cook(true, jit, "y = i * 2 + i;");
        CHECK(sum.first == cook(false, jit, "y = i * 2 + i;").first);
    }
}

TEST(programsThatDifferOnlyInAttributeNamesDoNotShareAKernel) {
    // `x = x * 2;` and `y = y * 2;` have the same structure; a cached kernel binds channels by name, so they must not collide.
    for (bool fusion : {true, false}) {
        Network net(1);
        net.setFusionEnabled(fusion);
        NodeId line = net.addNode("line");
        net.node(line).setFloat("count", 4.0f);
        net.node(line).setFloat("length", 3.0f);
        net.node(line).setFloat("y", 10.0f);
        NodeId a = net.addNode("wrangle");
        net.node(a).setString("code", "x = x * 2;");
        NodeId b = net.addNode("wrangle");
        net.node(b).setString("code", "y = y * 2;");
        NodeId c = net.addNode("wrangle");
        net.node(c).setString("code", "u = x * 2;");
        net.connect(line, a);
        net.connect(line, b);
        net.connect(line, c);
        NodeId m = net.addNode("merge");
        net.connect(a, m, 0);
        net.connect(b, m, 1);
        net.connect(c, m, 2);
        net.setDisplayNode(m);
        GeometryHandle geo = net.cookDisplay();
        CHECK(geo->pointCount() == 12);
        const float* x = geo->x();
        const float* y = geo->y();
        const AttribArray* u = geo->findAttrib(AttribClass::point, "u");
        CHECK(u != nullptr);
        for (size_t i = 0; i < 4; ++i) {
            CHECK(x[i] == 2.0f * static_cast<float>(i) && y[i] == 10.0f);       // first branch doubled x
            CHECK(x[4 + i] == static_cast<float>(i) && y[4 + i] == 20.0f);       // second branch doubled y
            CHECK(x[8 + i] == static_cast<float>(i) && u && u->at(8 + i) == 2.0f * static_cast<float>(i));  // third branch wrote u
        }
        CHECK(net.compiler().compileCount() == 3);
    }
}

TEST(mergeTakesAnyNumberOfInputsAndKeepsOneSpare) {
    Network net(2);
    NodeId merge = net.addNode("merge");
    CHECK(net.node(merge).inputCount() == 2);
    std::vector<NodeId> lines;
    for (int i = 0; i < 5; ++i) {
        NodeId line = net.addNode("line");
        net.node(line).setFloat("count", 10.0f);
        net.node(line).setFloat("y", static_cast<float>(i));
        lines.push_back(line);
        net.connect(line, merge, i);
        CHECK(net.node(merge).inputCount() == i + 2);
        CHECK(net.inputOf(merge, i + 1) == invalidNodeId);
    }
    net.setDisplayNode(merge);
    GeometryHandle all = net.cookDisplay();
    CHECK(all->pointCount() == 50 && all->primitiveCount() == 5);

    net.disconnect(merge, 4);
    CHECK(net.node(merge).inputCount() == 5);
    net.disconnect(merge, 1);
    CHECK(net.node(merge).inputCount() == 5 && net.cookDisplay()->primitiveCount() == 3);

    net.removeNode(lines[3]);
    CHECK(net.node(merge).inputCount() == 4);
    net.setFusionEnabled(false);
    CHECK(net.cookDisplay()->primitiveCount() == 2);

    NodeId lonely = net.addNode("merge");
    net.setDisplayNode(lonely);
    CHECK(throwsRuntimeError([&] { net.cookDisplay(); }));
}

TEST(mergeWithManyInputsSurvivesSaveLoadAndUndo) {
    Network net(2);
    NodeId merge = net.addNode("merge");
    for (int i = 0; i < 6; ++i) {
        NodeId line = net.addNode("line");
        net.connect(line, merge, i);
    }
    GraphSnapshot before = snapshotNetwork(net);
    NodeId out = net.addNode("null");
    net.connect(merge, out, 0);
    net.disconnect(merge, 5);
    net.disconnect(merge, 2);
    CHECK(net.node(merge).inputCount() == 6);

    Network loaded(2);
    loadNetwork(loaded, saveNetwork(net));
    CHECK(loaded.node(merge).inputCount() == 6 && loaded.inputOf(merge, 2) == invalidNodeId && loaded.inputOf(merge, 4) != invalidNodeId);

    restoreSnapshot(net, before);
    CHECK(net.node(merge).inputCount() == 7 && net.inputOf(merge, 2) != invalidNodeId && net.inputOf(merge, 5) != invalidNodeId);
}

TEST(mergeTreesFlattenIntoOneTask) {
    auto build = [](Network& net, std::vector<NodeId>& merges) {
        std::vector<NodeId> tails;
        for (int i = 0; i < 4; ++i) {
            NodeId line = net.addNode("line");
            net.node(line).setFloat("count", 5000.0f + static_cast<float>(i));
            net.node(line).setFloat("y", static_cast<float>(i));
            tails.push_back(line);
        }
        while (tails.size() > 1) {
            std::vector<NodeId> next;
            for (size_t i = 0; i + 1 < tails.size(); i += 2) {
                NodeId merge = net.addNode("merge");
                net.connect(tails[i], merge, 0);
                net.connect(tails[i + 1], merge, 1);
                merges.push_back(merge);
                next.push_back(merge);
            }
            tails = next;
        }
        net.setDisplayNode(tails[0]);
    };
    Network flat(4), plain(4);
    plain.setFusionEnabled(false);
    std::vector<NodeId> flatMerges, plainMerges;
    build(flat, flatMerges);
    build(plain, plainMerges);
    GeometryHandle a = flat.cookDisplay();
    GeometryHandle b = plain.cookDisplay();
    expectSameGeometry(*a, *b);
    CHECK(a->primitiveCount() == 4);
    CHECK(flat.node(flatMerges[0]).cookCount() == 0 && flat.node(flatMerges[1]).cookCount() == 0);
    CHECK(flat.node(flatMerges[2]).cookCount() == 1);
    CHECK(plain.node(plainMerges[0]).cookCount() == 1);
    CHECK(flat.profileReport().find("fused into merge") != std::string::npos);

    flat.node(flat.findNode("line1")).setFloat("count", 123.0f);
    plain.node(plain.findNode("line1")).setFloat("count", 123.0f);
    expectSameGeometry(*flat.cookDisplay(), *plain.cookDisplay());
}

class CancelNode : public Node {
public:
    static inline bool armed = false;
    CancelNode() : Node("testCancel", 0) {}
    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>&) override {
        if (armed) ctx.network->cancel();
        return std::make_shared<Geometry>();
    }
};

TEST(cancelAbortsTheCookInProgress) {
    NodeRegistry::instance().registerType("testCancel", [] { return std::make_unique<CancelNode>(); });
    Network net(2);
    NodeId c = net.addNode("testCancel");
    NodeId n1 = net.addNode("null");
    NodeId n2 = net.addNode("null");
    net.connect(c, n1);
    net.connect(n1, n2);
    net.setDisplayNode(n2);

    net.cancel();
    CancelNode::armed = true;
    bool threw = false;
    try { net.cookDisplay(); } catch (const CookCancelled&) { threw = true; }
    CHECK(threw);
    CHECK(net.node(c).cookCount() == 1);
    CHECK(net.node(n2).cookCount() == 0);

    CancelNode::armed = false;
    CHECK(net.cookDisplay() != nullptr);
    CHECK(net.node(n2).cookCount() == 1);
}

TEST(undoOnlyTouchesTheNodesThatDiffer) {
    Network net(1);
    net.setFusionEnabled(false);
    NodeId line = net.addNode("line", "src");
    NodeId xf = net.addNode("transform", "xf");
    NodeId wr = net.addNode("wrangle", "wr");
    NodeId out = net.addNode("null", "out");
    net.connect(line, xf);
    net.connect(xf, wr);
    net.connect(wr, out);
    net.setDisplayNode(out);
    net.node(line).setFloat("count", 100.0f);
    net.node(wr).setString("code", "y = x * a;");
    net.node(wr).setFloat("a", 1.0f);
    History history(net);
    net.cookDisplay();

    net.node(wr).setFloat("a", 3.0f);
    CHECK(history.commit("edit a"));
    CHECK_NEAR(net.cookDisplay()->y()[10], 3.0f * net.cookDisplay()->x()[10]);

    CHECK(history.undo());
    const RestoreStats& stats = history.lastRestore();
    CHECK(stats.updated == 1 && stats.added == 0 && stats.removed == 0 && stats.rebuilt == 0 && stats.rewired == 0);
    auto geo = net.cookDisplay();
    CHECK_NEAR(geo->y()[10], geo->x()[10]);
    CHECK(net.node(line).cookCount() == 1 && net.node(xf).cookCount() == 1);
    CHECK(net.node(wr).cookCount() == 3 && net.node(out).cookCount() == 3);
}

TEST(undoRestoresStructureWiringAnimationAndSubnets) {
    SubnetRig rig;
    Network& net = rig.net;
    History history(net);
    std::vector<std::string> saved{saveNetwork(net, false)};
    auto step = [&](const char* label) {
        CHECK(history.commit(label));
        saved.push_back(saveNetwork(net, false));
    };

    net.node(rig.line).setExpression("length", "2 + time");
    step("expression");
    rig.inner().node(rig.xf).setFloat("tx", 7.0f);
    step("edit inside subnet");
    NodeId extra = net.addNode("transform", "extra");
    net.connect(rig.sub, extra);
    net.setDisplayNode(extra);
    step("add node");
    net.disconnect(rig.out, 0);
    net.connect(extra, rig.out);
    step("rewire");
    subnetOf(net, rig.sub).addSpareFloat("amount", 1.0f);
    step("spare parameter");
    net.removeNode(rig.out);
    step("delete");

    for (size_t i = saved.size() - 1; i > 0; --i) {
        CHECK(history.undo());
        CHECK(saveNetwork(net, false) == saved[i - 1]);
    }
    CHECK(!net.node(rig.line).isAnimated("length"));
    for (size_t i = 1; i < saved.size(); ++i) {
        CHECK(history.redo());
        CHECK(saveNetwork(net, false) == saved[i]);
    }
    CHECK(net.node(rig.line).isAnimated("length") && rig.inner().node(rig.xf).baseFloat("tx") == 7.0f);
    CHECK_NEAR(net.cook(extra)->x()[2], 2.0f + 7.0f);

    net.node(rig.line).setFloat("count", 50.0f);
    CHECK(history.undo());
    CHECK(saveNetwork(net, false) == saved[saved.size() - 2]);
    CHECK(history.redo());
    CHECK(saveNetwork(net, false) == saved.back());
}

TEST(historySharesUnchangedNodeText) {
    Network net(1);
    std::vector<NodeId> ids;
    for (int i = 0; i < 60; ++i) {
        ids.push_back(net.addNode(i == 0 ? "line" : "transform"));
        if (i) net.connect(ids[static_cast<size_t>(i) - 1], ids.back());
    }
    History history(net);
    const size_t fullSnapshot = saveNetwork(net, false).size();
    for (int i = 0; i < 30; ++i) {
        net.node(ids[static_cast<size_t>(i) + 1]).setFloat("tx", static_cast<float>(i) + 1.0f);
        CHECK(history.commit("edit"));
    }
    CHECK(history.size() == 31);
    CHECK(history.storedBytes() < 3 * fullSnapshot);
}

TEST(assetInstancesUpdateWhenTheDefinitionChanges) {
    auto define = [](int version, float scale, bool withExtra) {
        SubnetRig rig;
        SubnetNode& s = subnetOf(rig.net, rig.sub);
        s.addSpareFloat("amount", 1.0f);
        if (withExtra) s.addSpareFloat("extra", 0.5f);
        rig.inner().node(rig.xf).setExpression("tx", "parent.amount * " + std::to_string(scale) + (withExtra ? " + parent.extra" : ""));
        registerAsset(saveAsset(rig.net, rig.sub, "updAsset", version));
    };
    define(1, 10.0f, false);

    Network net;
    NodeId line = net.addNode("line", "src");
    NodeId a = net.addNode("updAsset", "a");
    NodeId b = net.addNode("updAsset", "b");
    NodeId holder = net.addNode("subnet", "holder");
    net.connect(line, a);
    net.connect(line, b);
    net.connect(line, holder);
    net.node(a).setFloat("amount", 2.0f);
    net.node(b).setExpression("amount", "time + 1");
    Network& inner = subnetOf(net, holder).network();
    NodeId in = inner.addNode("subnetInput", "in");
    NodeId c = inner.addNode("updAsset", "c");
    inner.connect(in, c);
    inner.setDisplayNode(c);
    CHECK_NEAR(net.cook(a)->x()[0], 20.0f);
    CHECK_NEAR(net.cook(holder)->x()[0], 10.0f);

    define(2, 100.0f, true);
    CHECK(NodeRegistry::instance().assetVersion("updAsset") == 2);
    CHECK(outdatedAssetInstances(net) == 3 && outdatedAssetInstances(net, "somethingElse") == 0);
    CHECK_NEAR(net.cook(a)->x()[0], 20.0f);

    CHECK(updateAssetInstances(net, "somethingElse") == 0);
    CHECK(updateAssetInstances(net) == 3);
    CHECK(updateAssetInstances(net) == 0 && outdatedAssetInstances(net) == 0);
    CHECK(net.node(a).isAssetInstance() && net.inputOf(a, 0) == line && net.node(a).hasFloat("extra"));
    CHECK_NEAR(net.cook(a)->x()[0], 200.5f);
    CHECK(net.node(b).isAnimated("amount"));
    net.setTime(2.0);
    CHECK_NEAR(net.cook(b)->x()[0], 300.5f);
    CHECK_NEAR(net.cook(holder)->x()[0], 100.5f);

    define(3, 100.0f, false);
    CHECK(updateAssetInstances(net) == 3);
    CHECK(!net.node(a).hasFloat("extra"));
    CHECK_NEAR(net.cook(a)->x()[0], 200.0f);

    const std::string text = saveNetwork(net);
    define(1, 10.0f, false);
    CHECK(NodeRegistry::instance().assetVersion("updAsset") == 1);
    Network loaded(1);
    loadNetwork(loaded, text);
    CHECK(NodeRegistry::instance().assetVersion("updAsset") == 3);
    CHECK_NEAR(loaded.cook(loaded.findNode("a"))->x()[0], 200.0f);
}

TEST(assetMigrationHooksUpgradeInstancesInPlaceAndOnLoad) {
    auto define = [](int version, bool renamed) {
        SubnetRig rig;
        SubnetNode& s = subnetOf(rig.net, rig.sub);
        const std::string param = renamed ? "gain" : "amount";
        s.addSpareFloat(param, 1.0f);
        rig.inner().node(rig.xf).setExpression("tx", "parent." + param);
        registerAsset(saveAsset(rig.net, rig.sub, "migAsset", version));
    };
    NodeRegistry& registry = NodeRegistry::instance();
    auto calls = std::make_shared<std::vector<std::pair<int, int>>>();
    registry.setAssetMigration("migAsset", [calls](AssetUpgrade& up) {
        calls->emplace_back(up.fromVersion, up.toVersion);
        if (up.old.hasFloat("amount")) up.fresh.setFloat("gain", up.old.baseFloat("amount") * 10.0f);
    });

    define(1, false);
    Network net;
    NodeId line = net.addNode("line", "src");
    NodeId a = net.addNode("migAsset", "a");
    net.connect(line, a);
    net.setDisplayNode(a);
    net.node(a).setFloat("amount", 2.0f);
    CHECK_NEAR(net.cookDisplay()->x()[0], 2.0f);
    const std::string oldText = saveNetwork(net);
    CHECK(oldText.find("\"assetVersion\"") != std::string::npos);

    define(2, true);
    CHECK(outdatedAssetInstances(net) == 1 && updateAssetInstances(net) == 1);
    CHECK(calls->size() == 1 && calls->front() == std::make_pair(1, 2));
    CHECK(net.node(a).hasFloat("gain") && !net.node(a).hasFloat("amount") && net.inputOf(a, 0) == line);
    CHECK_NEAR(net.cookDisplay()->x()[0], 20.0f);
    CHECK(updateAssetInstances(net) == 0 && calls->size() == 1);

    Network loaded(1);
    loadNetwork(loaded, oldText);
    CHECK(calls->size() > 1 && calls->back() == std::make_pair(1, 2));
    CHECK(loaded.node(loaded.findNode("a")).hasFloat("gain") && loaded.inputOf(loaded.findNode("a"), 0) == loaded.findNode("src"));
    CHECK_NEAR(loaded.cookDisplay()->x()[0], 20.0f);

    registry.setAssetMigration("migAsset", nullptr);
    Network plain(1);
    CHECK(!throwsRuntimeError([&] { loadNetwork(plain, oldText); }));
    CHECK_NEAR(plain.cookDisplay()->x()[0], 1.0f);

    registry.setAssetMigration("migAsset", [](AssetUpgrade&) { throw std::runtime_error("boom"); });
    define(3, true);
    std::string message;
    CHECK(throwsRuntimeError([&] { updateAssetInstances(net); }, &message));
    CHECK(message.find("asset migration of 'a'") != std::string::npos && message.find("boom") != std::string::npos);
    CHECK(outdatedAssetInstances(net) == 1 && net.inputOf(a, 0) == line);

    registry.setAssetMigration("migAsset", [](AssetUpgrade& up) { up.inputs.assign(up.inputs.size(), invalidNodeId); });
    CHECK(updateAssetInstances(net) == 1 && net.inputOf(a, 0) == invalidNodeId);
    registry.setAssetMigration("migAsset", nullptr);
}

TEST(nodePositionsRoundTripAndFollowUndo) {
    Network net(1);
    NodeId a = net.addNode("line", "a");
    NodeId b = net.addNode("null", "b");
    net.connect(a, b);
    CHECK(!net.node(a).hasPosition());
    net.node(a).setPosition(10.5f, -20.0f);
    History history(net);
    net.node(b).setPosition(30.0f, 40.0f);
    CHECK(history.commit("place b"));
    net.node(b).setPosition(35.0f, 45.0f);
    CHECK(history.commit("move b"));

    const std::string text = saveNetwork(net, false);
    Network copy(1);
    loadNetwork(copy, text);
    CHECK(copy.node(copy.findNode("a")).posX() == 10.5f && copy.node(copy.findNode("a")).posY() == -20.0f);
    CHECK(copy.node(copy.findNode("b")).hasPosition() && copy.node(copy.findNode("b")).posX() == 35.0f);
    CHECK(saveNetwork(copy, false) == text);

    CHECK(history.undo());
    CHECK(net.node(b).posX() == 30.0f && net.node(b).posY() == 40.0f);
    CHECK(net.node(b).cookCount() == 0 && net.node(a).cookCount() == 0);
    CHECK(throwsRuntimeError([&] { loadNetwork(copy, R"({ format: "jitnodes", version: 1, nodes: [ { id: 1, type: "null", name: "x", pos: [1], inputs: [0] } ] })"); }));
}

TEST(subnetsFollowTheParentFusionSetting) {
    auto build = [](Network& net, NodeId& first, NodeId& second) {
        NodeId line = net.addNode("line");
        NodeId sub = net.addNode("subnet", "sub");
        net.connect(line, sub);
        net.setDisplayNode(sub);
        Network& inner = dynamic_cast<SubnetNode&>(net.node(sub)).network();
        NodeId in = inner.addNode("subnetInput", "in");
        first = inner.addNode("transform", "x1");
        second = inner.addNode("transform", "x2");
        inner.connect(in, first);
        inner.connect(first, second);
        inner.setDisplayNode(second);
    };
    Network fused(1), plain(1);
    plain.setFusionEnabled(false);
    NodeId f1, f2, p1, p2;
    build(fused, f1, f2);
    build(plain, p1, p2);
    CHECK(fused.fusionEnabled() && !plain.fusionEnabled());
    expectSameGeometry(*fused.cookDisplay(), *plain.cookDisplay());
    Network& fusedInner = dynamic_cast<SubnetNode&>(fused.node(fused.findNode("sub"))).network();
    Network& plainInner = dynamic_cast<SubnetNode&>(plain.node(plain.findNode("sub"))).network();
    CHECK(fusedInner.fusionEnabled() && !plainInner.fusionEnabled());
    CHECK(fusedInner.node(f1).cookCount() == 0 && fusedInner.node(f2).cookCount() == 1);
    CHECK(plainInner.node(p1).cookCount() == 1 && plainInner.node(p2).cookCount() == 1);
}

TEST(unlockingAnAssetInstanceGivesAnEditableSubnetWithTheSameBehaviour) {
    SubnetRig rig;
    SubnetNode& s = subnetOf(rig.net, rig.sub);
    s.addSpareFloat("amount", 2.0f);
    rig.inner().node(rig.xf).setExpression("tx", "parent.amount * 10");
    registerAsset(saveAsset(rig.net, rig.sub, "unlockTestAsset"));

    Network net(1);
    NodeId line = net.addNode("line", "src");
    NodeId inst = net.addNode("unlockTestAsset", "inst");
    net.connect(line, inst);
    net.setDisplayNode(inst);
    net.node(inst).setFloat("amount", 4.0f);
    CHECK_NEAR(net.cookDisplay()->x()[0], 40.0f);
    net.node(inst).setPosition(5.0f, 6.0f);

    CHECK(throwsRuntimeError([&] { unlockAssetInstance(net, line); }));
    unlockAssetInstance(net, inst);
    CHECK(net.node(inst).typeName() == "subnet" && !net.node(inst).isAssetInstance() && net.node(inst).name() == "inst");
    CHECK(net.inputOf(inst, 0) == line && net.displayNode() == inst);
    CHECK(net.node(inst).hasPosition() && net.node(inst).posX() == 5.0f && net.node(inst).posY() == 6.0f);
    Network& inner = dynamic_cast<SubnetNode&>(net.node(inst)).network();
    CHECK(inner.nodeIds().size() == 2 && inner.findNode("xf") != invalidNodeId);
    CHECK(net.node(inst).hasFloat("amount") && net.node(inst).baseFloat("amount") == 4.0f);
    CHECK_NEAR(net.cookDisplay()->x()[0], 40.0f);

    inner.node(inner.findNode("xf")).setExpression("tx", "parent.amount * 100");
    net.node(inst).setFloat("amount", 5.0f);
    CHECK_NEAR(net.cookDisplay()->x()[0], 500.0f);
    CHECK(saveNetwork(net).find("\"graph\"") != std::string::npos);
    CHECK(NodeRegistry::instance().assetNames().size() >= 1);
    const std::vector<std::string> names = NodeRegistry::instance().assetNames();
    CHECK(std::find(names.begin(), names.end(), "unlockTestAsset") != names.end());
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

TEST(deeplyNestedProgramsAreRejectedInsteadOfOverflowingTheStack) {
    const std::string parens = "x = " + std::string(20000, '(') + "x" + std::string(20000, ')') + ";";
    bool threw = false;
    try { parseWrangle(parens, {}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { parseWrangle("x = " + std::string(20000, '-') + "x;", {}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    CHECK(parseWrangle("x = ((((x))));", {}).stores.size() == 1);
}

TEST(cycleDetectionDoesNotWalkEveryPath) {
    Network net;
    std::vector<NodeId> rungs = {net.addNode("null"), net.addNode("null")};
    for (int i = 2; i < 300; ++i) {
        NodeId m = net.addNode("merge");
        net.connect(rungs[static_cast<size_t>(i) - 1], m, 0);
        net.connect(rungs[static_cast<size_t>(i) - 2], m, 1);
        rungs.push_back(m);
    }
    bool threw = false;
    try { net.connect(rungs.back(), rungs.front(), 0); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    CHECK(net.indexesConsistent());
}

TEST(longChainsPlanAndCookWithoutRecursionLimits) {
    for (bool fusion : {true, false}) {
        Network net(1);
        net.setFusionEnabled(fusion);
        NodeId prev = net.addNode("line");
        net.node(prev).setFloat("count", 8.0f);
        for (int i = 0; i < 20000; ++i) {
            NodeId w = net.addNode("wrangle");
            net.node(w).setString("code", "x = x + 1;");
            net.connect(prev, w);
            prev = w;
        }
        net.setDisplayNode(prev);
        CHECK_NEAR(net.cookDisplay()->x()[0], 20000.0f);
    }
}

TEST(subnetsAndParameterReferencesHaveDepthLimits) {
    Network net;
    Network* level = &net;
    bool refused = false;
    try {
        for (size_t i = 0; i < Network::maxSubnetDepth + 5; ++i) {
            NodeId sub = level->addNode("subnet");
            level = &static_cast<SubnetNode&>(level->node(sub)).network();
        }
    } catch (const std::runtime_error&) { refused = true; }
    CHECK(refused);

    Network chain(1);
    NodeId prev = chain.addNode("line");
    std::vector<NodeId> nodes;
    for (int i = 0; i < 2000; ++i) {
        NodeId w = chain.addNode("wrangle");
        chain.node(w).setString("code", "x = x + a;");
        chain.connect(prev, w);
        nodes.push_back(w);
        prev = w;
    }
    for (size_t i = 1; i < nodes.size(); ++i) chain.node(nodes[i]).setExpression("a", chain.node(nodes[i - 1]).name() + ".a + 1");
    chain.setDisplayNode(prev);
    bool threw = false;
    try { chain.cookDisplay(); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

TEST(reverseIndexesFollowEditsRenamesAndRemovals) {
    Network net;
    NodeId a = net.addNode("line");
    NodeId b = net.addNode("wrangle");
    NodeId c = net.addNode("wrangle");
    NodeId m = net.addNode("merge");
    net.connect(a, b);
    net.connect(a, c);
    net.connect(b, m, 0);
    net.connect(c, m, 1);
    net.node(c).setExpression("a", net.node(b).name() + ".a + 1");
    CHECK(net.indexesConsistent());
    net.renameNode(b, "first");
    CHECK(net.indexesConsistent());
    net.node(c).setExpression("a", "");
    CHECK(net.indexesConsistent());
    net.connect(a, m, 0);
    CHECK(net.indexesConsistent());
    net.removeNode(c);
    CHECK(net.indexesConsistent());
    net.setDisplayNode(m);
    net.cookDisplay();
    net.node(a).setFloat("count", 5.0f);
    CHECK(net.node(m).dirty());
}

TEST(theKernelCacheIsCapped) {
    KernelCompiler compiler;
    for (size_t i = 0; i < KernelCompiler::maxCachedKernels + 100; ++i) compiler.compile(parseWrangle("x = x + " + std::to_string(i) + ".5;", {}));
    CHECK(compiler.cacheSize() <= KernelCompiler::maxCachedKernels);
    CHECK(compiler.cacheSize() > KernelCompiler::maxCachedKernels / 2);
}

TEST(pastingAThousandsOfNodesDoesNotScanTheGraphPerName) {
    Network net(1);
    std::vector<NodeClip> clips;
    NodeId prev = net.addNode("line");
    for (int i = 0; i < 3000; ++i) {
        NodeId w = net.addNode("wrangle");
        net.connect(prev, w);
        prev = w;
    }
    for (NodeId id : net.nodeIds()) clips.push_back(copyNode(net, id));
    const std::vector<NodeId> pasted = pasteNodes(net, clips);
    CHECK(pasted.size() == 3001);
    CHECK(net.nodeIds().size() == 6002);
    CHECK(net.indexesConsistent());
    net.removeNode(pasted[5]);
    const std::string reused = net.uniqueName(net.node(pasted[6]).name());
    CHECK(net.findNode(reused) == invalidNodeId);
    CHECK(net.indexesConsistent());
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
