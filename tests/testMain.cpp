#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
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
    CHECK(!kernel->isJitted());
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
