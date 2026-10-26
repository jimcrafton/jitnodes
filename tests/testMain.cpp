#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
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

namespace {
Geometry archCurve(bool closed) {
    // open: one cubic from (0,0) to (10,0) arching up to y = 7.5; closed: that arch and its mirror below, two segments
    Geometry g;
    const float openX[] = {0, 0, 10, 10}, openY[] = {0, 10, 10, 0};
    const float closedX[] = {0, 0, 10, 10, 10, 0}, closedY[] = {0, 10, 10, 0, -10, -10};
    const size_t n = closed ? 6 : 4;
    g.resizePoints(n);
    g.attrib(AttribClass::point, "w");
    for (size_t i = 0; i < n; ++i) {
        g.x()[i] = closed ? closedX[i] : openX[i];
        g.y()[i] = closed ? closedY[i] : openY[i];
    }
    g.attrib(AttribClass::point, "w").floats()[3] = 10.0f;
    g.addPrimitive({0, static_cast<uint32_t>(n), closed, PrimKind::cubic});
    return g;
}
}  // namespace

TEST(cubicPrimitiveLayoutAndFlatten) {
    Geometry open = archCurve(false);
    CHECK(hasCurves(open) && cubicSegments(open.primitives()[0]) == 1);
    Primitive bad{0, 5, false, PrimKind::cubic};
    CHECK(cubicSegments(bad) == 0);

    Geometry flat = flattenCurves(open, 0.01f);
    CHECK(!hasCurves(flat) && flat.primitiveCount() == 1 && flat.primitives()[0].kind == PrimKind::polyline && !flat.primitives()[0].closed);
    const size_t n = flat.pointCount();
    CHECK(n > 8);
    CHECK_NEAR(flat.x()[0], 0.0f);
    CHECK_NEAR(flat.y()[0], 0.0f);
    CHECK_NEAR(flat.x()[n - 1], 10.0f);
    CHECK_NEAR(flat.y()[n - 1], 0.0f);
    float top = 0.0f;
    for (size_t i = 0; i < n; ++i) top = std::max(top, flat.y()[i]);
    CHECK(top > 7.4f && top <= 7.5f + 1e-4f);
    const AttribArray* w = flat.findAttrib(AttribClass::point, "w");
    CHECK(w != nullptr);
    CHECK_NEAR(w->at(0), 0.0f);
    CHECK_NEAR(w->at(n - 1), 10.0f);
    for (size_t i = 1; i < n; ++i) CHECK(w->at(i) >= w->at(i - 1));

    Geometry closed = flattenCurves(archCurve(true), 0.05f);
    CHECK(closed.primitiveCount() == 1 && closed.primitives()[0].closed && closed.primitives()[0].kind == PrimKind::polyline);
    CHECK_NEAR(closed.x()[0], 0.0f);
    CHECK(std::fabs(closed.x()[closed.pointCount() - 1]) > 0.1f || std::fabs(closed.y()[closed.pointCount() - 1]) > 0.1f);

    Geometry resampled = resamplePolylines(open, 8, 0.0f);
    CHECK(resampled.pointCount() == 8 && !hasCurves(resampled));
    CHECK_NEAR(resampled.x()[0], 0.0f);
    CHECK_NEAR(resampled.x()[7], 10.0f);
}

namespace {
class GeoParamNode : public Node {
public:
    GeoParamNode() : Node("testGeoParam", 0) { declareGeometry("curve"); }
    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>&) override { return geometryParam("curve"); }
};

GeometryHandle authoredCurve(float shift) {
    auto g = std::make_shared<Geometry>(archCurve(false));
    g->x()[1] += shift;
    g->attrib(AttribClass::point, "id", AttribType::int32).ints()[2] = 7;
    g->addVectorAttrib(AttribClass::point, "n", 2);
    g->attrib(AttribClass::primitive, "layer", AttribType::int32).ints()[0] = 3;
    g->attrib(AttribClass::primitive, "weight").floats()[0] = 0.1f;
    g->setDetail("scale", 1.25f);
    return g;
}
}  // namespace

TEST(compactSaveLeavesOutDefaultsAndRoundTrips) {
    Network net;
    NodeId circle = net.addNode("circle");
    NodeId other = net.addNode("circle");
    net.node(other).setFloat("radius", 7.0f);
    net.node(other).setExpression("count", "8");
    const std::string full = saveNetwork(net, false, false);
    const std::string compact = saveNetwork(net, false, true);
    CHECK(compact.size() < full.size());
    Network loaded;
    loadNetwork(loaded, compact);
    CHECK(loaded.nodeIds().size() == 2);
    CHECK(loaded.node(circle).getFloat("radius") == net.node(circle).getFloat("radius"));
    CHECK(loaded.node(other).baseFloat("radius") == 7.0f);
    CHECK(loaded.node(other).expression("count") == "8");
    CHECK(saveNetwork(loaded, false, false) == full);
}

TEST(geometryParametersSaveSnapshotAndPaste) {
    static const bool registered = (NodeRegistry::instance().registerType("testGeoParam", [] { return std::make_unique<GeoParamNode>(); }), true);
    (void)registered;
    Network net;
    NodeId id = net.addNode("testGeoParam");
    CHECK(net.node(id).geometryParam("curve")->pointCount() == 0);
    net.node(id).setGeometryParam("curve", authoredCurve(0.0f));
    CHECK(sameGeometry(*net.cook(id), *authoredCurve(0.0f)));
    CHECK(net.cook(id)->primitives()[0].kind == PrimKind::cubic);

    const std::string text = saveNetwork(net);
    Network loaded;
    loadNetwork(loaded, text);
    CHECK(loaded.nodeIds().size() == 1);
    CHECK(sameGeometry(*loaded.node(loaded.nodeIds()[0]).geometryParam("curve"), *authoredCurve(0.0f)));

    GraphSnapshot before = snapshotNetwork(net);
    net.node(id).setGeometryParam("curve", authoredCurve(2.0f));
    CHECK(!sameGeometry(*net.cook(id), *authoredCurve(0.0f)));
    GraphSnapshot after = snapshotNetwork(net, &before);
    CHECK(!sameGraph(before, after));
    restoreSnapshot(net, before, &after);
    CHECK(sameGeometry(*net.cook(id), *authoredCurve(0.0f)));

    // an unchanged geometry does not dirty the node
    net.cook(id);
    net.node(id).setGeometryParam("curve", authoredCurve(0.0f));
    CHECK(!net.node(id).dirty());

    std::vector<NodeId> pasted = pasteNodes(net, clipsFromText(clipsToText({copyNode(net, id)})));
    CHECK(pasted.size() == 1 && sameGeometry(*net.cook(pasted[0]), *authoredCurve(0.0f)));
}

TEST(flattenNodeAndSvgExportOfCurves) {
    Network net;
    NodeId circle = net.addNode("circle");
    NodeId flat = net.addNode("flatten");
    net.connect(circle, flat);
    auto geo = net.cook(flat);
    CHECK(geo->primitiveCount() == 1 && !hasCurves(*geo));
    const std::string svg = geometryToSvg(archCurve(false));
    CHECK(svg.find("<path d=\"M") != std::string::npos && svg.find(" C") != std::string::npos);
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

// setTime remembers which nodes can depend on time; every edit that changes that has to be seen by the next setTime.
TEST(setTimeFollowsAnimationAddedAndRemovedAfterwards) {
    Network net;
    NodeId line = net.addNode("line");
    NodeId xf = net.addNode("transform");
    net.node(line).setFloat("count", 2.0f);
    net.node(line).setFloat("length", 1.0f);
    net.connect(line, xf);

    CHECK(!net.setTime(1.0));                       // nothing animated: nothing affected (and the list is built empty)
    net.cook(xf);
    net.node(xf).setKeyframes("ty", {{0.0, 0.0f, KeyInterp::linear}, {2.0, 10.0f, KeyInterp::linear}});
    CHECK(net.setTime(1.5));                        // keys added after the first setTime are seen
    CHECK_NEAR(net.cook(xf)->y()[0], 7.5f);

    net.node(xf).clearAnimation("ty");
    net.cook(xf);
    CHECK(!net.setTime(1.75));                      // and removed again
    net.node(xf).setExpression("tx", "time");
    CHECK(net.setTime(2.0));
    CHECK_NEAR(net.cook(xf)->x()[0], 2.0f);

    NodeId later = net.addNode("transform");        // a node added afterwards
    net.connect(line, later);
    net.node(later).setExpression("tx", "frame");
    net.setFps(10.0);
    CHECK(net.setTime(3.0));
    CHECK_NEAR(net.cook(later)->x()[0], 30.0f);

    net.removeNode(xf);
    net.removeNode(later);
    CHECK(!net.setTime(4.0));                       // removed nodes are forgotten
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

bool cookThrows(Network& net, NodeId id) {
    try {
        net.cook(id);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

TEST(generateIsDeterministicAndBounded) {
    Network net(1);
    NodeId gen = net.addNode("generate");
    net.node(gen).setFloat("count", 5000.0f);
    net.node(gen).setFloat("seed", 5.0f);
    net.node(gen).setFloat("cx", 10.0f);
    net.node(gen).setFloat("cy", -5.0f);
    net.node(gen).setFloat("width", 4.0f);
    net.node(gen).setFloat("height", 2.0f);
    auto geo = net.cook(gen);
    CHECK(geo->pointCount() == 5000 && geo->primitiveCount() == 0);
    const AttribArray* rand = geo->findAttrib(AttribClass::point, "rand");
    CHECK(rand && geo->hasAttrib(AttribClass::point, "rand2") && geo->findAttrib(AttribClass::point, "id")->type() == AttribType::int32);
    double meanX = 0.0, meanRand = 0.0;
    for (size_t i = 0; i < geo->pointCount(); ++i) {
        CHECK(geo->x()[i] >= 8.0f && geo->x()[i] <= 12.0f && geo->y()[i] >= -6.0f && geo->y()[i] <= -4.0f);
        CHECK(rand->at(i) >= 0.0f && rand->at(i) < 1.0f);
        meanX += geo->x()[i];
        meanRand += rand->at(i);
    }
    CHECK(std::fabs(meanX / 5000.0 - 10.0) < 0.1 && std::fabs(meanRand / 5000.0 - 0.5) < 0.03);

    // The thread count does not change the points.
    Network threaded(4);
    NodeId gen2 = threaded.addNode("generate");
    for (const char* p : {"count", "seed", "cx", "cy", "width", "height"}) threaded.node(gen2).setFloat(p, net.node(gen).getFloat(p));
    CHECK(sameGeometry(*geo, *threaded.cook(gen2)));
    net.node(gen).setFloat("seed", 6.0f);
    CHECK(!sameGeometry(*geo, *net.cook(gen)));

    net.node(gen).setString("shape", "disc");
    net.node(gen).setFloat("cx", 0.0f);
    net.node(gen).setFloat("cy", 0.0f);
    geo = net.cook(gen);
    for (size_t i = 0; i < geo->pointCount(); ++i) {
        const float u = geo->x()[i] / 2.0f, v = geo->y()[i];
        CHECK(u * u + v * v <= 1.0001f);
    }
    net.node(gen).setString("shape", "gaussian");
    geo = net.cook(gen);
    double var = 0.0;
    for (size_t i = 0; i < geo->pointCount(); ++i) var += static_cast<double>(geo->x()[i]) * geo->x()[i];
    CHECK(std::fabs(std::sqrt(var / 5000.0) - 2.0 / 3.0) < 0.05);

    net.node(gen).setString("shape", "blob");
    CHECK(cookThrows(net, gen));
    net.node(gen).setString("shape", "rect");
    net.node(gen).setFloat("count", 0.0f);
    CHECK(net.cook(gen)->pointCount() == 0);
}

TEST(scatterFillsClosedShapesAndWalksOpenOnes) {
    Network net(2);
    NodeId circle = net.addNode("circle");
    net.node(circle).setFloat("count", 96.0f);
    net.node(circle).setFloat("radius", 2.0f);
    NodeId scatter = net.addNode("scatter");
    net.node(scatter).setFloat("count", 4000.0f);
    net.node(scatter).setFloat("lineCount", 0.0f);
    net.connect(circle, scatter);
    auto geo = net.cook(scatter);
    CHECK(geo->pointCount() == 4000 && geo->primitiveCount() == 0);
    double meanR2 = 0.0;
    for (size_t i = 0; i < geo->pointCount(); ++i) {
        const double r2 = static_cast<double>(geo->x()[i]) * geo->x()[i] + static_cast<double>(geo->y()[i]) * geo->y()[i];
        CHECK(r2 <= 4.0);
        meanR2 += r2;
    }
    CHECK(std::fabs(meanR2 / 4000.0 - 2.0) < 0.15);  // uniform over a disc of radius 2

    NodeId line = net.addNode("line");
    net.node(line).setFloat("count", 5.0f);
    net.node(line).setFloat("length", 4.0f);
    NodeId merge = net.addNode("merge");
    NodeId both = net.addNode("scatter");
    net.node(both).setFloat("count", 1000.0f);
    net.node(both).setFloat("lineCount", 300.0f);
    net.connect(circle, merge, 0);
    net.connect(line, merge, 1);
    net.connect(merge, both);
    geo = net.cook(both);
    CHECK(geo->pointCount() == 1300);
    const auto& prim = geo->findAttrib(AttribClass::point, "srcPrim")->ints();
    const auto& u = geo->findAttrib(AttribClass::point, "u")->floats();
    size_t fromLine = 0;
    for (size_t i = 0; i < geo->pointCount(); ++i) {
        if (prim[i] == 1) {
            ++fromLine;
            CHECK(geo->y()[i] == 0.0f && geo->x()[i] >= 0.0f && geo->x()[i] <= 4.0f && u[i] >= 0.0f && u[i] <= 1.0f);
            CHECK_NEAR(geo->x()[i], u[i] * 4.0f);
        } else {
            CHECK(prim[i] == 0);
        }
    }
    CHECK(fromLine == 300);

    net.node(both).setFloat("spread", 0.5f);
    geo = net.cook(both);
    float maxOffset = 0.0f;
    for (size_t i = 0; i < geo->pointCount(); ++i)
        if (geo->findAttrib(AttribClass::point, "srcPrim")->ints()[i] == 1) maxOffset = std::max(maxOffset, std::fabs(geo->y()[i]));
    CHECK(maxOffset > 0.2f && maxOffset <= 0.5001f);

    // An empty or degenerate shape gives no points.
    NodeId single = net.addNode("line");
    net.node(single).setFloat("count", 1.0f);
    NodeId none = net.addNode("scatter");
    net.connect(single, none);
    CHECK(net.cook(none)->pointCount() == 0);
}

TEST(scatterKeepsConcaveShapesHollow) {
    Network net(1);
    // Eight circle points with the four diagonal ones pulled in make a four-pointed star whose notches (both |x| and |y| above 0.22) are outside.
    NodeId circle = net.addNode("circle");
    net.node(circle).setFloat("count", 8.0f);
    NodeId star = net.addNode("wrangle");
    net.node(star).setString("code", "s = abs(x) > 0.1 && abs(y) > 0.1 ? 0.3 : 1; x = x * s; y = y * s;");
    NodeId scatter = net.addNode("scatter");
    net.node(scatter).setFloat("count", 3000.0f);
    net.connect(circle, star);
    net.connect(star, scatter);
    auto geo = net.cook(scatter);
    CHECK(geo->pointCount() == 3000);
    size_t nearAxis = 0;
    for (size_t i = 0; i < geo->pointCount(); ++i) {
        const float ax = std::fabs(geo->x()[i]), ay = std::fabs(geo->y()[i]);
        CHECK(!(ax > 0.22f && ay > 0.22f));
        nearAxis += ax > 0.5f || ay > 0.5f ? 1 : 0;
    }
    CHECK(nearAxis > 100);
}

TEST(noiseNodeProducesBoundedDeterministicFields) {
    Network net(2);
    NodeId line = net.addNode("line");
    net.node(line).setFloat("count", 4000.0f);
    net.node(line).setFloat("length", 400.0f);
    NodeId noise = net.addNode("noise");
    net.connect(line, noise);
    net.node(noise).setString("name", "n");
    auto geo = net.cook(noise);
    const AttribArray* n = geo->findAttrib(AttribClass::point, "n");
    CHECK(n && geo->pointCount() == 4000);
    double mean = 0.0, sq = 0.0;
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = 0; i < 4000; ++i) {
        mean += n->at(i);
        sq += static_cast<double>(n->at(i)) * n->at(i);
        lo = std::min(lo, n->at(i));
        hi = std::max(hi, n->at(i));
    }
    mean /= 4000.0;
    CHECK(mean > 0.4 && mean < 0.6 && std::sqrt(sq / 4000.0 - mean * mean) > 0.03 && lo > -0.5f && hi < 1.5f);
    CHECK(sameGeometry(*geo, *net.cook(noise)));

    net.node(noise).setString("mode", "ridged");
    geo = net.cook(noise);
    for (size_t i = 0; i < 4000; ++i) CHECK(geo->findAttrib(AttribClass::point, "n")->at(i) >= 0.0f && geo->findAttrib(AttribClass::point, "n")->at(i) <= 1.0f);

    net.node(noise).setFloat("phase", 3.5f);
    CHECK(!sameGeometry(*geo, *net.cook(noise)));
    net.node(noise).setString("mode", "bogus");
    CHECK(cookThrows(net, noise));
}

TEST(warpDisplacesPointsAndZeroAmountIsIdentity) {
    Network net(2);
    NodeId gen = net.addNode("generate");
    net.node(gen).setFloat("count", 2000.0f);
    NodeId warp = net.addNode("warp");
    net.connect(gen, warp);
    net.node(warp).setFloat("amount", 0.0f);
    CHECK(sameGeometry(*net.cook(gen), *net.cook(warp)));

    for (const char* mode : {"noise", "curl"}) {
        net.node(warp).setString("mode", mode);
        net.node(warp).setFloat("amount", 2.0f);
        net.node(warp).setFloat("iterations", 3.0f);
        auto src = net.cook(gen);
        auto out = net.cook(warp);
        double moved = 0.0;
        for (size_t i = 0; i < out->pointCount(); ++i) {
            CHECK(std::isfinite(out->x()[i]) && std::isfinite(out->y()[i]));
            moved += std::hypot(out->x()[i] - src->x()[i], out->y()[i] - src->y()[i]);
        }
        CHECK(moved / 2000.0 > 0.05);
        CHECK(out->hasAttrib(AttribClass::point, "rand"));
    }
    net.node(warp).setString("mode", "swirl");
    CHECK(cookThrows(net, warp));
}

TEST(colorRampMapsAnAttributeToColour) {
    Network net(1);
    NodeId line = net.addNode("line");
    net.node(line).setFloat("count", 3.0f);
    net.node(line).setFloat("length", 1.0f);
    NodeId ramp = net.addNode("colorRamp");
    net.connect(line, ramp);
    net.node(ramp).setString("attribute", "x");
    net.node(ramp).setString("ramp", "0 #000000, 0.5 #ff0000, 1 0 0 1");
    auto geo = net.cook(ramp);
    const auto& r = geo->findAttrib(AttribClass::point, "Cd.x")->floats();
    const auto& g = geo->findAttrib(AttribClass::point, "Cd.y")->floats();
    const auto& b = geo->findAttrib(AttribClass::point, "Cd.z")->floats();
    CHECK(std::fabs(r[0]) < 1e-3f && std::fabs(r[1] - 1.0f) < 1e-3f && std::fabs(r[2]) < 1e-3f);
    CHECK(std::fabs(b[2] - 1.0f) < 1e-3f && std::fabs(g[1]) < 1e-3f);

    net.node(ramp).setFloat("min", 1.0f);
    net.node(ramp).setFloat("max", 0.0f);  // an inverted range reverses the ramp
    geo = net.cook(ramp);
    CHECK(std::fabs(geo->findAttrib(AttribClass::point, "Cd.z")->floats()[0] - 1.0f) < 1e-3f);

    net.node(ramp).setString("attribute", "missing");
    CHECK(cookThrows(net, ramp));
    net.node(ramp).setString("attribute", "x");
    net.node(ramp).setString("ramp", "0 #12");
    CHECK(cookThrows(net, ramp));
    net.node(ramp).setString("ramp", "");
    CHECK(cookThrows(net, ramp));
}

TEST(renderNodeAndSplatRendererDrawAdditiveSprites) {
    Network net(2);
    NodeId gen = net.addNode("generate");
    net.node(gen).setFloat("count", 1.0f);
    net.node(gen).setFloat("width", 0.0f);
    net.node(gen).setFloat("height", 0.0f);
    NodeId wr = net.addNode("wrangle");
    net.node(wr).setString("code", "radius = 2; alpha = 0.5; softness = 0; Cd.x = 1; Cd.y = 0; Cd.z = 0;");
    NodeId render = net.addNode("render");
    net.connect(gen, wr);
    net.connect(wr, render);
    net.node(render).setFloat("exposure", 20.0f);
    net.node(render).setFloat("gamma", 1.0f);
    net.node(render).setFloat("width", 100.0f);
    net.node(render).setFloat("height", 50.0f);
    net.node(render).setFloat("viewWidth", 10.0f);
    net.node(render).setString("background", "#000000");

    auto geo = net.cook(render);
    CHECK(hasRenderSettings(*geo) && !hasRenderSettings(*net.cook(wr)));
    RenderSettings settings = renderSettingsFromDetails(*geo);
    CHECK(settings.width == 100 && settings.height == 50 && settings.additive);
    CHECK_NEAR(settings.maxX - settings.minX, 10.0f);
    CHECK_NEAR(settings.maxY - settings.minY, 5.0f);

    Image image = renderSplats(*geo, settings, net.pool());
    CHECK(image.width == 100 && image.height == 50 && image.pixels.size() == 5000);
    const uint32_t centre = image.pixels[25 * 100 + 50], corner = image.pixels[0];
    CHECK(((centre >> 16) & 255u) > 240u && ((centre >> 8) & 255u) < 4u && (centre & 255u) < 4u);
    CHECK((corner & 0xFFFFFFu) < 0x030303u);
    // Radius 2 at 10 pixels per unit: covered out to 20 pixels, nothing beyond.
    CHECK(((image.pixels[25 * 100 + 66] >> 16) & 255u) > 200u && ((image.pixels[25 * 100 + 75] >> 16) & 255u) < 4u);

    // Over compositing is exact for opaque sprites and shows the background elsewhere.
    net.node(render).setFloat("additive", 0.0f);
    net.node(render).setString("background", "#0000ff");
    geo = net.cook(render);
    image = renderSplats(*geo, renderSettingsFromDetails(*geo), nullptr);
    CHECK((image.pixels[0] & 0xFFFFFFu) == 0x0000FFu);
    const uint32_t over = image.pixels[25 * 100 + 50];
    CHECK(std::abs(static_cast<int>((over >> 16) & 255u) - 128) <= 2 && std::abs(static_cast<int>(over & 255u) - 128) <= 2);

    net.node(render).setString("background", "red");
    CHECK(cookThrows(net, render));

    // A sprite smaller than a pixel keeps its energy: the lit pixels add up to about radius^2 * alpha in pixel units.
    net.node(render).setString("background", "#000000");
    net.node(render).setFloat("additive", 1.0f);
    net.node(wr).setString("code", "radius = 0.05; alpha = 1; softness = 0; Cd.x = 1; Cd.y = 1; Cd.z = 1;");
    net.node(render).setFloat("exposure", 1.0f);
    geo = net.cook(render);
    image = renderSplats(*geo, renderSettingsFromDetails(*geo), nullptr);
    size_t lit = 0;
    for (uint32_t px : image.pixels) lit += (px & 255u) > 8u ? 1 : 0;
    CHECK(lit >= 1 && lit <= 4);

    const std::string path = (std::filesystem::temp_directory_path() / "jitnodes_test_render.png").string();
    writePng(path, image);
    const size_t pngSize = static_cast<size_t>(std::filesystem::file_size(path));
    {
        std::ifstream f(path, std::ios::binary);
        char signature[4] = {};
        f.read(signature, 4);
        CHECK(signature[1] == 'P' && signature[2] == 'N' && signature[3] == 'G');
    }
    CHECK(pngSize > 60 && pngSize < image.pixels.size() * 3 / 4);
    std::filesystem::remove(path);
}

double minPairDistance(const Geometry& geo) {
    double best = 1e30;
    for (size_t i = 0; i < geo.pointCount(); ++i)
        for (size_t j = i + 1; j < geo.pointCount(); ++j) best = std::min(best, std::hypot(static_cast<double>(geo.x()[i] - geo.x()[j]), static_cast<double>(geo.y()[i] - geo.y()[j])));
    return best;
}

TEST(relaxSpreadsPointsApartAndKeepsThemInside) {
    Network net(1);
    NodeId gen = net.addNode("generate");
    net.node(gen).setFloat("count", 400.0f);
    net.node(gen).setFloat("width", 10.0f);
    net.node(gen).setFloat("height", 10.0f);
    NodeId relax = net.addNode("relax");
    net.connect(gen, relax);
    net.node(relax).setFloat("radius", 0.6f);
    net.node(relax).setFloat("iterations", 40.0f);
    auto before = net.cook(gen);
    auto after = net.cook(relax);
    CHECK(after->pointCount() == 400 && after->hasAttrib(AttribClass::point, "rand"));
    CHECK(minPairDistance(*after) > 2.0 * minPairDistance(*before));
    float minX = 1e9f, maxX = -1e9f;
    for (size_t i = 0; i < after->pointCount(); ++i) {
        minX = std::min(minX, after->x()[i]);
        maxX = std::max(maxX, after->x()[i]);
    }
    CHECK(minX >= -5.0f && maxX <= 5.0f);

    Network threaded(4);
    NodeId g2 = threaded.addNode("generate"), r2 = threaded.addNode("relax");
    threaded.connect(g2, r2);
    for (const char* p : {"count", "width", "height"}) threaded.node(g2).setFloat(p, net.node(gen).getFloat(p));
    for (const char* p : {"radius", "iterations"}) threaded.node(r2).setFloat(p, net.node(relax).getFloat(p));
    CHECK(sameGeometry(*after, *threaded.cook(r2)));

    // Coincident points are pushed apart too.
    net.node(gen).setFloat("count", 2.0f);
    net.node(gen).setFloat("width", 0.0f);
    net.node(gen).setFloat("height", 0.0f);
    net.node(relax).setFloat("keepInside", 0.0f);
    CHECK(minPairDistance(*net.cook(relax)) > 0.1);

    net.node(relax).setFloat("radius", 0.0f);
    CHECK(cookThrows(net, relax));
}

TEST(attribBlurAveragesNeighboursAndValidatesItsParameters) {
    Network net(2);
    NodeId gen = net.addNode("generate");
    net.node(gen).setFloat("count", 3000.0f);
    net.node(gen).setFloat("width", 10.0f);
    net.node(gen).setFloat("height", 10.0f);
    NodeId blur = net.addNode("attribBlur");
    net.connect(gen, blur);
    net.node(blur).setFloat("radius", 0.8f);
    net.node(blur).setFloat("iterations", 2.0f);
    auto src = net.cook(gen);
    auto out = net.cook(blur);
    auto variance = [](const Geometry& g, const char* name) {
        double sum = 0.0, sum2 = 0.0;
        const AttribArray* a = g.findAttrib(AttribClass::point, name);
        for (size_t i = 0; i < g.pointCount(); ++i) {
            sum += a->at(i);
            sum2 += static_cast<double>(a->at(i)) * a->at(i);
        }
        const double mean = sum / static_cast<double>(g.pointCount());
        return sum2 / static_cast<double>(g.pointCount()) - mean * mean;
    };
    CHECK(variance(*out, "rand") < 0.3 * variance(*src, "rand"));
    CHECK(sameGeometry(*net.cook(blur), *out));

    // output writes elsewhere and leaves the source alone; several attributes are blurred together.
    net.node(blur).setString("attribute", "rand, rand2");
    net.node(blur).setString("output", "a, b");
    out = net.cook(blur);
    CHECK(out->hasAttrib(AttribClass::point, "a") && out->hasAttrib(AttribClass::point, "b"));
    CHECK(variance(*out, "rand") > 0.99 * variance(*src, "rand") && variance(*out, "b") < 0.3 * variance(*src, "rand2"));

    net.node(blur).setString("output", "a");
    CHECK(cookThrows(net, blur));
    net.node(blur).setString("output", "");
    net.node(blur).setString("attribute", "missing");
    CHECK(cookThrows(net, blur));
    net.node(blur).setString("attribute", "id");  // int attributes cannot be overwritten in place
    CHECK(cookThrows(net, blur));
    net.node(blur).setString("attribute", "rand");
    net.node(blur).setString("falloff", "bogus");
    CHECK(cookThrows(net, blur));
    net.node(blur).setString("falloff", "gaussian");
    net.node(blur).setFloat("radius", 0.0f);
    CHECK(cookThrows(net, blur));
}

TEST(waveWritesASineAndCanDisplacePoints) {
    Network net(1);
    NodeId line = net.addNode("line");
    net.node(line).setFloat("count", 3.0f);
    net.node(line).setFloat("length", 2.0f);
    NodeId wave = net.addNode("wave");
    net.connect(line, wave);
    net.node(wave).setFloat("wavelength", 4.0f);
    net.node(wave).setFloat("amplitude", 2.0f);
    auto geo = net.cook(wave);  // radial from the origin: sin(2 pi r / 4) at r = 0, 1, 2
    const AttribArray* w = geo->findAttrib(AttribClass::point, "wave");
    CHECK(w && std::fabs(w->at(0)) < 1e-4f && std::fabs(w->at(1) - 2.0f) < 1e-4f && std::fabs(w->at(2)) < 1e-3f);

    net.node(wave).setString("mode", "plane");
    net.node(wave).setFloat("angle", 180.0f);  // running towards -x: the same points see the mirrored phase
    CHECK_NEAR(net.cook(wave)->findAttrib(AttribClass::point, "wave")->at(1), -2.0f);

    net.node(wave).setString("mode", "radial");
    net.node(wave).setFloat("phase", 1.5707963f);
    net.node(wave).setFloat("displaceY", 0.5f);
    geo = net.cook(wave);
    CHECK_NEAR(geo->findAttrib(AttribClass::point, "wave")->at(0), -2.0f);
    CHECK_NEAR(geo->y()[0], -1.0f);

    // Two sources at the same place are one source; two apart are averaged.
    net.node(wave).setFloat("phase", 0.0f);
    net.node(wave).setFloat("displaceY", 0.0f);
    net.node(wave).setFloat("sourceCount", 2.0f);
    CHECK_NEAR(net.cook(wave)->findAttrib(AttribClass::point, "wave")->at(1), 2.0f);
    net.node(wave).setFloat("cx2", 1.0f);  // sources at 0 and 1: the point at 1 sees sin(pi / 2) and sin(0)
    CHECK_NEAR(net.cook(wave)->findAttrib(AttribClass::point, "wave")->at(1), 1.0f);

    net.node(wave).setFloat("falloff", 1.0f);
    net.node(wave).setFloat("cx2", 0.0f);
    CHECK_NEAR(net.cook(wave)->findAttrib(AttribClass::point, "wave")->at(1), 2.0f * std::exp(-1.0f));
    net.node(wave).setFloat("wavelength", 0.0f);
    CHECK(cookThrows(net, wave));
    net.node(wave).setFloat("wavelength", 1.0f);
    net.node(wave).setString("mode", "spiral");
    CHECK(cookThrows(net, wave));
}

TEST(attractorIteratesAMapAndStaysBounded) {
    Network net(2);
    NodeId gen = net.addNode("generate");
    net.node(gen).setFloat("count", 3000.0f);
    net.node(gen).setFloat("width", 4.0f);
    net.node(gen).setFloat("height", 4.0f);
    NodeId att = net.addNode("attractor");
    net.connect(gen, att);

    // With a = b = 0 and c = d = 1 the Clifford map sends every point to (1, 1).
    net.node(att).setFloat("a", 0.0f);
    net.node(att).setFloat("b", 0.0f);
    net.node(att).setFloat("d", 1.0f);
    net.node(att).setFloat("iterations", 3.0f);
    net.node(att).setFloat("scale", 2.0f);
    net.node(att).setFloat("cx", 10.0f);
    auto geo = net.cook(att);
    CHECK_NEAR(geo->x()[7], 12.0f);
    CHECK_NEAR(geo->y()[7], 2.0f);
    CHECK_NEAR(geo->findAttrib(AttribClass::point, "speed")->at(7), 0.0f);

    for (const char* type : {"clifford", "dejong", "svensson"}) {
        net.node(att).setString("type", type);
        net.node(att).setFloat("a", 1.5f);
        net.node(att).setFloat("b", -1.8f);
        net.node(att).setFloat("c", 1.6f);
        net.node(att).setFloat("d", 0.9f);
        net.node(att).setFloat("iterations", 25.0f);
        net.node(att).setFloat("scale", 1.0f);
        net.node(att).setFloat("cx", 0.0f);
        geo = net.cook(att);
        double spread = 0.0;
        for (size_t i = 0; i < geo->pointCount(); ++i) {
            CHECK(std::fabs(geo->x()[i]) <= 3.0f && std::fabs(geo->y()[i]) <= 3.0f);
            const float d = geo->findAttrib(AttribClass::point, "dir")->at(i);
            CHECK(d >= 0.0f && d <= 1.0f);
            spread += std::fabs(geo->x()[i]);
        }
        CHECK(spread / 3000.0 > 0.2);
        CHECK(sameGeometry(*geo, *net.cook(att)));
    }

    Network threaded(5);
    NodeId g2 = threaded.addNode("generate"), a2 = threaded.addNode("attractor");
    threaded.connect(g2, a2);
    for (const char* p : {"count", "width", "height"}) threaded.node(g2).setFloat(p, net.node(gen).getFloat(p));
    threaded.node(a2).setString("type", "svensson");
    for (const char* p : {"a", "b", "c", "d", "iterations", "scale"}) threaded.node(a2).setFloat(p, net.node(att).getFloat(p));
    CHECK(sameGeometry(*geo, *threaded.cook(a2)));

    net.node(att).setString("type", "lorenz");
    CHECK(cookThrows(net, att));
}

TEST(voronoiCellsPartitionTheBoundsAndBelongToTheirSites) {
    Network net(2);
    NodeId gen = net.addNode("generate");
    net.node(gen).setFloat("count", 300.0f);
    net.node(gen).setFloat("width", 10.0f);
    net.node(gen).setFloat("height", 6.0f);
    NodeId vor = net.addNode("voronoi");
    net.connect(gen, vor);
    auto sites = net.cook(gen);
    auto geo = net.cook(vor);
    CHECK(geo->primitiveCount() == 300 && geo->hasAttrib(AttribClass::point, "rand"));

    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (size_t i = 0; i < sites->pointCount(); ++i) {
        minX = std::min(minX, sites->x()[i]);
        maxX = std::max(maxX, sites->x()[i]);
        minY = std::min(minY, sites->y()[i]);
        maxY = std::max(maxY, sites->y()[i]);
    }
    double areaSum = 0.0;
    for (const Primitive& p : geo->primitives()) {
        CHECK(p.closed && p.count >= 3);
        const int32_t site = geo->findAttrib(AttribClass::point, "site")->ints()[p.start];
        areaSum += geo->findAttrib(AttribClass::point, "area")->at(p.start);
        // The average of a convex cell's corners lies inside it, so its nearest site must be the cell's own.
        double cx = 0.0, cy = 0.0;
        for (uint32_t k = 0; k < p.count; ++k) {
            cx += geo->x()[p.start + k];
            cy += geo->y()[p.start + k];
        }
        cx /= p.count;
        cy /= p.count;
        size_t nearest = 0;
        double best = 1e30;
        for (size_t s = 0; s < sites->pointCount(); ++s) {
            const double d = std::hypot(cx - sites->x()[s], cy - sites->y()[s]);
            if (d < best) {
                best = d;
                nearest = s;
            }
        }
        CHECK(static_cast<int32_t>(nearest) == site);
        CHECK(std::fabs(geo->x()[p.start] - sites->x()[static_cast<size_t>(site)]) < 20.0f);
    }
    CHECK(std::fabs(areaSum - static_cast<double>(maxX - minX) * (maxY - minY)) < 1e-3 * areaSum);

    // A margin grows the clipping rectangle; open outlines repeat the first corner.
    net.node(vor).setFloat("margin", 1.0f);
    net.node(vor).setFloat("closed", 0.0f);
    geo = net.cook(vor);
    double grown = 0.0;
    for (const Primitive& p : geo->primitives()) {
        CHECK(!p.closed);
        CHECK(geo->x()[p.start] == geo->x()[p.start + p.count - 1] && geo->y()[p.start] == geo->y()[p.start + p.count - 1]);
        grown += geo->findAttrib(AttribClass::point, "area")->at(p.start);
    }
    CHECK(std::fabs(grown - static_cast<double>(maxX - minX + 2.0f) * (maxY - minY + 2.0f)) < 1e-3 * grown);

    // Degenerate inputs: nothing, one point, coincident points.
    net.node(gen).setFloat("count", 0.0f);
    CHECK(net.cook(vor)->pointCount() == 0);
    net.node(gen).setFloat("count", 1.0f);
    CHECK(net.cook(vor)->primitiveCount() == 1);
    net.node(gen).setFloat("count", 3.0f);
    net.node(gen).setFloat("width", 0.0f);
    net.node(gen).setFloat("height", 0.0f);
    CHECK(net.cook(vor)->primitiveCount() >= 1);
}

double cellAreaSpread(Network& net, NodeId points) {
    NodeId vor = net.addNode("voronoi");
    net.connect(points, vor);
    auto geo = net.cook(vor);
    double sum = 0.0, sumSq = 0.0;
    for (const Primitive& p : geo->primitives()) {
        const double a = geo->findAttrib(AttribClass::point, "area")->at(p.start);
        sum += a;
        sumSq += a * a;
    }
    const double n = static_cast<double>(geo->primitiveCount()), mean = sum / n;
    return std::sqrt(std::max(sumSq / n - mean * mean, 0.0)) / mean;
}

TEST(lloydEvensOutTheVoronoiCellsAndKeepsPointsInside) {
    Network net(1);
    NodeId gen = net.addNode("generate");
    net.node(gen).setFloat("count", 300.0f);
    net.node(gen).setFloat("width", 10.0f);
    net.node(gen).setFloat("height", 6.0f);
    NodeId lloyd = net.addNode("lloyd");
    net.connect(gen, lloyd);
    net.node(lloyd).setFloat("iterations", 8.0f);
    auto before = net.cook(gen);
    auto after = net.cook(lloyd);
    CHECK(after->pointCount() == 300 && after->hasAttrib(AttribClass::point, "rand"));
    CHECK(cellAreaSpread(net, lloyd) < 0.5 * cellAreaSpread(net, gen));
    CHECK(minPairDistance(*after) > minPairDistance(*before));

    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (size_t i = 0; i < before->pointCount(); ++i) {
        minX = std::min(minX, before->x()[i]);
        maxX = std::max(maxX, before->x()[i]);
        minY = std::min(minY, before->y()[i]);
        maxY = std::max(maxY, before->y()[i]);
    }
    for (size_t i = 0; i < after->pointCount(); ++i) CHECK(after->x()[i] >= minX && after->x()[i] <= maxX && after->y()[i] >= minY && after->y()[i] <= maxY);

    Network threaded(4);
    NodeId g2 = threaded.addNode("generate"), l2 = threaded.addNode("lloyd");
    threaded.connect(g2, l2);
    for (const char* p : {"count", "width", "height"}) threaded.node(g2).setFloat(p, net.node(gen).getFloat(p));
    threaded.node(l2).setFloat("iterations", 8.0f);
    CHECK(sameGeometry(*after, *threaded.cook(l2)));

    net.node(lloyd).setFloat("strength", 0.0f);
    CHECK(sameGeometry(*net.cook(lloyd), *before));

    net.node(gen).setFloat("count", 0.0f);
    CHECK(net.cook(lloyd)->pointCount() == 0);
    net.node(gen).setFloat("count", 3.0f);
    net.node(gen).setFloat("width", 0.0f);
    net.node(gen).setFloat("height", 0.0f);
    CHECK(net.cook(lloyd)->pointCount() == 3);
}

TEST(flowFollowsAFieldAndCarriesTheSeedAttributes) {
    Network net(2);
    NodeId gen = net.addNode("generate");
    net.node(gen).setFloat("count", 100.0f);
    NodeId flow = net.addNode("flow");
    net.connect(gen, flow);
    net.node(flow).setFloat("steps", 10.0f);
    net.node(flow).setFloat("stepLength", 0.2f);
    auto seeds = net.cook(gen);
    for (const char* mode : {"curl", "angle"}) {
        net.node(flow).setString("mode", mode);
        auto geo = net.cook(flow);
        CHECK(geo->pointCount() == 1100 && geo->primitiveCount() == 100);
        const AttribArray* rand = geo->findAttrib(AttribClass::point, "rand");
        const AttribArray* u = geo->findAttrib(AttribClass::point, "u");
        const AttribArray* trail = geo->findAttrib(AttribClass::point, "trail");
        CHECK(rand && u && trail && trail->type() == AttribType::int32);
        size_t t = 0;
        for (const Primitive& p : geo->primitives()) {
            CHECK(!p.closed && p.count == 11);
            CHECK_NEAR(geo->x()[p.start], seeds->x()[t]);
            CHECK_NEAR(geo->y()[p.start], seeds->y()[t]);
            CHECK_NEAR(u->at(p.start), 0.0f);
            CHECK_NEAR(u->at(p.start + 10), 1.0f);
            for (uint32_t k = 0; k < p.count; ++k) {
                CHECK(rand->at(p.start + k) == seeds->findAttrib(AttribClass::point, "rand")->at(t));
                CHECK(trail->at(p.start + k) == static_cast<float>(t));
                if (k > 0) CHECK(std::fabs(std::hypot(geo->x()[p.start + k] - geo->x()[p.start + k - 1], geo->y()[p.start + k] - geo->y()[p.start + k - 1]) - 0.2f) < 1e-4f);
            }
            ++t;
        }
    }

    // Both ways: the seed sits in the middle of a trail twice as long.
    net.node(flow).setFloat("bothWays", 1.0f);
    auto both = net.cook(flow);
    CHECK(both->pointCount() == 2100 && both->primitives()[0].count == 21);
    CHECK_NEAR(both->x()[10], seeds->x()[0]);
    CHECK_NEAR(both->y()[10], seeds->y()[0]);

    Network threaded(5);
    NodeId g2 = threaded.addNode("generate"), f2 = threaded.addNode("flow");
    threaded.connect(g2, f2);
    threaded.node(g2).setFloat("count", 100.0f);
    for (const char* p : {"steps", "stepLength", "bothWays"}) threaded.node(f2).setFloat(p, net.node(flow).getFloat(p));
    threaded.node(f2).setString("mode", "angle");
    net.node(flow).setString("mode", "angle");
    CHECK(sameGeometry(*net.cook(flow), *threaded.cook(f2)));

    net.node(flow).setString("mode", "spiral");
    CHECK(cookThrows(net, flow));
}

TEST(particlesSimulateDeterministicallyAndFollowThePlayhead) {
    Network net(2);
    NodeId gen = net.addNode("generate");
    net.node(gen).setFloat("count", 500.0f);
    NodeId sim = net.addNode("particles");
    net.connect(gen, sim);
    net.node(sim).setFloat("drag", 0.0f);
    net.node(sim).setFloat("gravityY", -10.0f);
    CHECK(net.dependsOnTime(sim));
    CHECK(!net.dependsOnTime(gen));

    auto start = net.cook(sim);
    CHECK(sameGeometry(*start, *net.cook(gen)) == false);  // the node adds vx, vy, age, life and speed
    CHECK_NEAR(start->y()[7], net.cook(gen)->y()[7]);

    // Free fall: y0 - g t^2 / 2 (semi-implicit Euler lands a fraction of a percent off).
    CHECK(net.setTime(1.0));
    auto fell = net.cook(sim);
    CHECK(std::fabs(fell->y()[7] - (net.cook(gen)->y()[7] - 5.0f)) < 0.08f);
    CHECK(std::fabs(fell->findAttrib(AttribClass::point, "vy")->at(7) + 10.0f) < 0.1f);
    CHECK(std::fabs(fell->findAttrib(AttribClass::point, "age")->at(7) - 1.0f) < 0.01f);

    // Continuing from the cached state, or restarting after scrubbing back, gives what a fresh network gives at that time.
    auto fresh = [&](double t, size_t threads) {
        Network other(threads);
        NodeId g = other.addNode("generate"), s = other.addNode("particles");
        other.connect(g, s);
        other.node(g).setFloat("count", 500.0f);
        other.node(s).setFloat("drag", 0.0f);
        other.node(s).setFloat("gravityY", -10.0f);
        other.node(s).setFloat("noiseForce", net.node(sim).getFloat("noiseForce"));
        other.setTime(t);
        return other.cook(s);
    };
    net.node(sim).setFloat("noiseForce", 3.0f);
    net.setTime(0.5);
    net.cook(sim);
    net.setTime(1.25);
    auto later = net.cook(sim);
    CHECK(sameGeometry(*later, *fresh(1.25, 1)));
    CHECK(sameGeometry(*later, *fresh(1.25, 4)));
    net.setTime(0.3);
    CHECK(sameGeometry(*net.cook(sim), *fresh(0.3, 3)));

    // A different parameter restarts the simulation.
    net.node(sim).setFloat("gravityY", 0.0f);
    net.node(sim).setFloat("noiseForce", 0.0f);
    net.setTime(1.0);
    CHECK_NEAR(net.cook(sim)->y()[7], net.cook(gen)->y()[7]);

    // Before startTime nothing has moved.
    net.node(sim).setFloat("gravityY", -10.0f);
    net.node(sim).setFloat("startTime", 5.0f);
    CHECK_NEAR(net.cook(sim)->y()[7], net.cook(gen)->y()[7]);
    net.node(sim).setFloat("startTime", 0.0f);

    // A bouncing floor and a wrapping box keep the particles inside.
    net.node(sim).setString("boundary", "bounce");
    net.node(sim).setFloat("boundsMinY", -1.0f);
    net.node(sim).setFloat("boundsMaxY", 6.0f);
    net.setTime(3.0);
    auto bounced = net.cook(sim);
    for (size_t i = 0; i < bounced->pointCount(); ++i) CHECK(bounced->y()[i] >= -1.0f && bounced->y()[i] <= 6.0f && bounced->x()[i] >= -10.0f && bounced->x()[i] <= 10.0f);
    net.node(sim).setString("boundary", "wrap");
    auto wrapped = net.cook(sim);
    for (size_t i = 0; i < wrapped->pointCount(); ++i) CHECK(wrapped->y()[i] >= -1.0f && wrapped->y()[i] <= 6.0f);

    // Emission with a lifetime keeps respawning: ages stay below the lifetime and the particles stay near the nozzle's reach.
    net.node(sim).setString("boundary", "none");
    net.node(sim).setFloat("gravityY", 0.0f);
    net.node(sim).setFloat("lifetime", 0.5f);
    net.node(sim).setFloat("initialSpeed", 4.0f);
    net.setTime(4.0);
    auto emitted = net.cook(sim);
    for (size_t i = 0; i < emitted->pointCount(); ++i) {
        const float life = emitted->findAttrib(AttribClass::point, "life")->at(i);
        CHECK(life >= 0.0f && life < 1.0f);
        CHECK(std::hypot(emitted->x()[i] - net.cook(gen)->x()[i], emitted->y()[i] - net.cook(gen)->y()[i]) <= 4.0f * 0.5f + 0.05f);
    }

    // A single particle falls towards an attractor.
    net.node(gen).setFloat("count", 1.0f);
    net.node(gen).setFloat("cx", 5.0f);
    net.node(gen).setFloat("cy", 0.0f);
    net.node(gen).setFloat("width", 0.0f);
    net.node(gen).setFloat("height", 0.0f);
    net.node(sim).setFloat("lifetime", 0.0f);
    net.node(sim).setFloat("initialSpeed", 0.0f);
    net.node(sim).setFloat("attract", 20.0f);
    net.node(sim).setFloat("attractX", 0.0f);
    net.setTime(0.5);
    CHECK(net.cook(sim)->x()[0] < 5.0f);

    net.node(sim).setString("boundary", "teleport");
    CHECK(cookThrows(net, sim));
    net.node(sim).setString("boundary", "bounce");
    net.node(sim).setFloat("boundsMaxX", -20.0f);
    CHECK(cookThrows(net, sim));
}

double alignmentOf(const Geometry& geo) {
    double sx = 0.0, sy = 0.0, total = 0.0;
    for (size_t i = 0; i < geo.pointCount(); ++i) {
        const double vx = geo.findAttrib(AttribClass::point, "vx")->at(i), vy = geo.findAttrib(AttribClass::point, "vy")->at(i);
        sx += vx;
        sy += vy;
        total += std::hypot(vx, vy);
    }
    return std::hypot(sx, sy) / total;
}

TEST(boidsFlockAlignAndStayWithinTheirSpeedLimits) {
    auto build = [](Network& net, NodeId& gen, NodeId& sim) {
        gen = net.addNode("generate");
        net.node(gen).setFloat("count", 200.0f);
        net.node(gen).setFloat("width", 6.0f);
        net.node(gen).setFloat("height", 6.0f);
        sim = net.addNode("boids");
        net.connect(gen, sim);
        net.node(sim).setFloat("radius", 2.0f);
    };
    Network net(2);
    NodeId gen, sim;
    build(net, gen, sim);
    CHECK(net.dependsOnTime(sim));
    auto start = net.cook(sim);
    CHECK(start->pointCount() == 200 && start->hasAttrib(AttribClass::point, "neighbors") && start->hasAttrib(AttribClass::point, "angle"));
    const double before = alignmentOf(*start);
    net.setTime(6.0);
    auto later = net.cook(sim);
    CHECK(alignmentOf(*later) > before + 0.3);
    for (size_t i = 0; i < later->pointCount(); ++i) {
        const float speed = later->findAttrib(AttribClass::point, "speed")->at(i);
        CHECK(speed >= 1.0f - 1e-3f && speed <= 3.0f + 1e-3f);
        CHECK(later->x()[i] >= -10.0f && later->x()[i] <= 10.0f && later->y()[i] >= -6.0f && later->y()[i] <= 6.0f);
    }

    // Incremental, restarted and threaded runs agree with a fresh network.
    auto fresh = [&](double t, size_t threads) {
        Network other(threads);
        NodeId g, s;
        build(other, g, s);
        other.setTime(t);
        return other.cook(s);
    };
    CHECK(sameGeometry(*later, *fresh(6.0, 1)));
    CHECK(sameGeometry(*later, *fresh(6.0, 4)));
    net.setTime(2.0);
    CHECK(sameGeometry(*net.cook(sim), *fresh(2.0, 3)));

    // Without any rule the boids just fly straight at their initial speed.
    net.node(sim).setFloat("separation", 0.0f);
    net.node(sim).setFloat("alignment", 0.0f);
    net.node(sim).setFloat("cohesion", 0.0f);
    net.node(sim).setString("boundary", "none");
    net.setTime(0.0);
    auto s0 = net.cook(sim);
    net.setTime(1.0);
    auto s1 = net.cook(sim);
    CHECK_NEAR(s1->x()[5] - s0->x()[5], s1->findAttrib(AttribClass::point, "vx")->at(5));

    // A goal pulls the flock towards it.
    net.node(sim).setFloat("goal", 4.0f);
    net.node(sim).setFloat("goalX", 100.0f);
    net.node(sim).setFloat("goalY", 0.0f);
    net.setTime(8.0);
    double meanVx = 0.0;
    auto goalGeo = net.cook(sim);
    for (size_t i = 0; i < goalGeo->pointCount(); ++i) meanVx += goalGeo->findAttrib(AttribClass::point, "vx")->at(i);
    CHECK(meanVx / static_cast<double>(goalGeo->pointCount()) > 1.5);

    net.node(sim).setString("boundary", "teleport");
    CHECK(cookThrows(net, sim));
    net.node(sim).setString("boundary", "bounce");
    net.node(sim).setFloat("radius", 0.0f);
    CHECK(cookThrows(net, sim));
}

TEST(springsHangAChainThatKeepsItsLengthAndHonoursPins) {
    auto build = [](Network& net, NodeId& line, NodeId& sim) {
        line = net.addNode("line");
        net.node(line).setFloat("count", 20.0f);
        sim = net.addNode("springs");
        net.connect(line, sim);
        net.node(sim).setFloat("pinEnds", 1.0f);
    };
    Network net(2);
    NodeId line, sim;
    build(net, line, sim);
    auto rest = net.cook(line);
    CHECK(net.dependsOnTime(sim));
    CHECK(sameGeometry(*net.cook(sim), *rest) == false);
    CHECK_NEAR(net.cook(sim)->x()[7], rest->x()[7]);

    net.setTime(4.0);
    auto swung = net.cook(sim);
    CHECK(swung->primitiveCount() == rest->primitiveCount());
    CHECK_NEAR(swung->x()[0], rest->x()[0]);  // the pinned end does not move
    CHECK_NEAR(swung->y()[0], rest->y()[0]);
    CHECK(swung->y()[19] < rest->y()[19] - 1.0f);
    double restLength = 0.0, length = 0.0;
    for (size_t i = 1; i < rest->pointCount(); ++i) {
        restLength += std::hypot(static_cast<double>(rest->x()[i] - rest->x()[i - 1]), static_cast<double>(rest->y()[i] - rest->y()[i - 1]));
        length += std::hypot(static_cast<double>(swung->x()[i] - swung->x()[i - 1]), static_cast<double>(swung->y()[i] - swung->y()[i - 1]));
    }
    CHECK(std::fabs(length - restLength) < 0.05 * restLength);
    CHECK(swung->hasAttrib(AttribClass::point, "strain") && swung->hasAttrib(AttribClass::point, "speed"));

    auto fresh = [&](double t, size_t threads) {
        Network other(threads);
        NodeId l, s;
        build(other, l, s);
        other.setTime(t);
        return other.cook(s);
    };
    net.setTime(1.5);
    net.cook(sim);
    net.setTime(3.0);
    CHECK(sameGeometry(*net.cook(sim), *fresh(3.0, 1)));
    CHECK(sameGeometry(*net.cook(sim), *fresh(3.0, 4)));
    net.setTime(0.7);
    CHECK(sameGeometry(*net.cook(sim), *fresh(0.7, 2)));

    // Without pins everything falls, and a stiffer or looser rest length changes the shape.
    net.node(sim).setFloat("pinEnds", 0.0f);
    net.setTime(1.0);
    CHECK(net.cook(sim)->y()[0] < rest->y()[0] - 1.0f);
    net.node(sim).setFloat("neighborRadius", 1.5f);
    CHECK(net.cook(sim)->pointCount() == 20);

    net.node(sim).setFloat("restScale", 0.0f);
    CHECK(cookThrows(net, sim));
}

TEST(springsCanOutputTheirSpringsAsLines) {
    Network net(2);
    NodeId line = net.addNode("line");
    net.node(line).setFloat("count", 20.0f);
    NodeId wr = net.addNode("wrangle");
    net.connect(line, wr);
    net.node(wr).setString("code", "tag = x * 2;");
    NodeId sim = net.addNode("springs");
    net.connect(wr, sim);
    net.node(sim).setFloat("pinEnds", 1.0f);
    net.setTime(2.0);
    auto points = net.cook(sim);
    net.node(sim).setString("output", "springs");
    auto lines = net.cook(sim);
    CHECK(lines->pointCount() == 38 && lines->primitiveCount() == 19);
    for (const Primitive& p : lines->primitives()) CHECK(p.count == 2 && !p.closed);
    const AttribArray* tag = lines->findAttrib(AttribClass::point, "tag");
    CHECK(tag && lines->hasAttrib(AttribClass::point, "strain") && lines->hasAttrib(AttribClass::point, "spring"));
    // Spring k joins points k and k + 1: its ends sit where the points are and carry their attributes.
    for (size_t k = 0; k < 19; ++k) {
        CHECK_NEAR(lines->x()[2 * k], points->x()[k]);
        CHECK_NEAR(lines->y()[2 * k + 1], points->y()[k + 1]);
        CHECK_NEAR(tag->at(2 * k), points->findAttrib(AttribClass::point, "tag")->at(k));
        CHECK_NEAR(tag->at(2 * k + 1), points->findAttrib(AttribClass::point, "tag")->at(k + 1));
        CHECK(lines->findAttrib(AttribClass::point, "spring")->ints()[2 * k] == static_cast<int32_t>(k));
    }
    // Lines from neighborRadius come from the mesh, not from primitives.
    net.node(sim).setFloat("neighborRadius", 1.2f);
    CHECK(net.cook(sim)->primitiveCount() > 19);
    net.node(sim).setString("output", "mesh");
    CHECK(cookThrows(net, sim));
}

TEST(boidsAvoidObstaclesFromTheirExtraInputs) {
    Network net(2);
    NodeId gen = net.addNode("generate");
    net.node(gen).setFloat("count", 300.0f);
    net.node(gen).setFloat("width", 10.0f);
    net.node(gen).setFloat("height", 10.0f);
    NodeId sim = net.addNode("boids");
    net.connect(gen, sim);
    net.node(sim).setFloat("radius", 1.5f);
    net.node(sim).setFloat("boundsMinX", -8.0f);
    net.node(sim).setFloat("boundsMaxX", 8.0f);
    net.node(sim).setFloat("boundsMinY", -8.0f);
    net.node(sim).setFloat("boundsMaxY", 8.0f);
    auto insideCount = [&](const Geometry& g, float radius) {
        size_t inside = 0;
        for (size_t i = 0; i < g.pointCount(); ++i) inside += std::hypot(g.x()[i], g.y()[i]) < radius ? 1 : 0;
        return inside;
    };
    net.setTime(4.0);
    CHECK(insideCount(*net.cook(sim), 2.0f) > 0);  // without an obstacle the flock flies through the middle

    NodeId rock = net.addNode("generate");
    net.node(rock).setFloat("count", 1.0f);
    net.node(rock).setFloat("width", 0.0f);
    net.node(rock).setFloat("height", 0.0f);
    net.connect(rock, sim, 1);
    net.node(sim).setFloat("obstacleRadius", 2.0f);
    for (double t : {0.5, 1.0, 4.0}) {
        net.setTime(t);
        CHECK(insideCount(*net.cook(sim), 2.0f - 0.01f) == 0);
    }

    Network threaded(4);
    NodeId g2 = threaded.addNode("generate"), r2 = threaded.addNode("generate"), s2 = threaded.addNode("boids");
    threaded.connect(g2, s2);
    threaded.connect(r2, s2, 1);
    for (const char* p : {"count", "width", "height"}) threaded.node(g2).setFloat(p, net.node(gen).getFloat(p));
    for (const char* p : {"count", "width", "height"}) threaded.node(r2).setFloat(p, net.node(rock).getFloat(p));
    for (const char* p : {"radius", "boundsMinX", "boundsMaxX", "boundsMinY", "boundsMaxY", "obstacleRadius"}) threaded.node(s2).setFloat(p, net.node(sim).getFloat(p));
    threaded.setTime(4.0);
    net.setTime(4.0);
    CHECK(sameGeometry(*net.cook(sim), *threaded.cook(s2)));

    // An obstacle's own radius attribute wins over the parameter.
    NodeId sized = net.addNode("wrangle");
    net.connect(rock, sized);
    net.node(sized).setString("code", "radius = 4;");
    net.connect(sized, sim, 1);
    CHECK(insideCount(*net.cook(sim), 4.0f - 0.01f) == 0);
}

TEST(lsystemGrowsPlantsAndReportsBadRules) {
    Network net(1);
    NodeId ls = net.addNode("lsystem");
    net.node(ls).setString("axiom", "F");
    net.node(ls).setString("rules", "F=FF");
    net.node(ls).setFloat("iterations", 3.0f);
    net.node(ls).setFloat("length", 0.5f);
    auto trunk = net.cook(ls);
    CHECK(trunk->pointCount() == 9 && trunk->primitiveCount() == 1);
    CHECK_NEAR(trunk->x()[8], 0.0f);
    CHECK_NEAR(trunk->y()[8], 4.0f);  // 8 steps of 0.5 straight up
    CHECK_NEAR(trunk->findAttrib(AttribClass::point, "u")->at(8), 1.0f);

    // Four right-angle turns close a square; a branch starts a new stroke one level deeper.
    net.node(ls).setString("axiom", "F+F+F+F");
    net.node(ls).setString("rules", "");
    net.node(ls).setFloat("angle", 90.0f);
    net.node(ls).setFloat("length", 1.0f);
    auto square = net.cook(ls);
    CHECK(square->pointCount() == 5);
    CHECK_NEAR(square->x()[4], square->x()[0]);
    CHECK_NEAR(square->y()[4], square->y()[0]);
    net.node(ls).setString("axiom", "F[+F]F");
    auto branched = net.cook(ls);
    CHECK(branched->primitiveCount() == 3);
    bool sawDeep = false;
    for (size_t i = 0; i < branched->pointCount(); ++i) sawDeep |= branched->findAttrib(AttribClass::point, "depth")->ints()[i] == 1;
    CHECK(sawDeep);

    // A plant: deterministic, and random rule choices and jitter depend on the seed only.
    net.node(ls).setString("axiom", "X");
    net.node(ls).setString("rules", "X=F[+X][-X]FX; X=F[+X]F[-X]; F=FF");
    net.node(ls).setFloat("iterations", 6.0f);
    net.node(ls).setFloat("angleJitter", 8.0f);
    net.node(ls).setFloat("lengthJitter", 0.2f);
    net.node(ls).setFloat("branchScale", 0.9f);
    auto plant = net.cook(ls);
    CHECK(plant->pointCount() > 1000 && plant->primitiveCount() > 100);
    Network other(4);
    NodeId l2 = other.addNode("lsystem");
    for (const char* p : {"iterations", "angleJitter", "lengthJitter", "branchScale", "angle", "length"}) other.node(l2).setFloat(p, net.node(ls).getFloat(p));
    for (const char* p : {"axiom", "rules"}) other.node(l2).setString(p, net.node(ls).getString(p));
    CHECK(sameGeometry(*plant, *other.cook(l2)));
    net.node(ls).setFloat("seed", 2.0f);
    CHECK(!sameGeometry(*plant, *net.cook(ls)));

    net.node(ls).setString("rules", "XY=F");
    CHECK(cookThrows(net, ls));
    net.node(ls).setString("rules", "F=FF");
    net.node(ls).setString("axiom", "F]");
    CHECK(cookThrows(net, ls));
    net.node(ls).setString("axiom", "");
    CHECK(cookThrows(net, ls));
    net.node(ls).setString("axiom", "F");
    net.node(ls).setFloat("iterations", 30.0f);
    CHECK(cookThrows(net, ls));  // 2^30 symbols is over the limit
}

TEST(curveSampleSpacesPointsAlongCurvesWithFramesAndAttributes) {
    Network net(2);
    NodeId line = net.addNode("line");
    net.node(line).setFloat("count", 11.0f);
    net.node(line).setFloat("length", 10.0f);
    net.node(line).setFloat("y", 3.0f);
    NodeId wr = net.addNode("wrangle");
    net.connect(line, wr);
    net.node(wr).setString("code", "w = x * 2;");
    NodeId cs = net.addNode("curveSample");
    net.connect(wr, cs);
    net.node(cs).setFloat("count", 5.0f);
    auto even = net.cook(cs);
    CHECK(even->pointCount() == 5 && even->primitiveCount() == 0);
    for (size_t i = 0; i < 5; ++i) {
        CHECK_NEAR(even->x()[i], 2.5f * static_cast<float>(i));
        CHECK_NEAR(even->y()[i], 3.0f);
        CHECK_NEAR(even->findAttrib(AttribClass::point, "u")->at(i), 0.25f * static_cast<float>(i));
        CHECK_NEAR(even->findAttrib(AttribClass::point, "w")->at(i), 5.0f * static_cast<float>(i));  // interpolated between points
        CHECK_NEAR(even->findAttrib(AttribClass::point, "tx")->at(i), 1.0f);
        CHECK_NEAR(even->findAttrib(AttribClass::point, "ny")->at(i), 1.0f);
        CHECK(even->findAttrib(AttribClass::point, "curve")->ints()[i] == 0);
    }

    net.node(cs).setFloat("offset", 0.5f);
    CHECK_NEAR(net.cook(cs)->y()[2], 3.5f);  // along the normal, to the left of the direction of travel
    net.node(cs).setFloat("offset", 0.0f);

    net.node(cs).setFloat("spacing", 2.0f);
    auto spaced = net.cook(cs);
    CHECK(spaced->pointCount() == 6);
    CHECK_NEAR(spaced->x()[5], 10.0f);

    net.node(cs).setFloat("spacing", 0.0f);
    net.node(cs).setFloat("count", 400.0f);
    net.node(cs).setFloat("random", 1.0f);
    auto scattered = net.cook(cs);
    CHECK(scattered->pointCount() == 400);
    for (size_t i = 0; i < 400; ++i) {
        CHECK(scattered->x()[i] >= 0.0f && scattered->x()[i] <= 10.0f);
        CHECK_NEAR(scattered->findAttrib(AttribClass::point, "w")->at(i), scattered->x()[i] * 2.0f);
    }
    Network other(4);
    NodeId l2 = other.addNode("line"), w2 = other.addNode("wrangle"), c2 = other.addNode("curveSample");
    other.connect(l2, w2);
    other.connect(w2, c2);
    other.node(l2).setFloat("count", 11.0f);
    other.node(l2).setFloat("y", 3.0f);
    other.node(w2).setString("code", "w = x * 2;");
    other.node(c2).setFloat("count", 400.0f);
    other.node(c2).setFloat("random", 1.0f);
    CHECK(sameGeometry(*scattered, *other.cook(c2)));

    net.node(line).setFloat("count", 1.0f);
    CHECK(net.cook(cs)->pointCount() == 0);  // nothing to sample on a single point
}

TEST(boidsObstaclesDriftWithTheirVelocityWithoutRestarting) {
    Network net(2);
    NodeId gen = net.addNode("generate");
    net.node(gen).setFloat("count", 300.0f);
    net.node(gen).setFloat("width", 10.0f);
    net.node(gen).setFloat("height", 10.0f);
    NodeId rock = net.addNode("generate");
    net.node(rock).setFloat("count", 1.0f);
    net.node(rock).setFloat("width", 0.0f);
    net.node(rock).setFloat("height", 0.0f);
    NodeId drift = net.addNode("wrangle");
    net.connect(rock, drift);
    net.node(drift).setString("code", "vx = 1.5; radius = 1.5;");
    NodeId sim = net.addNode("boids");
    net.connect(gen, sim);
    net.connect(drift, sim, 1);
    net.node(sim).setFloat("radius", 1.5f);
    for (const char* p : {"boundsMinX", "boundsMinY"}) net.node(sim).setFloat(p, -12.0f);
    for (const char* p : {"boundsMaxX", "boundsMaxY"}) net.node(sim).setFloat(p, 12.0f);

    // The rock is at x = 1.5 * t, and no boid is ever inside it.
    std::vector<std::shared_ptr<const Geometry>> walked;
    for (double t : {0.5, 1.0, 2.0, 3.0}) {
        net.setTime(t);
        auto g = net.cook(sim);
        const float cx = 1.5f * static_cast<float>(t);
        for (size_t i = 0; i < g->pointCount(); ++i) CHECK(std::hypot(g->x()[i] - cx, g->y()[i]) >= 1.5f - 0.01f);
        walked.push_back(g);
    }

    // A pure function of time: jumping straight to t = 3 gives what playing up to it gave.
    Network jump(2);
    NodeId g2 = jump.addNode("generate"), r2 = jump.addNode("generate"), w2 = jump.addNode("wrangle"), s2 = jump.addNode("boids");
    jump.connect(g2, s2);
    jump.connect(r2, w2);
    jump.connect(w2, s2, 1);
    for (const char* p : {"count", "width", "height"}) jump.node(g2).setFloat(p, net.node(gen).getFloat(p));
    for (const char* p : {"count", "width", "height"}) jump.node(r2).setFloat(p, net.node(rock).getFloat(p));
    jump.node(w2).setString("code", "vx = 1.5; radius = 1.5;");
    for (const char* p : {"radius", "boundsMinX", "boundsMinY", "boundsMaxX", "boundsMaxY"}) jump.node(s2).setFloat(p, net.node(sim).getFloat(p));
    jump.setTime(3.0);
    CHECK(sameGeometry(*walked.back(), *jump.cook(s2)));

    // The velocity matters: a rock that stays put leaves the flock somewhere else.
    net.node(drift).setString("code", "radius = 1.5;");
    net.setTime(3.0);
    CHECK(!sameGeometry(*walked.back(), *net.cook(sim)));
}

TEST(ribbonWidensCurvesIntoOutlinesEdgesAndRungs) {
    Network net(2);
    NodeId line = net.addNode("line");
    net.node(line).setFloat("count", 11.0f);
    net.node(line).setFloat("length", 10.0f);
    net.node(line).setFloat("y", 3.0f);
    NodeId rb = net.addNode("ribbon");
    net.connect(line, rb);
    net.node(rb).setFloat("width", 2.0f);
    net.node(rb).setFloat("widthEnd", 2.0f);
    auto outline = net.cook(rb);
    CHECK(outline->pointCount() == 22 && outline->primitiveCount() == 1);
    CHECK(outline->primitives()[0].closed && outline->primitives()[0].count == 22);
    for (size_t i = 0; i < 22; ++i) {
        const float side = outline->findAttrib(AttribClass::point, "side")->at(i);
        CHECK_NEAR(outline->y()[i], 3.0f + side);  // left of +x is +y
        CHECK_NEAR(outline->findAttrib(AttribClass::point, "width")->at(i), 2.0f);
    }
    CHECK(outline->findAttrib(AttribClass::point, "side")->at(0) == 1.0f && outline->findAttrib(AttribClass::point, "side")->at(21) == -1.0f);

    net.node(rb).setString("output", "edges");
    auto edges = net.cook(rb);
    CHECK(edges->primitiveCount() == 2 && !edges->primitives()[0].closed && edges->primitives()[1].start == 11);
    net.node(rb).setString("output", "rungs");
    auto rungs = net.cook(rb);
    CHECK(rungs->primitiveCount() == 11 && rungs->primitives()[3].count == 2);
    CHECK_NEAR(rungs->y()[6] - rungs->y()[7], 2.0f);

    // Width runs from one end to the other, tapers pinch the ends, and an attribute scales it.
    net.node(rb).setString("output", "outline");
    net.node(rb).setFloat("width", 0.0f);
    net.node(rb).setFloat("widthEnd", 4.0f);
    auto grown = net.cook(rb);
    CHECK_NEAR(grown->findAttrib(AttribClass::point, "width")->at(0), 0.0f);
    CHECK_NEAR(grown->findAttrib(AttribClass::point, "width")->at(10), 4.0f);
    CHECK_NEAR(grown->findAttrib(AttribClass::point, "width")->at(5), 2.0f);
    net.node(rb).setFloat("width", 2.0f);
    net.node(rb).setFloat("widthEnd", 2.0f);
    net.node(rb).setFloat("taper", 0.2f);
    auto pinched = net.cook(rb);
    CHECK_NEAR(pinched->findAttrib(AttribClass::point, "width")->at(0), 0.0f);
    CHECK_NEAR(pinched->findAttrib(AttribClass::point, "width")->at(10), 0.0f);
    CHECK_NEAR(pinched->findAttrib(AttribClass::point, "width")->at(5), 2.0f);
    net.node(rb).setFloat("taper", 0.0f);
    NodeId wr = net.addNode("wrangle");
    net.connect(line, wr);
    net.node(wr).setString("code", "w = x;");
    net.connect(wr, rb);
    net.node(rb).setString("widthAttrib", "w");
    auto scaled = net.cook(rb);
    CHECK_NEAR(scaled->findAttrib(AttribClass::point, "width")->at(4), 2.0f * scaled->findAttrib(AttribClass::point, "w")->at(4));
    net.node(rb).setString("widthAttrib", "missing");
    CHECK(cookThrows(net, rb));
    net.node(rb).setString("widthAttrib", "");
    net.node(rb).setString("output", "sideways");
    CHECK(cookThrows(net, rb));

    // Closed curves give two closed rings and every point keeps its source attributes.
    Network cells(2);
    NodeId pts = cells.addNode("generate"), vor = cells.addNode("voronoi"), rib = cells.addNode("ribbon");
    cells.node(pts).setFloat("count", 12.0f);
    cells.connect(pts, vor);
    cells.connect(vor, rib);
    cells.node(rib).setString("output", "edges");
    auto base = cells.cook(vor);
    auto rings = cells.cook(rib);
    CHECK(rings->primitiveCount() == 2 * base->primitiveCount());
    CHECK(rings->pointCount() == 2 * base->pointCount());
    for (const Primitive& p : rings->primitives()) CHECK(p.closed);

    Network other(4);
    NodeId l2 = other.addNode("line"), r2 = other.addNode("ribbon");
    other.connect(l2, r2);
    other.node(l2).setFloat("count", 11.0f);
    other.node(l2).setFloat("length", 10.0f);
    other.node(l2).setFloat("y", 3.0f);
    other.node(r2).setFloat("width", 0.0f);
    other.node(r2).setFloat("widthEnd", 4.0f);
    CHECK(sameGeometry(*grown, *other.cook(r2)));
}

TEST(trailDrawsStreaksBehindMovingPoints) {
    Network net(2);
    NodeId gen = net.addNode("generate");
    net.node(gen).setFloat("count", 20.0f);
    NodeId wr = net.addNode("wrangle");
    net.connect(gen, wr);
    net.node(wr).setString("code", "vx = 2; vy = 0;");
    NodeId tr = net.addNode("trail");
    net.connect(wr, tr);
    net.node(tr).setFloat("length", 0.5f);
    net.node(tr).setFloat("segments", 4.0f);
    auto base = net.cook(wr);
    auto g = net.cook(tr);
    CHECK(g->pointCount() == 20 * 5 && g->primitiveCount() == 20);
    for (size_t p = 0; p < 20; ++p) {
        const size_t head = p * 5, tail = p * 5 + 4;
        CHECK_NEAR(g->x()[head], base->x()[p]);
        CHECK_NEAR(g->x()[tail], base->x()[p] - 1.0f);  // 2 per second for half a second, behind the point
        CHECK_NEAR(g->y()[tail], base->y()[p]);
        CHECK_NEAR(g->findAttrib(AttribClass::point, "u")->at(tail), 1.0f);
        CHECK_NEAR(g->findAttrib(AttribClass::point, "alpha")->at(head), 1.0f);
        CHECK_NEAR(g->findAttrib(AttribClass::point, "alpha")->at(tail), 0.0f);
        CHECK(g->findAttrib(AttribClass::point, "trail")->ints()[tail] == static_cast<int32_t>(p));
        CHECK(g->primitives()[p].start == head && g->primitives()[p].count == 5 && !g->primitives()[p].closed);
    }
    net.node(tr).setFloat("maxLength", 0.25f);
    CHECK_NEAR(net.cook(tr)->x()[4], base->x()[0] - 0.25f);
    net.node(tr).setFloat("fade", 0.0f);
    CHECK_NEAR(net.cook(tr)->findAttrib(AttribClass::point, "alpha")->at(4), 1.0f);
    net.connect(gen, tr);
    CHECK(cookThrows(net, tr));  // no velocity to follow
}

TEST(reactionDiffusionGrowsPatternsDeterministicallyAndStaysBounded) {
    auto build = [](Network& net, NodeId& sim) {
        sim = net.addNode("reactionDiffusion");
        net.node(sim).setFloat("width", 48.0f);
        net.node(sim).setFloat("height", 32.0f);
        net.node(sim).setFloat("stepsPerSecond", 200.0f);
    };
    Network net(2);
    NodeId sim;
    build(net, sim);
    CHECK(net.dependsOnTime(sim));
    auto start = net.cook(sim);
    CHECK(start->pointCount() == 48 * 32);
    size_t seeded = 0;
    for (size_t i = 0; i < start->pointCount(); ++i) seeded += start->findAttrib(AttribClass::point, "v")->at(i) > 0.0f ? 1 : 0;
    CHECK(seeded > 0 && seeded < start->pointCount() / 2);

    net.setTime(5.0);
    auto grown = net.cook(sim);
    size_t active = 0;
    for (size_t i = 0; i < grown->pointCount(); ++i) {
        const float u = grown->findAttrib(AttribClass::point, "u")->at(i), v = grown->findAttrib(AttribClass::point, "v")->at(i);
        CHECK(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f);
        active += v > 0.05f ? 1 : 0;
    }
    CHECK(active > seeded);

    auto fresh = [&](double t, size_t threads) {
        Network other(threads);
        NodeId s;
        build(other, s);
        other.setTime(t);
        return other.cook(s);
    };
    CHECK(sameGeometry(*grown, *fresh(5.0, 1)));
    CHECK(sameGeometry(*grown, *fresh(5.0, 4)));
    net.setTime(2.0);
    CHECK(sameGeometry(*net.cook(sim), *fresh(2.0, 3)));

    // The grid is centred on (cx, cy) with cellSize spacing, and a pattern-free setting (no feed) dies out.
    net.node(sim).setFloat("cx", 10.0f);
    net.node(sim).setFloat("cellSize", 0.5f);
    auto moved = net.cook(sim);
    CHECK_NEAR(moved->x()[1] - moved->x()[0], 0.5f);
    CHECK_NEAR(moved->x()[0] + moved->x()[47], 20.0f);
    net.node(sim).setFloat("feed", 0.0f);
    net.node(sim).setFloat("kill", 0.1f);
    net.setTime(20.0);
    auto dead = net.cook(sim);
    for (size_t i = 0; i < dead->pointCount(); ++i) CHECK(dead->findAttrib(AttribClass::point, "v")->at(i) < 0.05f);

    net.node(sim).setFloat("cellSize", 0.0f);
    CHECK(cookThrows(net, sim));
}

TEST(xxh64MatchesReferenceVectors) {
    const auto hash = [](const char* s) { return xxh64(s, std::strlen(s)); };
    CHECK(xxh64("", 0) == 0xEF46DB3751D8E999ull);
    CHECK(hash("abc") == 0x44BC2CF5AD770999ull);
    CHECK(hash("Nobody inspects the spammish repetition") == 0xFBCEA83C8A378BF1ull);
}

namespace {

bool sameBits(const Geometry& a, const Geometry& b) {
    if (a.pointCount() != b.pointCount() || a.primitiveCount() != b.primitiveCount() || a.details().size() != b.details().size()) return false;
    for (size_t i = 0; i < a.primitiveCount(); ++i) {
        const Primitive &p = a.primitives()[i], &q = b.primitives()[i];
        if (p.start != q.start || p.count != q.count || p.closed != q.closed || p.kind != q.kind) return false;
    }
    for (auto l = a.details().begin(), r = b.details().begin(); l != a.details().end(); ++l, ++r)
        if (l->first != r->first || std::memcmp(&l->second, &r->second, 4) != 0) return false;
    for (AttribClass cls : {AttribClass::point, AttribClass::primitive}) {
        const auto &left = a.attribs(cls), &right = b.attribs(cls);
        if (left.size() != right.size()) return false;
        for (auto l = left.begin(), r = right.begin(); l != left.end(); ++l, ++r) {
            if (l->first != r->first || l->second.type() != r->second.type() || l->second.size() != r->second.size()) return false;
            if (std::memcmp(l->second.data(), r->second.data(), l->second.paddedSize() * 4) != 0) return false;
        }
    }
    return true;
}

Geometry sampleGeometry() {
    Geometry g;
    g.resizePoints(37);
    for (size_t i = 0; i < 37; ++i) {
        g.x()[i] = static_cast<float>(i) * 0.5f;
        g.y()[i] = -static_cast<float>(i * i);
    }
    AttribArray& tricky = g.attrib(AttribClass::point, "tricky");
    const uint32_t bits[] = {0x80000000u, 0x7fc00001u, 0x7f800000u, 0xff800000u, 0x00000001u};
    for (size_t i = 0; i < 5; ++i) std::memcpy(&tricky.floats()[i], &bits[i], 4);
    AttribArray& id = g.attrib(AttribClass::point, "id", AttribType::int32);
    for (size_t i = 0; i < 37; ++i) id.ints()[i] = static_cast<int32_t>(i) - 20;
    g.addPrimitive({0, 12, true, PrimKind::polyline});
    g.addPrimitive({12, 25, false, PrimKind::cubic});
    g.attrib(AttribClass::primitive, "w").floats()[1] = 3.5f;
    g.attrib(AttribClass::primitive, "kind", AttribType::int32).ints()[0] = 7;
    g.setDetail("render.width", 640.0f);
    g.setDetail("render.gamma", -0.0f);
    return g;
}

void refreshChecksum(std::vector<uint8_t>& bytes) {
    const uint64_t h = xxh64(bytes.data() + 64, bytes.size() - 64);
    std::memcpy(bytes.data() + 32, &h, 8);
}

bool loadFails(const std::vector<uint8_t>& bytes, size_t length) {
    try {
        loadGeometry(bytes.data(), length);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

}  // namespace

TEST(geoFileRoundTripIsBitIdentical) {
    const Geometry g = sampleGeometry();
    const std::vector<uint8_t> bytes = saveGeometry(g);
    CHECK(bytes.size() % 64 == 0);
    const Geometry back = loadGeometry(bytes.data(), bytes.size());
    CHECK(sameBits(g, back));
    CHECK(saveGeometry(back) == bytes);

    const Geometry empty;
    const std::vector<uint8_t> emptyBytes = saveGeometry(empty);
    const Geometry emptyBack = loadGeometry(emptyBytes.data(), emptyBytes.size());
    CHECK(sameBits(empty, emptyBack));
    CHECK(emptyBack.pointCount() == 0 && emptyBack.primitiveCount() == 0);

    Geometry noPrims;
    noPrims.resizePoints(5);
    noPrims.setDetail("only", 1.0f);
    const std::vector<uint8_t> noPrimBytes = saveGeometry(noPrims);
    CHECK(sameBits(noPrims, loadGeometry(noPrimBytes.data(), noPrimBytes.size())));
}

TEST(geoFileAttributeArraysStayAlignedAndIndependent) {
    const Geometry g = sampleGeometry();
    const std::vector<uint8_t> bytes = saveGeometry(g);
    Geometry back = loadGeometry(bytes.data(), bytes.size());
    for (const auto& [name, array] : back.attribs(AttribClass::point)) CHECK(reinterpret_cast<uintptr_t>(array.data()) % 64 == 0);
    back.x()[0] = 99.0f;
    CHECK(g.x()[0] == 0.0f);
}

TEST(geoFileWritesAndReadsFiles) {
    const Geometry g = sampleGeometry();
    const std::string path = (std::filesystem::temp_directory_path() / "jitnodes_test_geometry.jngeo").string();
    saveGeometryToFile(g, path);
    CHECK(!std::filesystem::exists(path + ".tmp"));
    saveGeometryToFile(g, path);
    CHECK(sameBits(g, loadGeometryFromFile(path)));
    std::filesystem::remove(path);
    bool threw = false;
    try {
        loadGeometryFromFile(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(geoFileRejectsEveryTruncationAndByteFlip) {
    const std::vector<uint8_t> bytes = saveGeometry(sampleGeometry());
    for (size_t length = 0; length < bytes.size(); ++length) CHECK(loadFails(bytes, length));
    std::vector<uint8_t> extended = bytes;
    extended.push_back(0);
    CHECK(loadFails(extended, extended.size()));
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i == 8 || i == 9) continue;  // minor version: newer minors load
        std::vector<uint8_t> mutated = bytes;
        mutated[i] ^= 0x5a;
        CHECK(loadFails(mutated, mutated.size()));
    }
}

TEST(geoFileSurvivesMutationsWithAValidChecksum) {
    const std::vector<uint8_t> bytes = saveGeometry(sampleGeometry());
    size_t rejected = 0;
    for (size_t i = 64; i < bytes.size(); ++i) {
        for (uint8_t value : {uint8_t(0x00), uint8_t(0x01), uint8_t(0x7f), uint8_t(0x80), uint8_t(0xff)}) {
            std::vector<uint8_t> mutated = bytes;
            if (mutated[i] == value) continue;
            mutated[i] = value;
            refreshChecksum(mutated);
            try {
                const Geometry g = loadGeometry(mutated.data(), mutated.size());
                for (AttribClass cls : {AttribClass::point, AttribClass::primitive})
                    for (const auto& [name, array] : g.attribs(cls)) CHECK(array.size() == g.count(cls));
                for (const Primitive& p : g.primitives()) CHECK(static_cast<uint64_t>(p.start) + p.count <= g.pointCount());
            } catch (const std::runtime_error&) {
                ++rejected;
            }
        }
    }
    CHECK(rejected > 1000);
}

TEST(geoFileRejectsHugeCountsAndOffsets) {
    const std::vector<uint8_t> bytes = saveGeometry(sampleGeometry());
    const auto sectionOffset = [&](uint32_t id) {
        uint64_t offset;
        std::memcpy(&offset, bytes.data() + 64 + 24 * (id - 1) + 8, 8);
        return static_cast<size_t>(offset);
    };
    const auto patched = [&](size_t at, uint64_t value) {
        std::vector<uint8_t> mutated = bytes;
        std::memcpy(mutated.data() + at, &value, 8);
        refreshChecksum(mutated);
        return mutated;
    };
    for (uint64_t huge : {uint64_t(0x7fffffff), uint64_t(0x80000000), uint64_t(1) << 40, ~uint64_t(0)}) {
        CHECK(loadFails(patched(sectionOffset(2), huge), bytes.size()));
        CHECK(loadFails(patched(sectionOffset(2) + 8, huge), bytes.size()));
        CHECK(loadFails(patched(sectionOffset(3), huge), bytes.size()));
        CHECK(loadFails(patched(sectionOffset(4), huge), bytes.size()));
        CHECK(loadFails(patched(sectionOffset(5), huge), bytes.size()));
        CHECK(loadFails(patched(64 + 8, huge), bytes.size()));
        CHECK(loadFails(patched(64 + 16, huge), bytes.size()));
        CHECK(loadFails(patched(sectionOffset(4) + 8 + 16, huge), bytes.size()));
    }

    std::vector<uint8_t> future = bytes;
    future[10] = 2;
    refreshChecksum(future);
    CHECK(loadFails(future, future.size()));
    std::vector<uint8_t> minor = bytes;
    minor[8] = 5;
    refreshChecksum(minor);
    CHECK(!loadFails(minor, minor.size()));
}

TEST(fileReadPlaysABakedSequenceByFrame) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "jitnodes_test_sequence";
    std::filesystem::create_directories(dir);
    for (int frame = 1; frame <= 3; ++frame) {
        Geometry g;
        g.resizePoints(static_cast<size_t>(frame));
        saveGeometryToFile(g, (dir / ("f" + std::string(frame < 10 ? "000" : "00") + std::to_string(frame) + ".jngeo")).string());
    }
    Network net(2);
    net.setFps(10.0);
    const NodeId read = net.addNode("fileRead");
    net.node(read).setString("path", (dir / "f$F4.jngeo").string());
    CHECK(net.dependsOnTime(read));
    net.setTime(0.1);
    CHECK(net.cook(read)->pointCount() == 1);
    net.setTime(0.3);
    CHECK(net.cook(read)->pointCount() == 3);
    net.setTime(0.5);
    CHECK(cookThrows(net, read));

    net.node(read).setString("path", (dir / "f0002.jngeo").string());
    CHECK(!net.dependsOnTime(read));
    CHECK(net.cook(read)->pointCount() == 2);
    net.node(read).setString("path", "");
    CHECK(cookThrows(net, read));
    std::filesystem::remove_all(dir);
}

TEST(fileTrailFollowsPointsThroughRecordedFrames) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "jitnodes_test_trail";
    std::filesystem::create_directories(dir);
    for (int frame = 1; frame <= 4; ++frame) {
        Geometry g;
        g.resizePoints(2);
        g.x()[0] = static_cast<float>(frame);
        g.y()[0] = 0.0f;
        g.x()[1] = 0.0f;
        g.y()[1] = static_cast<float>(10 * frame);
        saveGeometryToFile(g, (dir / ("t" + std::to_string(frame) + ".jngeo")).string());
    }
    Network net(2);
    net.setFps(10.0);
    const NodeId trail = net.addNode("fileTrail");
    net.node(trail).setString("path", (dir / "t$F.jngeo").string());
    net.node(trail).setFloat("frames", 3.0f);
    CHECK(net.dependsOnTime(trail));

    net.setTime(0.4);
    GeometryHandle geo = net.cook(trail);
    CHECK(geo->pointCount() == 8);
    CHECK(geo->primitiveCount() == 2);
    for (size_t k = 0; k < 4; ++k) {
        CHECK(geo->x()[k] == static_cast<float>(4 - k));
        CHECK(geo->y()[4 + k] == static_cast<float>(10 * (4 - k)));
    }

    net.setTime(0.2);
    geo = net.cook(trail);
    CHECK(geo->x()[0] == 2.0f && geo->x()[1] == 1.0f);
    CHECK(geo->x()[2] == 1.0f && geo->x()[3] == 1.0f);

    net.node(trail).setString("path", (dir / "t0001.jngeo").string());
    CHECK(cookThrows(net, trail));
    std::filesystem::remove_all(dir);
}

namespace {

Geometry labelled(const std::vector<std::string>& texts, const std::vector<std::string>& tableOrder) {
    Geometry g;
    g.resizePoints(texts.size());
    AttribArray& label = g.attrib(AttribClass::point, "label", AttribType::string);
    for (const std::string& t : tableOrder) label.intern(t);
    for (size_t i = 0; i < texts.size(); ++i) label.ints()[i] = label.intern(texts[i]);
    for (size_t i = 0; i < texts.size(); ++i) g.x()[i] = static_cast<float>(i);
    return g;
}

std::vector<std::string> labelsOf(const Geometry& g) {
    std::vector<std::string> out;
    const AttribArray* a = g.findAttrib(AttribClass::point, "label");
    for (size_t i = 0; i < g.pointCount(); ++i) out.push_back(a->stringAt(i));
    return out;
}

}  // namespace

TEST(stringAttributeTablesCopyOnWriteAndStartWithTheEmptyString) {
    AttribArray a(AttribType::string);
    CHECK(a.table().strings.size() == 1 && a.table().strings[0].empty());
    CHECK(a.intern("") == 0);
    const int32_t tree = a.intern("tree");
    CHECK(tree == 1 && a.intern("tree") == tree);
    a.resize(3);
    a.ints()[0] = tree;

    AttribArray b = a;
    CHECK(b.tableHandle() == a.tableHandle());
    CHECK(b.intern("rock") == 2);
    CHECK(a.table().strings.size() == 2 && b.table().strings.size() == 3);
    CHECK(a.stringAt(0) == "tree" && a.stringAt(1).empty());
    a.ints()[2] = 9;
    bool threw = false;
    try {
        a.stringAt(2);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    Geometry g;
    g.resizePoints(3);
    g.attrib(AttribClass::point, "label", AttribType::string).ints()[1] = 5;
    threw = false;
    try {
        g.checkStrings();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(!g.hasAttrib(AttribClass::point, "label.x"));
}

TEST(stringAttributesKeepTheirTextWhenGeometriesAreMerged) {
    const Geometry a = labelled({"tree", "rock", "", "tree"}, {"tree", "rock"});
    const Geometry b = labelled({"rock", "bush", "tree", "bush", "moss"}, {"moss", "bush", "rock", "tree"});
    const Geometry same = labelled({"rock", "tree"}, {"tree", "rock"});
    Geometry plain;
    plain.resizePoints(2);

    ThreadPool pool(3);
    const Geometry merged = Geometry::concat({&a, &plain, &b, &a}, &pool);
    CHECK(merged.pointCount() == 4 + 2 + 5 + 4);
    merged.checkStrings();
    const std::vector<std::string> expected = {"tree", "rock", "", "tree", "", "", "rock", "bush", "tree", "bush", "moss", "tree", "rock", "", "tree"};
    CHECK(labelsOf(merged) == expected);
    CHECK(a.findAttrib(AttribClass::point, "label")->table().strings.size() == 3);

    const Geometry serial = Geometry::concat({&a, &plain, &b, &a}, nullptr);
    CHECK(sameGeometry(merged, serial));

    Geometry appended = a;
    appended.append(b);
    appended.append(plain);
    appended.checkStrings();
    CHECK(labelsOf(appended) == std::vector<std::string>({"tree", "rock", "", "tree", "rock", "bush", "tree", "bush", "moss", "", ""}));

    // Equal tables are shared, not rebuilt.
    const Geometry twice = Geometry::concat({&same, &same}, nullptr);
    CHECK(twice.findAttrib(AttribClass::point, "label")->tableHandle() == same.findAttrib(AttribClass::point, "label")->tableHandle());

    // The same text is the same geometry, whatever order the table is in.
    const Geometry reordered = labelled({"tree", "rock", "", "tree"}, {"rock", "tree"});
    CHECK(sameGeometry(a, reordered));
    CHECK(!sameGeometry(a, labelled({"tree", "rock", "", "rock"}, {"rock", "tree"})));
}

TEST(stringAttributesAreReadOnlyInKernels) {
    Geometry g = labelled({"tree", "rock", "tree"}, {"tree", "rock"});
    g.attrib(AttribClass::point, "mark");
    KernelCompiler compiler;

    auto kernel = compiler.compileFor(parseWrangle("mark = label * 10;", {}), g, AttribClass::point);
    kernel->run(g, {}, AttribClass::point, nullptr);
    CHECK(g.findAttrib(AttribClass::point, "mark")->at(0) == 10.0f);
    CHECK(g.findAttrib(AttribClass::point, "mark")->at(1) == 20.0f);
    CHECK(g.findAttrib(AttribClass::point, "mark")->at(2) == 10.0f);

    bool threw = false;
    try {
        compiler.compileFor(parseWrangle("label = 1;", {}), g, AttribClass::point);
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("cannot be assigned") != std::string::npos;
    }
    CHECK(threw);
}

// Every node type that cooks must leave string attributes consistent: all ids inside their table, and the text one of the input's.
TEST(everyNodeTypeKeepsStringAttributesConsistent) {
    const std::vector<std::string> words = {"tree", "rock", "bush", ""};
    Geometry input;
    input.resizePoints(24);
    AttribArray& label = input.attrib(AttribClass::point, "label", AttribType::string);
    label.intern("tree");
    label.intern("rock");
    label.intern("bush");
    for (size_t i = 0; i < 24; ++i) {
        input.x()[i] = static_cast<float>(i % 6) * 0.7f;
        input.y()[i] = static_cast<float>(i / 6) * 0.9f;
        label.ints()[i] = static_cast<int32_t>(i % 4);
    }
    for (uint32_t p = 0; p < 4; ++p) input.addPrimitive({p * 6, 6, p % 2 == 0, PrimKind::polyline});
    AttribArray& kind = input.attrib(AttribClass::primitive, "kind", AttribType::string);
    kind.ints()[0] = kind.intern("tree");
    kind.ints()[1] = kind.intern("rock");
    kind.ints()[2] = kind.intern("bush");
    input.checkStrings();

    size_t cooked = 0, kept = 0;
    for (const std::string& type : NodeRegistry::instance().typeNames()) {
        if (type == "subnetInput" || type == "subnet") continue;
        Network net(1);
        net.setExternalInputs({std::make_shared<Geometry>(input)});
        const NodeId in = net.addNode("subnetInput", "in");
        const NodeId node = net.addNode(type, "under");
        const int inputs = net.node(node).inputCount();  // a node with variable inputs grows one with each connection
        for (int i = 0; i < inputs; ++i) net.connect(in, node, i);
        GeometryHandle out;
        try {
            out = net.cook(node);
        } catch (const std::exception&) {
            continue;
        }
        ++cooked;
        out->checkStrings();
        for (AttribClass cls : {AttribClass::point, AttribClass::primitive}) {
            for (const auto& [name, array] : out->attribs(cls)) {
                if (array.type() != AttribType::string) continue;
                ++kept;
                for (size_t i = 0; i < array.size(); ++i) CHECK(std::find(words.begin(), words.end(), array.stringAt(i)) != words.end());
            }
        }
    }
    CHECK(cooked > 20);
    CHECK(kept > 5);
}

TEST(stringAttributesAreNotSavedUntilTheFormatsKnowThem) {
    const Geometry g = labelled({"tree"}, {});
    bool threw = false;
    try {
        saveGeometry(g);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(traceRecordsEveryCookedTaskOnceAndChangesNothing) {
    Branches plain(4, 4, 2000.0f), traced(4, 4, 2000.0f);
    traced.net.setTraceEnabled(true);
    CHECK(plain.net.traceEventCount() == 0);
    const GeometryHandle expected = plain.net.cook(plain.out);
    const GeometryHandle actual = traced.net.cook(traced.out);
    CHECK(sameGeometry(*expected, *actual));
    CHECK(plain.net.traceEventCount() == 0);

    size_t tasks = 0;
    for (NodeId id : traced.net.nodeIds()) {
        const NodeStats& st = traced.net.node(id).stats();
        if (st.cooks > 0 && st.fusedInto == invalidNodeId) ++tasks;
    }
    CHECK(tasks > 0);
    CHECK(traced.net.traceEventCount() == tasks);

    const std::string json = traced.net.traceJson();
    CHECK(json.find("{\"traceEvents\":[") == 0);
    size_t phases = 0;
    for (size_t at = json.find("\"ph\":\"X\""); at != std::string::npos; at = json.find("\"ph\":\"X\"", at + 1)) ++phases;
    CHECK(phases == tasks);
    for (NodeId id : traced.net.nodeIds()) {
        const NodeStats& st = traced.net.node(id).stats();
        if (st.cooks == 0 || st.fusedInto != invalidNodeId) continue;
        const std::string key = "\"name\":\"" + traced.net.node(id).name() + "\"";
        const size_t first = json.find(key);
        CHECK(first != std::string::npos);
        CHECK(json.find(key, first + 1) == std::string::npos);
    }

    const std::string path = (std::filesystem::temp_directory_path() / "jitnodes_test_trace.json").string();
    traced.net.writeTrace(path);
    {
        std::ifstream in(path, std::ios::binary);
        const std::string onDisk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(onDisk == json);
    }
    std::filesystem::remove(path);

    traced.net.clearTrace();
    CHECK(traced.net.traceEventCount() == 0);
}

TEST(fileCacheSkipsTheUpstreamCookWhenTheFileIsThere) {
    const std::string path = (std::filesystem::temp_directory_path() / "jitnodes_test_cache.jngeo").string();
    std::filesystem::remove(path);
    const auto build = [&](Network& net, NodeId& source, NodeId& cache, const char* mode) {
        source = net.addNode("generate");
        net.node(source).setFloat("count", 500.0f);
        cache = net.addNode("fileCache");
        net.node(cache).setString("path", path);
        net.node(cache).setString("mode", mode);
        net.connect(source, cache, 0);
    };

    Network first(2);
    NodeId source, cache;
    build(first, source, cache, "auto");
    auto written = first.cook(cache);
    CHECK(first.node(source).cookCount() == 1);
    CHECK(std::filesystem::exists(path));
    CHECK(sameGeometry(*written, *first.cook(source)));

    // A new session: the file satisfies the cache, so the generate node is never cooked.
    Network second(2);
    build(second, source, cache, "auto");
    auto loaded = second.cook(cache);
    CHECK(second.node(source).cookCount() == 0);
    CHECK(sameGeometry(*loaded, *written));

    // read mode works with nothing connected; write mode recooks and rewrites.
    Network third(2);
    const NodeId lone = third.addNode("fileCache");
    third.node(lone).setString("path", path);
    third.node(lone).setString("mode", "read");
    CHECK(sameGeometry(*third.cook(lone), *written));

    Network fourth(2);
    build(fourth, source, cache, "write");
    fourth.node(source).setFloat("count", 20.0f);
    CHECK(fourth.cook(cache)->pointCount() == 20);
    CHECK(loadGeometryFromFile(path).pointCount() == 20);

    // A corrupt file: auto cooks and replaces it, read reports it.
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "not a geometry file";
    }
    Network fifth(2);
    build(fifth, source, cache, "read");
    CHECK(cookThrows(fifth, cache));
    fifth.node(cache).setString("mode", "auto");
    CHECK(fifth.cook(cache)->pointCount() == 500);
    CHECK(fifth.node(source).cookCount() == 1);
    CHECK(loadGeometryFromFile(path).pointCount() == 500);

    fifth.node(cache).setString("mode", "sideways");
    CHECK(cookThrows(fifth, cache));
    std::filesystem::remove(path);
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
