#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "jitnodes/jitnodes.h"
#include "../src/graph/nodeUtil.h"

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

// One two-point primitive per entry; an empty key is an anonymous primitive.
static Geometry keyedLines(const std::vector<std::string>& keys) {
    Geometry g;
    g.resizePoints(keys.size() * 2);
    for (size_t i = 0; i < keys.size(); ++i) g.addShapePrimitive({static_cast<uint32_t>(i * 2), 2, false}, keys[i]);
    return g;
}

static std::vector<std::string> shapeKeysOf(const Geometry& g) {
    std::vector<std::string> keys;
    for (const ShapeRange& range : g.shapes()) keys.push_back(std::string(range.key));
    return keys;
}

static bool checkShapesThrows(const Geometry& g) {
    try {
        g.checkShapes();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

TEST(geometryWithoutKeysHasOneShapePerPrimitive) {
    Geometry g = keyedLines({"", "", ""});
    CHECK(!g.hasShapeKeys());   // nothing is allocated until something is keyed
    const std::vector<ShapeRange> shapes = g.shapes();
    CHECK(shapes.size() == 3);
    for (uint32_t i = 0; i < 3; ++i) CHECK(shapes[i].first == i && shapes[i].count == 1 && shapes[i].key.empty());
    CHECK(g.shapeKey(1).empty() && g.shapeKey(99).empty());
    CHECK(Geometry().shapes().empty());
}

TEST(shapesAreRunsOfEqualKeys) {
    const Geometry g = keyedLines({"a", "a", "b", "", "c", "c"});
    const std::vector<ShapeRange> shapes = g.shapes();
    CHECK(shapes.size() == 4);
    CHECK(shapes[0].first == 0 && shapes[0].count == 2 && shapes[0].key == "a");
    CHECK(shapes[1].first == 2 && shapes[1].count == 1 && shapes[1].key == "b");
    CHECK(shapes[2].first == 3 && shapes[2].count == 1 && shapes[2].key.empty());
    CHECK(shapes[3].first == 4 && shapes[3].count == 2 && shapes[3].key == "c");
    CHECK((g.shapeOfPrimitive() == std::vector<uint32_t>{0, 0, 1, 2, 3, 3}));
    CHECK(g.shapeKey(1) == "a" && g.shapeKey(3).empty());
    const ShapeRange* c = Geometry::findShape(shapes, "c");
    CHECK(c != nullptr && c->first == 4);
    CHECK(Geometry::findShape(shapes, "") == nullptr && Geometry::findShape(shapes, "zz") == nullptr);
    g.checkShapes();
}

TEST(setShapeKeyChangesAndClearsAKey) {
    Geometry g = keyedLines({"", ""});
    g.setShapeKey(0, "");
    CHECK(!g.hasShapeKeys());   // clearing a key that was never set creates nothing
    g.setShapeKey(0, "x");
    g.setShapeKey(1, "x");
    CHECK(shapeKeysOf(g) == std::vector<std::string>{"x"});
    g.setShapeKey(1, "");
    CHECK((shapeKeysOf(g) == std::vector<std::string>{"x", ""}));
    bool threw = false;
    try {
        g.setShapeKey(2, "y");
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(editingACopyOfAGeometryLeavesTheOriginalKeys) {
    const Geometry a = keyedLines({"a", "b"});
    Geometry b = a;
    b.setShapeKey(0, "z");
    b.setShapeKey(1, "y");
    CHECK((shapeKeysOf(a) == std::vector<std::string>{"a", "b"}));
    CHECK((shapeKeysOf(b) == std::vector<std::string>{"z", "y"}));
}

TEST(appendAndConcatCarryKeysAndKeepAnonymousParts) {
    const Geometry a = keyedLines({"x", "x", "y"});
    const Geometry none = keyedLines({""});
    const Geometry c = keyedLines({"w", "z"});   // its table holds w before z, so the ids differ from a's
    const Geometry all = Geometry::concat({&a, &none, &c});
    CHECK((shapeKeysOf(all) == std::vector<std::string>{"x", "y", "", "w", "z"}));
    CHECK(all.shapes()[0].count == 2);
    all.checkShapes();

    Geometry appended = a;
    appended.append(none);
    appended.append(c);
    CHECK((shapeKeysOf(appended) == shapeKeysOf(all)));

    Geometry plain = keyedLines({""});   // a part without the attribute first: it zero fills
    plain.append(a);
    CHECK((shapeKeysOf(plain) == std::vector<std::string>{"", "x", "y"}));
}

TEST(keysFollowThePrimitivesThroughGatherOperations) {
    const Geometry g = keyedLines({"a", "a", "b", "c"});
    const Geometry picked = extractPrimitives(g, {3, 0});
    CHECK((shapeKeysOf(picked) == std::vector<std::string>{"c", "a"}));
    const Geometry both = extractPrimitives(g, {0, 1});
    CHECK(both.shapes().size() == 1 && both.shapes()[0].count == 2 && both.shapes()[0].key == "a");

    Geometry marked = g;
    marked.createGroup(AttribClass::primitive, "drop").ints()[2] = 1;   // drops "b"
    const Geometry kept = blast(marked, AttribClass::primitive, "drop", false);
    CHECK((shapeKeysOf(kept) == std::vector<std::string>{"a", "c"}));
    CHECK(kept.shapes()[0].count == 2);
}

TEST(flatteningCurvesKeepsTheShapeKeys) {
    Geometry g;
    g.resizePoints(8);
    for (size_t i = 0; i < 8; ++i) {
        g.x()[i] = static_cast<float>(i);
        g.y()[i] = static_cast<float>(i % 3);
    }
    g.addShapePrimitive({0, 4, false, PrimKind::cubic}, "k");
    g.addShapePrimitive({4, 4, false, PrimKind::cubic}, "k");   // a compound shape of two curves
    const Geometry flat = flattenCurves(g);
    CHECK(!hasCurves(flat));
    CHECK(flat.primitiveCount() == 2);
    CHECK(flat.shapes().size() == 1 && flat.shapes()[0].key == "k" && flat.shapes()[0].count == 2);
}

TEST(checkShapesFindsAKeyThatComesBack) {
    CHECK(!checkShapesThrows(keyedLines({"a", "a", "b"})));
    CHECK(!checkShapesThrows(keyedLines({"a", "", "b", ""})));
    CHECK(checkShapesThrows(keyedLines({"a", "b", "a"})));
    CHECK(!checkShapesThrows(Geometry()));
}

TEST(shapeKeysSurviveTheGeometryFile) {
    const Geometry g = keyedLines({"p/1", "p/1", "q", "", "r"});
    const std::vector<uint8_t> bytes = saveGeometry(g);
    const Geometry back = loadGeometry(bytes.data(), bytes.size());
    CHECK((shapeKeysOf(back) == shapeKeysOf(g)));
    CHECK(back.shapes()[0].count == 2);
    CHECK(sameGeometry(g, back));
}

TEST(parserRejectsBadInput) {
    bool threw = false;
    try { parseWrangle("x = (1 + ;", {}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { parseWrangle("a = 1;", {"a"}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

TEST(parserSkipsHashComments) {
    KernelSpec plain = parseWrangle("x = x * 2; y = y + 1;", {});
    KernelSpec commented = parseWrangle("# double x\nx = x * 2; # trailing\n  # a line of its own\ny = y + 1;\n# last line, no newline", {});
    CHECK(commented.graph.size() == plain.graph.size());
    CHECK(commented.stores.size() == plain.stores.size());
    CHECK(parseWrangle("# only a comment", {}).stores.empty());

    bool threw = false;
    std::string message;
    try { parseWrangle("x = 1; # fine\ny = (1 + ;", {}); } catch (const std::runtime_error& e) { threw = true; message = e.what(); }
    CHECK(threw);
    CHECK(message.find("offset 23") != std::string::npos);   // offsets still count the comment's characters
}

TEST(parserReadsParamDeclarations) {
    std::vector<WrangleParam> declared;
    KernelSpec spec = parseWrangle("# tweakables\n| gain = 1.5, radius = 2(-1, 10), count(1,), ratio(, 1), plain |\nlabel = x;\nx = x * gain + radius + a;", {"a"}, {}, &declared);
    CHECK(declared.size() == 5 && spec.paramCount == 6);
    CHECK(declared[0].name == "gain" && declared[0].value == 1.5f && !declared[0].hasMinimum && !declared[0].hasMaximum && declared[0].slot == 1);
    CHECK(declared[1].name == "radius" && declared[1].value == 2.0f && declared[1].hasMinimum && declared[1].minimum == -1.0f &&
          declared[1].hasMaximum && declared[1].maximum == 10.0f && declared[1].slot == 2);
    CHECK(declared[2].name == "count" && declared[2].value == 0.0f && declared[2].hasMinimum && declared[2].minimum == 1.0f && !declared[2].hasMaximum);
    CHECK(declared[3].name == "ratio" && !declared[3].hasMinimum && declared[3].hasMaximum && declared[3].maximum == 1.0f);
    CHECK(declared[4].name == "plain" && declared[4].value == 0.0f && !declared[4].hasMinimum && !declared[4].hasMaximum && declared[4].slot == 5);

    // several lists may stand at the top; empty parentheses set no limit
    parseWrangle("| p |\n# next\n| q(,) |\nx = p + q;", {}, {}, &declared);
    CHECK(declared.size() == 2 && !declared[1].hasMinimum && !declared[1].hasMaximum);

    // without the out-parameter a bar is not part of the language, but || still is
    bool threw = false;
    try { parseWrangle("| g |\nx = g;", {}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    CHECK(parseWrangle("x = a || b;", {}, {}, &declared).stores.size() == 1 && declared.empty());

    // a string literal takes a slot after the declared ones
    parseWrangle("| p |\nok = label == \"a\";", {}, {}, &declared);
    CHECK(declared.size() == 1 && declared[0].slot == 0);
}

TEST(parserRejectsBadParamDeclarations) {
    for (const char* code : {"| g |\ng = 2;", "| a |", "| time |", "| g, g |", "x = 1;\n| g |", "| |", "| g = |", "| g = x |", "| _t |", "| n.x |", "| g",
                             "| g(1 |", "| g(5, 2) |", "| g(1) |", "| g, |", "| g = 1 2 |", "|| g |"}) {
        std::vector<WrangleParam> declared;
        bool threw = false;
        try { parseWrangle(code, {"a"}, {}, &declared); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
    }
}

TEST(wrangleDeclaredParametersAreNodeParameters) {
    Network net;
    const NodeId line = net.addNode("line", "ln");
    const NodeId wr = net.addNode("wrangle", "wr");
    net.node(line).setFloat("count", 5.0f);
    net.node(line).setFloat("length", 4.0f);
    net.connect(line, wr);
    net.setDisplayNode(wr);
    Node& node = net.node(wr);
    node.setString("code", "| gain = 2(0, 10) |\nx = x * gain;");
    CHECK(node.hasFloat("gain") && node.isSpare("gain") && node.getFloat("gain") == 2.0f);
    float limit = -1.0f;
    CHECK(node.floatMinimum("gain", limit) && limit == 0.0f);
    CHECK(node.floatMaximum("gain", limit) && limit == 10.0f);
    CHECK(!node.floatMinimum("a", limit) && !node.floatMaximum("nothing", limit));
    GeometryHandle g = net.cookDisplay();
    CHECK(g->findAttrib(AttribClass::point, "x")->at(2) == 4.0f);

    node.setFloat("gain", 3.0f);
    g = net.cookDisplay();
    CHECK(g->findAttrib(AttribClass::point, "x")->at(2) == 6.0f);

    // editing the code keeps the value, an expression survives, a new declaration appears, a dropped one goes
    node.setExpression("gain", "time + 1");
    node.setString("code", "| gain = 9, extra(1,) |\n# comment\nx = x * gain + extra;");
    CHECK(node.hasFloat("extra") && node.expression("gain") == "time + 1" && node.baseFloat("gain") == 3.0f);
    CHECK(!node.floatMinimum("gain", limit) && !node.floatMaximum("gain", limit));   // the limits follow the code
    CHECK(node.floatMinimum("extra", limit) && limit == 1.0f && !node.floatMaximum("extra", limit));
    node.setString("code", "x = x * a;");
    CHECK(!node.hasFloat("gain") && !node.hasFloat("extra"));

    // a parse error leaves the parameters alone; the cook reports it
    node.setString("code", "| keep = 4 | x = x * keep;");
    node.setString("code", "| keep = 4 | x = x * (keep;");
    CHECK(node.hasFloat("keep"));
    bool threw = false;
    try { net.cookDisplay(); } catch (const std::exception&) { threw = true; }
    CHECK(threw);

    // a user-added spare is never removed by code edits
    node.addSpareFloat("mine", 1.0f);
    node.setString("code", "x = x;");
    CHECK(node.hasFloat("mine") && !node.hasFloat("keep"));
}

TEST(wrangleDeclaredParametersSurviveSaveAndLoad) {
    Network net;
    const NodeId wr = net.addNode("wrangle", "wr");
    net.node(wr).setString("code", "| gain = 2(0, 10) |\nx = x * gain;");
    net.node(wr).setFloat("gain", 7.0f);
    const std::string text = saveNetwork(net);
    Network loaded;
    loadNetwork(loaded, text);
    Node& node = loaded.node(loaded.findNode("wr"));
    float limit = 0.0f;
    CHECK(node.hasFloat("gain") && node.baseFloat("gain") == 7.0f);
    CHECK(node.floatMaximum("gain", limit) && limit == 10.0f);
    node.setString("code", "x = x;");
    CHECK(!node.hasFloat("gain"));
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

TEST(sampleIndexCountsElementsOnEveryIsa) {
    KernelCompiler probe;
    for (Isa isa : probe.supportedIsas()) {
        KernelCompiler compiler;
        compiler.setIsa(isa);
        Geometry g = makeRamp(37);
        compiler.compile(parseWrangle("x = sampleIndex * k + s;", {"s"}, {{"k", 0.5f}}))->run(g, {1.0f});
        for (size_t i = 0; i < 37; ++i) CHECK_NEAR(g.x()[i], static_cast<float>(i) * 0.5f + 1.0f);
    }
}

TEST(sampleIndexJitMatchesInterpreter) {
    expectJitMatchesInterpreter("x = sin(sampleIndex * 0.1) * s; y = sampleIndex > 20 ? y : -y;", {"s"}, {3.0f});
}

namespace {

// line(count = block) -> wrangle osc (a = phase, b = phase step) -> wrangle gain (a = gain), as the editor would build it.
struct OscGain {
    Network net;
    NodeId osc, gain;
    explicit OscGain(size_t block) {
        NodeId line = net.addNode("line");
        net.node(line).setFloat("count", static_cast<float>(block));
        osc = net.addNode("wrangle");
        net.node(osc).setString("code", "x = sin(6.28318530718 * (a + sampleIndex * b));");
        gain = net.addNode("wrangle");
        net.node(gain).setString("code", "x = x * a;");
        net.connect(line, osc);
        net.connect(osc, gain);
    }
};

}  // namespace

TEST(audioProgramMatchesInterpreterAcrossBlocks) {
    constexpr size_t block = 64;
    OscGain g(block);
    auto program = AudioProgram::compile(g.net, g.gain, {block, {"x"}});
    program->warm();

    OscGain ref(block);
    ref.net.compiler().setJitEnabled(false);

    const float step = 440.0f / 48000.0f;
    float phase = 0.0f;
    std::vector<float> out(block);
    for (int b = 0; b < 8; ++b) {
        program->param(g.osc, 0) = phase;
        program->param(g.osc, 1) = step;
        program->param(g.gain, 0) = 0.25f;
        float* channels[] = {out.data()};
        program->process(channels);

        ref.net.node(ref.osc).setFloat("a", phase);
        ref.net.node(ref.osc).setFloat("b", step);
        ref.net.node(ref.gain).setFloat("a", 0.25f);
        const auto expected = ref.net.cook(ref.gain);
        for (size_t i = 0; i < block; ++i) CHECK(std::fabs(out[i] - expected->x()[i]) < 1e-4f);
        phase = std::fmod(phase + step * static_cast<float>(block), 1.0f);
    }
    CHECK(std::fabs(out[block - 1]) > 0.0f);
}

TEST(audioProgramRejectsBadGraphs) {
    OscGain g(48);
    bool threw = false;
    try { AudioProgram::compile(g.net, g.gain, {48, {"x"}}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(!threw);  // 48 is a multiple of 16
    threw = false;
    try { AudioProgram::compile(g.net, g.gain, {50, {"x"}}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { AudioProgram::compile(g.net, g.gain, {64, {"x"}}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);  // the source has 48 points
    threw = false;
    try { AudioProgram::compile(g.net, g.gain, {48, {"nope"}}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

namespace {

// Osc nodes read from `line`s of `block` points: oscillators 0 and 1 share the first line, 2 has its own. mixAB = osc0 * g0 + osc1,
// outer = mixAB + osc2, master = outer * gain.
struct MixGraph {
    Network net;
    NodeId osc[3], gain0, mixAB, outer, master;

    explicit MixGraph(size_t block) {
        NodeId lines[2];
        for (NodeId& line : lines) {
            line = net.addNode("line");
            net.node(line).setFloat("count", static_cast<float>(block));
        }
        for (int i = 0; i < 3; ++i) {
            osc[i] = net.addNode("wrangle");
            net.node(osc[i]).setString("code", "x = sin(6.28318530718 * (a + sampleIndex * b));");
            net.connect(lines[i == 2 ? 1 : 0], osc[i]);
        }
        gain0 = net.addNode("wrangle");
        net.node(gain0).setString("code", "x = x * a;");
        net.connect(osc[0], gain0);
        mixAB = net.addNode("mix");
        net.connect(gain0, mixAB, 0);
        net.connect(osc[1], mixAB, 1);
        outer = net.addNode("mix");
        net.connect(mixAB, outer, 0);
        net.connect(osc[2], outer, 1);
        master = net.addNode("wrangle");
        net.node(master).setString("code", "x = x * a;");
        net.connect(outer, master);
    }
};

}  // namespace

TEST(audioProgramMixSumsBranchesLikeTheInterpreter) {
    constexpr size_t block = 64;
    MixGraph g(block);
    auto program = AudioProgram::compile(g.net, g.master, {block, {"x"}});
    program->warm();

    MixGraph ref(block);
    ref.net.compiler().setJitEnabled(false);

    const float steps[3] = {220.0f / 48000.0f, 330.0f / 48000.0f, 550.0f / 48000.0f};
    float phase[3] = {0.0f, 0.1f, 0.2f};
    std::vector<float> out(block);
    for (int b = 0; b < 6; ++b) {
        for (int i = 0; i < 3; ++i) {
            for (auto set : {std::pair<MixGraph*, bool>{&g, true}, std::pair<MixGraph*, bool>{&ref, false}}) {
                MixGraph& m = *set.first;
                if (set.second) {
                    program->param(m.osc[i], 0) = phase[i];
                    program->param(m.osc[i], 1) = steps[i];
                } else {
                    m.net.node(m.osc[i]).setFloat("a", phase[i]);
                    m.net.node(m.osc[i]).setFloat("b", steps[i]);
                }
            }
            phase[i] = std::fmod(phase[i] + steps[i] * static_cast<float>(block), 1.0f);
        }
        program->param(g.gain0, 0) = 0.5f;
        program->param(g.master, 0) = 0.25f;
        ref.net.node(ref.gain0).setFloat("a", 0.5f);
        ref.net.node(ref.master).setFloat("a", 0.25f);

        float* channels[] = {out.data()};
        program->process(channels);
        const auto expected = ref.net.cook(ref.master);
        for (size_t i = 0; i < block; ++i) CHECK(std::fabs(out[i] - expected->x()[i]) < 1e-4f);
    }
    CHECK(std::fabs(out[block - 1]) > 0.0f);
}

TEST(audioProgramMixRejectsSharedNodesAndEmptyMixes) {
    MixGraph g(32);
    Network& net = g.net;
    bool threw = false;
    NodeId shared = net.addNode("mix");
    net.connect(g.osc[2], shared, 0);
    net.connect(g.osc[2], shared, 1);
    try { AudioProgram::compile(net, shared, {32, {"x"}}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);  // one oscillator on both inputs would leave its parameters ambiguous
    threw = false;
    NodeId empty = net.addNode("mix");
    try { AudioProgram::compile(net, empty, {32, {"x"}}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

namespace {

// line(count) -> osc (a = phase, b = step) -> delay -> biquad -> fir -> gain, with `count` either one block or the whole signal.
struct StepChain {
    Network net;
    NodeId osc, delay, biquad, fir, gain;

    explicit StepChain(size_t count) {
        NodeId line = net.addNode("line");
        net.node(line).setFloat("count", static_cast<float>(count));
        osc = net.addNode("wrangle");
        net.node(osc).setString("code", "x = sin(6.28318530718 * (a + sampleIndex * b));");
        net.connect(line, osc);
        delay = net.addNode("delay");
        net.node(delay).setFloat("samples", 37.0f);
        net.node(delay).setFloat("maxSamples", 128.0f);
        net.connect(osc, delay);
        biquad = net.addNode("biquad");
        const float coeffs[5] = {0.2f, 0.4f, 0.2f, -0.3f, 0.1f};
        const char* names[5] = {"b0", "b1", "b2", "a1", "a2"};
        for (int k = 0; k < 5; ++k) net.node(biquad).setFloat(names[k], coeffs[k]);
        net.connect(delay, biquad);
        fir = net.addNode("fir");
        net.node(fir).setString("coeffs", "0.5, 0.25, 0.125, -0.0625");
        net.connect(biquad, fir);
        gain = net.addNode("wrangle");
        net.node(gain).setString("code", "x = x * a;");
        net.connect(fir, gain);
    }
};

}  // namespace

TEST(audioProgramStepsMatchTheInterpreterAcrossBlocks) {
    constexpr size_t block = 64, blocks = 10;
    StepChain g(block);
    auto program = AudioProgram::compile(g.net, g.gain, {block, {"x"}});
    const float step = 440.0f / 48000.0f;
    program->param(g.osc, 1) = step;
    program->param(g.delay, 0) = 37.0f;
    const float coeffs[5] = {0.2f, 0.4f, 0.2f, -0.3f, 0.1f};
    for (size_t k = 0; k < 5; ++k) program->param(g.biquad, k) = coeffs[k];
    program->param(g.gain, 0) = 0.5f;
    program->addPhase(g.osc, 0, 1);
    program->warm();
    program->resetState();

    StepChain ref(block * blocks);
    ref.net.node(ref.osc).setFloat("b", step);
    ref.net.node(ref.gain).setFloat("a", 0.5f);
    const auto expected = ref.net.cook(ref.gain);

    std::vector<float> out(block);
    for (size_t b = 0; b < blocks; ++b) {
        float* channels[] = {out.data()};
        program->process(channels);
        for (size_t i = 0; i < block; ++i) CHECK(std::fabs(out[i] - expected->x()[b * block + i]) < 2e-3f);
    }
    CHECK(std::fabs(out[block - 1]) > 0.0f);
}

TEST(audioProgramFeedbackEchoesWithOneBlockOfLatency) {
    constexpr size_t block = 32, blocks = 8;
    constexpr float feedback = 0.5f;
    Network net;
    NodeId line = net.addNode("line");
    net.node(line).setFloat("count", static_cast<float>(block));
    net.node(line).setFloat("length", static_cast<float>(block - 1));  // x[i] = i
    NodeId source = net.addNode("fbSource");
    net.node(source).setString("name", "echo");
    NodeId tail = net.addNode("wrangle");
    net.node(tail).setString("code", "x = x * a;");
    net.connect(source, tail);
    NodeId mix = net.addNode("mix");
    net.connect(line, mix, 0);
    net.connect(tail, mix, 1);
    NodeId sink = net.addNode("fbSink");
    net.node(sink).setString("name", "echo");
    net.connect(mix, sink);
    NodeId master = net.addNode("wrangle");
    net.node(master).setString("code", "x = x * a;");
    net.connect(sink, master);

    auto program = AudioProgram::compile(net, master, {block, {"x"}});
    program->param(tail, 0) = feedback;
    program->param(master, 0) = 1.0f;
    std::vector<float> out(block), previous(block, 0.0f);
    for (size_t b = 0; b < blocks; ++b) {
        float* channels[] = {out.data()};
        program->process(channels);
        for (size_t i = 0; i < block; ++i) CHECK_NEAR(out[i], static_cast<float>(i) + feedback * previous[i]);
        previous = out;
    }
}

TEST(audioProgramMigratedStateContinuesInARecompiledProgram) {
    constexpr size_t block = 64;
    const float step = 440.0f / 48000.0f;
    auto build = [&](StepChain& g) {
        auto program = AudioProgram::compile(g.net, g.gain, {block, {"x"}});
        program->param(g.osc, 1) = step;
        program->param(g.delay, 0) = 37.0f;
        const float coeffs[5] = {0.2f, 0.4f, 0.2f, -0.3f, 0.1f};
        for (size_t k = 0; k < 5; ++k) program->param(g.biquad, k) = coeffs[k];
        program->param(g.gain, 0) = 0.5f;
        program->addPhase(g.osc, 0, 1);
        return program;
    };
    StepChain a(block), b(block);
    auto first = build(a);
    std::vector<float> out(block), whole;
    for (int n = 0; n < 4; ++n) {
        float* channels[] = {out.data()};
        first->process(channels);
        whole.insert(whole.end(), out.begin(), out.end());
    }
    auto second = build(b);
    second->migrateState(*first);
    for (int n = 0; n < 4; ++n) {
        float* channels[] = {out.data()};
        second->process(channels);
        whole.insert(whole.end(), out.begin(), out.end());
    }

    StepChain ref(block * 8);
    ref.net.node(ref.osc).setFloat("b", step);
    ref.net.node(ref.gain).setFloat("a", 0.5f);
    const auto expected = ref.net.cook(ref.gain);
    for (size_t i = 0; i < whole.size(); ++i) CHECK(std::fabs(whole[i] - expected->x()[i]) < 2e-3f);
}

TEST(audioProgramStepsRejectBadGraphsAndCoefficients) {
    StepChain g(32);
    bool threw = false;
    try { AudioProgram::compile(g.net, g.delay, {32, {"x"}}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);  // a step cannot be the output node
    g.net.node(g.fir).setString("coeffs", "1,abc");
    threw = false;
    try { AudioProgram::compile(g.net, g.gain, {32, {"x"}}); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
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

namespace {
class ExtraStateNode : public Node {
public:
    ExtraStateNode() : Node("testExtraState", 0) {}
    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>&) override { return shape_ ? shape_ : std::make_shared<Geometry>(); }

    void set(double gain, std::string label, GeometryHandle shape) {
        gain_ = gain;
        label_ = std::move(label);
        shape_ = std::move(shape);
        savedStateChanged();
    }
    double gain() const { return gain_; }
    const std::string& label() const { return label_; }
    const GeometryHandle& shape() const { return shape_; }

    void save(NodeWriter& out) const override {
        Node::save(out);
        out.number("gain", gain_);
        out.text("label", label_);
        out.geometry("shape", shape_);
    }
    void load(NodeReader& in) override {
        Node::load(in);
        gain_ = 1.0;
        label_.clear();
        shape_ = nullptr;
        in.number("gain", gain_);
        in.text("label", label_);
        in.geometry("shape", shape_);
    }

private:
    double gain_ = 1.0;
    std::string label_;
    GeometryHandle shape_;
};

ExtraStateNode& extraNode(Network& net, NodeId id) { return static_cast<ExtraStateNode&>(net.node(id)); }
}  // namespace

TEST(aNodeSavesAndLoadsStateOfItsOwn) {
    static const bool registered = (NodeRegistry::instance().registerType("testExtraState", [] { return std::make_unique<ExtraStateNode>(); }), true);
    (void)registered;

    Network net;
    const NodeId id = net.addNode("testExtraState");
    net.node(id).setPosition(4.0f, 5.0f);
    extraNode(net, id).set(2.5, "layer one", authoredCurve(0.0f));

    Network loaded;
    loadNetwork(loaded, saveNetwork(net));
    ExtraStateNode& copy = extraNode(loaded, id);
    CHECK(copy.gain() == 2.5 && copy.label() == "layer one");
    CHECK(sameGeometry(*copy.shape(), *authoredCurve(0.0f)));
    CHECK(copy.posX() == 4.0f && copy.posY() == 5.0f);

    // a snapshot sees the change, and restoring it brings the old state back
    GraphSnapshot before = snapshotNetwork(net);
    extraNode(net, id).set(7.0, "other", nullptr);
    GraphSnapshot after = snapshotNetwork(net, &before);
    CHECK(!sameGraph(before, after));
    restoreSnapshot(net, before, &after);
    CHECK(extraNode(net, id).gain() == 2.5 && extraNode(net, id).label() == "layer one" && extraNode(net, id).shape());

    // an unchanged node reuses its snapshot text
    CHECK(sameGraph(snapshotNetwork(net, &before), before));

    const std::vector<NodeId> pasted = pasteNodes(net, clipsFromText(clipsToText({copyNode(net, id)})));
    CHECK(pasted.size() == 1);
    CHECK(extraNode(net, pasted[0]).gain() == 2.5 && extraNode(net, pasted[0]).label() == "layer one");

    // a node left at its defaults loads back at them
    Network plain;
    const NodeId bare = plain.addNode("testExtraState");
    Network again;
    loadNetwork(again, saveNetwork(plain));
    CHECK(extraNode(again, bare).gain() == 1.0 && extraNode(again, bare).label().empty() && !extraNode(again, bare).shape());
}

TEST(aNodeCloneHasTheSavedStateOfItsOriginalAndNothingElse) {
    Network net;
    const NodeId line = net.addNode("line", "line");
    const NodeId xf = net.addNode("transform", "mover");
    net.connect(line, xf);
    Node& original = net.node(xf);
    original.setFloat("px", 3.0f);
    original.setExpression("rot", "line.length * 2");
    original.setKeyframes("sx", {{0.0, 1.0f, KeyInterp::linear}, {2.0, 3.0f, KeyInterp::linear}});
    original.setPosition(7.0f, 8.0f);
    original.setBypassed(true);
    original.addTag("layer");
    original.addParamTag("px", "unit:0.5");
    original.addSpareFloat("extra", 4.0f);
    original.setFrozenGeometry(authoredCurve(0.0f));
    net.cook(xf);

    std::unique_ptr<Node> copy = original.clone();
    CHECK(copy->typeName() == "transform" && copy->name() == "mover" && copy->inputCount() == original.inputCount());
    CHECK(copy->uid() == original.uid() && copy->id() == invalidNodeId);
    CHECK(copy->baseFloat("px") == 3.0f && copy->expression("rot") == "line.length * 2");
    CHECK(copy->keyframes("sx").size() == 2 && copy->keyframes("sx")[1].value == 3.0f);
    CHECK(copy->hasPosition() && copy->posX() == 7.0f && copy->posY() == 8.0f);
    CHECK(copy->bypassed() && copy->hasTag("layer") && copy->hasParamTag("px", "unit:0.5"));
    CHECK(copy->isSpare("extra") && copy->baseFloat("extra") == 4.0f);
    CHECK(copy->frozenGeometry() == original.frozenGeometry());
    CHECK(copy->dirty() && copy->cookCount() == 0 && copy->revision() != original.revision());

    CHECK(copy->hasSameStateAs(original) && original.hasSameStateAs(*copy));
    CHECK(!copy->hasSameStateAs(net.node(line)));
    {
        std::unique_ptr<Node> changed = original.clone();
        changed->setFloat("px", 3.5f);
        CHECK(!changed->hasSameStateAs(original));
        changed = original.clone();
        changed->setKeyframes("sx", {{0.0, 1.0f, KeyInterp::linear}, {2.0, 4.0f, KeyInterp::linear}});
        CHECK(!changed->hasSameStateAs(original));
        changed = original.clone();
        changed->setExpression("rot", "line.length * 3");
        CHECK(!changed->hasSameStateAs(original));
        changed = original.clone();
        changed->setPosition(7.0f, 9.0f);
        CHECK(!changed->hasSameStateAs(original));
        changed = original.clone();
        changed->setBypassed(false);
        CHECK(!changed->hasSameStateAs(original));
        changed = original.clone();
        changed->addParamTag("px", "extra");
        CHECK(!changed->hasSameStateAs(original));
        changed = original.clone();
        changed->setFrozenGeometry(authoredCurve(1.0f));
        CHECK(!changed->hasSameStateAs(original));
        changed->setFrozenGeometry(std::make_shared<Geometry>(*original.frozenGeometry()));
        CHECK(changed->hasSameStateAs(original) && changed->frozenGeometry() != original.frozenGeometry());
    }

    copy->setFloat("px", 9.0f);
    copy->setExpression("rot", "");
    copy->addTag("other");
    CHECK(original.baseFloat("px") == 3.0f && original.expression("rot") == "line.length * 2" && !original.hasTag("other"));

    // the copy's expression was compiled with the original's: it works once the copy is put into a network
    Network other;
    const NodeId otherLine = other.addNode("line", "line");
    const NodeId target = other.addNode("transform");
    other.connect(otherLine, target);
    other.node(target).copyStateFrom(original);
    CHECK(other.node(target).expression("rot") == "line.length * 2" && other.node(target).uid() == original.uid());
    CHECK(other.node(target).name() == "transform" + std::to_string(target));
    other.cook(target);
    CHECK(other.node(target).dirty() == false);
    other.node(target).copyStateFrom(*copy);
    CHECK(other.node(target).dirty() && other.node(target).baseFloat("px") == 9.0f && other.node(target).expression("rot").empty());

    bool refused = false;
    try {
        other.node(otherLine).copyStateFrom(original);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);
}

TEST(aWrangleCloneHasTheParametersAndLimitsItsCodeDeclares) {
    Network net;
    const NodeId id = net.addNode("wrangle");
    net.node(id).setString("code", "| gain = 2(0, 10) |\ny = y * gain;");
    net.node(id).setFloat("gain", 7.0f);
    float low = -1.0f, high = -1.0f;
    CHECK(net.node(id).floatMinimum("gain", low) && net.node(id).floatMaximum("gain", high));

    std::unique_ptr<Node> copy = net.node(id).clone();
    CHECK(copy->hasFloat("gain") && copy->isSpare("gain") && copy->baseFloat("gain") == 7.0f);
    float copyLow = -1.0f, copyHigh = -1.0f;
    CHECK(copy->floatMinimum("gain", copyLow) && copy->floatMaximum("gain", copyHigh) && copyLow == low && copyHigh == high);
    CHECK(copy->hasSameStateAs(net.node(id)));

    const NodeId plain = net.addNode("wrangle");
    net.node(plain).copyStateFrom(*copy);
    CHECK(net.node(plain).floatMaximum("gain", copyHigh) && copyHigh == high && net.node(plain).baseFloat("gain") == 7.0f);
}

TEST(aNodeCloneCarriesTheFieldsOfANodeTypeThroughSaveAndLoad) {
    static const bool registered = (NodeRegistry::instance().registerType("testExtraState", [] { return std::make_unique<ExtraStateNode>(); }), true);
    (void)registered;
    Network net;
    const NodeId id = net.addNode("testExtraState");
    const GeometryHandle shape = authoredCurve(0.0f);
    extraNode(net, id).set(2.5, "layer one", shape);

    std::unique_ptr<Node> copy = net.node(id).clone();
    const ExtraStateNode& typed = static_cast<const ExtraStateNode&>(*copy);
    CHECK(typed.gain() == 2.5 && typed.label() == "layer one" && typed.shape() == shape);
    CHECK(copy->hasSameStateAs(net.node(id)));
    extraNode(net, id).set(2.5, "layer two", shape);
    CHECK(!copy->hasSameStateAs(net.node(id)));
    extraNode(net, id).set(2.5, "layer one", std::make_shared<Geometry>(*shape));
    CHECK(copy->hasSameStateAs(net.node(id)));

    extraNode(net, id).set(7.0, "", nullptr);
    net.node(id).copyStateFrom(*copy);
    CHECK(extraNode(net, id).gain() == 2.5 && extraNode(net, id).label() == "layer one" && extraNode(net, id).shape() == shape);
}

TEST(transformRotatesSkewsAndKeepsItsPivotFixed) {
    Network net;
    NodeId line = net.addNode("line");
    NodeId xf = net.addNode("transform");
    net.node(line).setFloat("count", 2.0f);
    net.node(line).setFloat("length", 2.0f);        // points (0,0) and (2,0)
    net.connect(line, xf);

    net.node(xf).setFloat("rot", 90.0f);
    net.node(xf).setFloat("px", 1.0f);
    auto geo = net.cook(xf);
    CHECK_NEAR(geo->x()[0], 1.0f);                  // (0,0) about (1,0): (-1,0) -> (0,-1)
    CHECK_NEAR(geo->y()[0], -1.0f);
    CHECK_NEAR(geo->x()[1], 1.0f);
    CHECK_NEAR(geo->y()[1], 1.0f);

    net.node(xf).setFloat("rot", 0.0f);
    net.node(xf).setFloat("px", 0.0f);
    net.node(xf).setFloat("skew", 45.0f);
    net.node(xf).setFloat("tx", 5.0f);
    geo = net.cook(xf);
    CHECK_NEAR(geo->x()[1], 7.0f);                  // y is 0, so a skew leaves x alone
    CHECK_NEAR(geo->y()[1], 0.0f);
}

TEST(anEmptyMergeCooksToEmptyGeometry) {
    Network net;
    NodeId merge = net.addNode("merge");
    CHECK(net.cook(merge)->pointCount() == 0);
}

TEST(tagsArePlainOrKeyValueAndSurviveSaveSnapshotAndPaste) {
    Network net;
    const NodeId id = net.addNode("circle");
    Node& n = net.node(id);
    CHECK(n.tags().empty() && !n.tagValue("color"));
    CHECK(n.addTag("layer") && n.addTag("color:red") && n.addTag("path:C:/art"));
    CHECK(!n.addTag("layer") && !n.addTag(""));
    CHECK(n.tags().size() == 3 && n.hasTag("layer") && n.hasTag("color:red") && !n.hasTag("color"));
    CHECK(*n.tagValue("color") == "red" && *n.tagValue("path") == "C:/art" && !n.tagValue("layer"));

    // a tag does not dirty the node
    net.cook(id);
    const uint64_t revision = n.revision();
    n.addTag("locked:true");
    CHECK(!n.dirty() && n.revision() != revision);

    Network loaded;
    loadNetwork(loaded, saveNetwork(net));
    CHECK(loaded.node(id).tags() == n.tags());

    GraphSnapshot before = snapshotNetwork(net);
    CHECK(n.removeTag("locked:true") && !n.removeTag("locked:true"));
    GraphSnapshot after = snapshotNetwork(net, &before);
    CHECK(!sameGraph(before, after));
    restoreSnapshot(net, before, &after);
    CHECK(net.node(id).hasTag("locked:true"));

    // a snapshot without tags clears the tags a node has since gained
    GraphSnapshot tagged = snapshotNetwork(net, &before);
    n.setTags({});
    GraphSnapshot bare = snapshotNetwork(net, &tagged);
    restoreSnapshot(net, tagged, &bare);
    CHECK(net.node(id).hasTag("layer"));
    restoreSnapshot(net, bare, &tagged);
    CHECK(net.node(id).tags().empty());

    n.setTags({"group", "size:3"});
    const std::vector<NodeId> pasted = pasteNodes(net, clipsFromText(clipsToText({copyNode(net, id)})));
    CHECK(pasted.size() == 1 && net.node(pasted[0]).tags() == n.tags());
}

TEST(parameterTagsAPersonAddedSurviveSaveSnapshotAndPasteButTheDeclaredOnesAreNotSaved) {
    Network net;
    const NodeId id = net.addNode("transform");
    Node& n = net.node(id);
    CHECK(n.paramTags("sx") == std::vector<std::string>{"unit:0.01"} && n.userParamTags("sx").empty());

    n.addSpareFloat("mine", 1.0f);
    n.addParamTag("sx", "hot");
    n.addParamTag("sx", "unit:0.01");   // declared already: not stored twice
    n.addParamTag("sx", "hot");
    n.addParamTag("sx", "");
    n.setParamTags("mine", {"group:color", "channel:r", "group:color"});
    CHECK((n.paramTags("sx") == std::vector<std::string>{"unit:0.01", "hot"}) && n.userParamTags("sx") == std::vector<std::string>{"hot"});
    CHECK((n.paramTags("mine") == std::vector<std::string>{"group:color", "channel:r"}));
    CHECK(*n.paramTagValue("mine", "channel") == "r");

    // a parameter tag does not dirty the node
    net.connect(net.addNode("circle"), id);
    net.cook(id);
    const uint64_t revision = n.revision();
    n.addParamTag("sy", "x");
    CHECK(!n.dirty() && n.revision() != revision);
    n.setParamTags("sy", {});

    const std::string text = saveNetwork(net);
    CHECK(text.find("ptags") != std::string::npos && text.find("unit:0.01") == std::string::npos);   // the declared tag stays in the type
    Network loaded;
    loadNetwork(loaded, text);
    CHECK(loaded.node(id).paramTags("sx") == n.paramTags("sx") && loaded.node(id).paramTags("mine") == n.paramTags("mine"));
    CHECK(loaded.node(id).userParamTags("sy").empty());
    CHECK(saveNetwork(loaded) == text);

    GraphSnapshot before = snapshotNetwork(net);
    n.setParamTags("sx", {});
    GraphSnapshot after = snapshotNetwork(net, &before);
    CHECK(!sameGraph(before, after));
    restoreSnapshot(net, before, &after);
    CHECK(net.node(id).userParamTags("sx") == std::vector<std::string>{"hot"});
    restoreSnapshot(net, after, &before);   // a snapshot without ptags clears what was gained since
    CHECK(net.node(id).userParamTags("sx").empty() && net.node(id).paramTags("sx") == std::vector<std::string>{"unit:0.01"});
    restoreSnapshot(net, before, &after);

    const std::vector<NodeId> pasted = pasteNodes(net, clipsFromText(clipsToText({copyNode(net, id)})));
    CHECK(pasted.size() == 1 && net.node(pasted[0]).paramTags("mine") == n.paramTags("mine") && net.node(pasted[0]).paramTags("sx") == n.paramTags("sx"));
}

TEST(aParameterSaysWhatKindOfValueItIs) {
    Network net;
    Node& n = net.node(net.addNode("transform"));
    CHECK(n.paramKind("sx") == ParamKind::scalar);
    CHECK(n.paramKind("nothing") == ParamKind::none);

    n.addSpareFloat("tint", 0.0f);
    n.addParamTag("tint", "kind:color");
    CHECK(n.paramKind("tint") == ParamKind::color);
    n.addSpareFloat("mode", 0.0f);
    n.addParamTag("mode", "choices:A,B");
    CHECK(n.paramKind("mode") == ParamKind::choice);
    n.addSpareFloat("seed", 0.0f);
    n.addParamTag("seed", "kind:id");
    CHECK(n.paramKind("seed") == ParamKind::number);
    n.addSpareFloat("amount", 0.0f);
    n.addParamTag("amount", "kind:scalar");
    CHECK(n.paramKind("amount") == ParamKind::scalar);

    Node& remap = net.node(net.addNode("remap"));
    CHECK(remap.paramKind("ramp") == ParamKind::ramp);
    CHECK(remap.paramKind("attribute") == ParamKind::text);
}

TEST(aSubnetsRampIsFollowedByInnerRampsThroughSaveAssetsAndRename) {
    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", 3.0f);
    const NodeId sub = net.addNode("subnet");
    net.connect(line, sub);
    Network& inner = *net.node(sub).childNetwork();
    const NodeId in = inner.addNode("subnetInput");
    const NodeId remap = inner.addNode("remap");
    inner.node(remap).setString("attribute", "x");
    inner.node(remap).setString("output", "w");
    inner.node(remap).setString("ramp", "parent.profile");
    inner.connect(in, remap);
    inner.setDisplayNode(remap);
    net.node(sub).addSpareRamp("profile", "0 0.25, 1 0.25");
    const auto w = [](Network& of, NodeId id) {
        const GeometryHandle geometry = of.cook(id);
        return (*geometry->findPointAttrib("w"))[0];
    };
    CHECK(w(net, sub) == 0.25f);
    net.node(sub).setString("profile", "0 0.5, 1 0.5");
    CHECK(w(net, sub) == 0.5f);

    Network loaded;
    loadNetwork(loaded, saveNetwork(net));
    CHECK(loaded.node(sub).isSpareRamp("profile") && w(loaded, sub) == 0.5f);

    const AssetDefinition definition = AssetDefinition::fromNode(net, sub, "rampAssetUnderTest");
    CHECK(definition.params.size() == 1 && definition.params[0].ramp && definition.params[0].defaultRamp == "0 0.5, 1 0.5");
    CHECK(definition.validate().empty());
    registerAsset(AssetDefinition::fromText(definition.toText()).toText());
    const NodeId instance = net.addNode("rampAssetUnderTest");
    net.connect(line, instance);
    CHECK(w(net, instance) == 0.5f);
    net.node(instance).setString("profile", "0 0.75, 1 0.75");
    CHECK(w(net, instance) == 0.75f);

    CHECK(renameSpareParameter(net, sub, "profile", "shape") == 1);
    CHECK(inner.node(remap).getString("ramp") == "parent.shape" && w(net, sub) == 0.5f);
}

TEST(aWrangleRampMatchesTheRampOnEveryInstructionSetAndWithoutTheJit) {
    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", 101.0f);
    net.node(line).setFloat("length", 1.0f);
    const NodeId wrangle = net.addNode("wrangle");
    net.connect(line, wrangle);
    net.node(wrangle).setString("code", "| gain = 2, profile = ramp(\"0 0 smooth, 0.5 1 linear, 1 0.25\") |\nw = profile(x * 1.5 - 0.25) * gain;\nbad = profile(sqrt(0 - 1));");
    CHECK(net.node(wrangle).isSpareRamp("profile") && net.node(wrangle).isSpare("gain"));

    const auto check = [&](const std::string& text) {
        const Ramp ramp = Ramp::parse(text);
        const GeometryHandle out = net.cook(wrangle);
        const AlignedArray<float>& w = *out->findPointAttrib("w");
        for (size_t i = 0; i < out->pointCount(); ++i) {
            CHECK(std::fabs(w[i] - 2.0f * ramp.eval(out->x()[i] * 1.5f - 0.25f)) < 0.02f);
        }
        CHECK((*out->findPointAttrib("bad"))[0] == ramp.eval(ramp.points().front().pos));
    };
    for (const Isa isa : net.compiler().supportedIsas()) {
        net.compiler().setIsa(isa);
        net.node(wrangle).invalidateCook();
        check(net.node(wrangle).getString("profile"));
    }
    net.compiler().setJitEnabled(false);
    net.node(wrangle).invalidateCook();
    check(net.node(wrangle).getString("profile"));
    net.compiler().setJitEnabled(true);

    // editing the curve does not compile again
    net.node(wrangle).invalidateCook();   // switching the JIT emptied the kernel cache
    check(net.node(wrangle).getString("profile"));
    const size_t compiled = net.compiler().compileCount();
    net.node(wrangle).setString("profile", "0 1, 1 0");
    check("0 1, 1 0");
    CHECK(net.compiler().compileCount() == compiled);

    // fused with a following wrangle, the table keeps its place in the parameter block
    const NodeId after = net.addNode("wrangle");
    net.connect(wrangle, after);
    net.node(after).setString("code", "| lift = ramp(\"0 3, 1 3\") |\nv = w + lift(x);");
    const GeometryHandle fused = net.cook(after);
    CHECK(std::fabs((*fused->findPointAttrib("v"))[0] - (*fused->findPointAttrib("w"))[0] - 3.0f) < 1e-5f);

    Network loaded;
    loadNetwork(loaded, saveNetwork(net));
    CHECK(loaded.node(wrangle).getString("profile") == "0 1, 1 0");

    net.node(wrangle).setString("code", "w = x;");
    CHECK(!net.node(wrangle).hasString("profile") && !net.node(wrangle).hasFloat("gain"));
}

TEST(geometryNodeOutputsItsStoredGeometryAndFollowsEdits) {
    Network net;
    NodeId src = net.addNode("geometry");
    NodeId flat = net.addNode("flatten");
    net.connect(src, flat);
    CHECK(net.cook(src)->pointCount() == 0);
    net.node(src).setGeometryParam("geometry", authoredCurve(0.0f));
    CHECK(sameGeometry(*net.cook(src), *authoredCurve(0.0f)));
    const GeometryHandle flatBefore = net.cook(flat);
    CHECK(flatBefore->pointCount() > authoredCurve(0.0f)->pointCount());

    net.node(src).setGeometryParam("geometry", authoredCurve(2.0f));
    CHECK(sameGeometry(*net.cook(src), *authoredCurve(2.0f)));
    CHECK(!sameGeometry(*flatBefore, *net.cook(flat)));

    Network loaded;
    loadNetwork(loaded, saveNetwork(net));
    NodeId again = loaded.nodeIds()[0];
    for (NodeId id : loaded.nodeIds()) if (loaded.node(id).typeName() == "geometry") again = id;
    CHECK(sameGeometry(*loaded.cook(again), *authoredCurve(2.0f)));
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

TEST(uidsAreUniqueInTheDocumentAndSurviveSaveAndLoad) {
    SubnetRig rig;
    std::set<uint32_t> seen;
    for (NodeId id : rig.net.nodeIds()) seen.insert(rig.net.node(id).uid());
    for (NodeId id : rig.inner().nodeIds()) seen.insert(rig.inner().node(id).uid());
    CHECK(seen.size() == 5 && !seen.count(0));

    Network loaded;
    loadNetwork(loaded, saveNetwork(rig.net));
    CHECK(loaded.node(rig.sub).uid() == rig.net.node(rig.sub).uid());
    Network& inner = subnetOf(loaded, rig.sub).network();
    CHECK(inner.node(rig.xf).uid() == rig.inner().node(rig.xf).uid());
    const NodeId more = inner.addNode("null");
    CHECK(!seen.count(inner.node(more).uid()));
    CHECK(!seen.count(loaded.node(loaded.addNode("null")).uid()));
}

TEST(pasteGivesTheCopiesFreshUids) {
    SubnetRig rig;
    const std::vector<NodeId> pasted = pasteNodes(rig.net, clipsFromText(clipsToText({copyNode(rig.net, rig.sub)})));
    Node& copy = rig.net.node(pasted[0]);
    CHECK(copy.uid() != rig.net.node(rig.sub).uid());
    Network& copyInner = *copy.childNetwork();
    CHECK(copyInner.node(rig.xf).uid() != rig.inner().node(rig.xf).uid());
    CHECK(copyInner.node(rig.xf).uid() != copy.uid());
}

TEST(restoringASnapshotKeepsTheUidOfARemovedNode) {
    Network net;
    const NodeId a = net.addNode("circle");
    const NodeId b = net.addNode("circle");
    const uint32_t uidB = net.node(b).uid();
    GraphSnapshot before = snapshotNetwork(net);
    net.removeNode(b);
    GraphSnapshot after = snapshotNetwork(net, &before);
    restoreSnapshot(net, before, &after);
    CHECK(net.node(b).uid() == uidB && net.node(a).uid() != uidB);
}

TEST(shapeKeysAreBuiltFromUidsAndIgnoreParameters) {
    Network net;
    const NodeId source = net.addNode("line");
    const NodeId sub = net.addNode("subnet");
    net.connect(source, sub);
    Network& inner = subnetOf(net, sub).network();
    const NodeId circle = inner.addNode("circle");
    inner.setDisplayNode(circle);
    net.setDisplayNode(sub);
    const std::string expected = net.node(sub).uidText() + "/" + inner.node(circle).uidText();
    auto geo = net.cookDisplay();
    CHECK(geo->shapes().size() == 1 && geo->shapeKey(0) == expected);
    inner.node(circle).setFloat("radius", 5.0f);
    CHECK(net.cookDisplay()->shapeKey(0) == expected);

    Network flat;
    const NodeId top = flat.addNode("line");
    flat.setDisplayNode(top);
    CHECK(flat.cookDisplay()->shapeKey(0) == flat.node(top).uidText());
}

TEST(copiesOfAKeyedShapeGetTheirOwnKeys) {
    Network net;
    const NodeId circle = net.addNode("circle");
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", 3.0f);
    const NodeId copies = net.addNode("copyToPoints");
    net.connect(circle, copies, 0);
    net.connect(line, copies, 1);
    const std::string key = net.node(circle).uidText();
    auto geo = net.cook(copies);
    const std::vector<ShapeRange> shapes = geo->shapes();
    CHECK(shapes.size() == 3);
    CHECK(shapes[0].key == key + "@0" && shapes[2].key == key + "@2");
    CHECK(!checkShapesThrows(*geo));

    const NodeId mirror = net.addNode("mirror");
    net.connect(circle, mirror);
    auto both = net.cook(mirror);
    CHECK(both->shapes().size() == 2 && both->shapes()[0].key == key + "@0" && both->shapes()[1].key == key + "@1");
}

TEST(aBindingCoversAKeyOrAPrefixEndingAtALevel) {
    CHECK(shapeKeyMatches("ab", "ab") && shapeKeyMatches("ab/cd", "ab") && shapeKeyMatches("ab#g5", "ab") && shapeKeyMatches("ab@3", "ab"));
    CHECK(shapeKeyMatches("ab/cd@1", "ab/cd") && shapeKeyMatches("ab/cd@1", "ab/cd@1"));
    CHECK(!shapeKeyMatches("abc", "ab") && !shapeKeyMatches("ab", "abc") && !shapeKeyMatches("ab", "") && !shapeKeyMatches("", "ab"));
}

namespace {
// The average x of every point in the shape.
float shapeMeanX(const Geometry& g, const ShapeRange& shape) {
    float sum = 0.0f;
    size_t n = 0;
    for (uint32_t p = shape.first; p < shape.first + shape.count; ++p) {
        const Primitive& prim = g.primitives()[p];
        for (uint32_t i = prim.start; i < prim.start + prim.count; ++i, ++n) {
            sum += g.x()[i];
        }
    }
    return sum / static_cast<float>(n);
}
}  // namespace

TEST(shapeTransformMovesOnlyTheBoundShapesAndKeepsEveryKey) {
    Network net;
    const NodeId circle = net.addNode("circle");
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", 3.0f);
    const NodeId copies = net.addNode("copyToPoints");
    net.connect(circle, copies, 0);
    net.connect(line, copies, 1);
    const NodeId rig = net.addNode("shapeTransform");
    net.connect(copies, rig);
    const std::string key = net.node(circle).uidText();

    auto before = net.cook(copies);
    const std::vector<ShapeRange> was = before->shapes();
    CHECK(was.size() == 3);
    const float x0 = shapeMeanX(*before, was[0]), x1 = shapeMeanX(*before, was[1]), x2 = shapeMeanX(*before, was[2]);

    net.node(rig).setString("bind", key + "@1");
    net.node(rig).setFloat("tx", 100.0f);
    auto moved = net.cook(rig);
    const std::vector<ShapeRange> now = moved->shapes();
    CHECK(now.size() == 3 && now[0].key == was[0].key && now[1].key == was[1].key && now[2].key == was[2].key);
    CHECK_NEAR(shapeMeanX(*moved, now[0]), x0);
    CHECK_NEAR(shapeMeanX(*moved, now[1]), x1 + 100.0f);
    CHECK_NEAR(shapeMeanX(*moved, now[2]), x2);
    CHECK_NEAR(shapeMeanX(*before, was[1]), x1);  // the input is not touched

    net.node(rig).setString("bind", key);  // the node's key is a prefix of every copy
    auto all = net.cook(rig);
    CHECK_NEAR(shapeMeanX(*all, all->shapes()[0]), x0 + 100.0f);
    CHECK_NEAR(shapeMeanX(*all, all->shapes()[2]), x2 + 100.0f);

    net.node(rig).setString("bind", "nothing, here");  // a binding that finds nothing leaves the geometry as it was
    auto same = net.cook(rig);
    CHECK_NEAR(shapeMeanX(*same, same->shapes()[1]), x1);
    net.node(rig).setString("bind", "");
    auto none = net.cook(rig);
    CHECK_NEAR(shapeMeanX(*none, none->shapes()[1]), x1);
}

TEST(shapeTransformTurnsAndScalesAroundThePivot) {
    Network net;
    const NodeId circle = net.addNode("circle");
    net.node(circle).setFloat("count", 4.0f);
    net.node(circle).setFloat("cx", 10.0f);
    net.node(circle).setFloat("radius", 2.0f);
    const NodeId rig = net.addNode("shapeTransform");
    net.connect(circle, rig);
    net.node(rig).setString("bind", net.node(circle).uidText());

    net.node(rig).setFloat("scaleX", 2.0f);  // around the centre, (10, 0): the point at (12, 0) goes to (14, 0)
    auto wide = net.cook(rig);
    CHECK_NEAR(wide->x()[0], 14.0f);
    CHECK_NEAR(wide->x()[2], 6.0f);

    net.node(rig).setFloat("scaleX", 1.0f);
    net.node(rig).setFloat("angle", 90.0f);
    net.node(rig).setString("pivot", "point");  // around the origin: (12, 0) turns to (0, 12)
    auto turned = net.cook(rig);
    CHECK_NEAR(turned->x()[0], 0.0f);
    CHECK_NEAR(turned->y()[0], 12.0f);

    net.node(rig).setString("pivot", "elsewhere");
    bool refused = false;
    try {
        net.cook(rig);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
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

TEST(editingTheEndOfALongFusedChainKeepsTheRestCooked) {
    Network net;
    const NodeId line = net.addNode("line");
    net.node(line).setFloat("count", 4000.0f);
    std::vector<NodeId> xf;
    NodeId prev = line;
    for (int i = 0; i < 5; ++i) {
        xf.push_back(net.addNode("transform"));
        net.node(xf.back()).setFloat("tx", 1.0f);
        net.connect(prev, xf.back());
        prev = xf.back();
    }
    net.setDisplayNode(prev);
    const float base = net.cookDisplay()->x()[0] - 5.0f;
    CHECK(net.node(xf[4]).cookCount() == 1 && net.node(xf[3]).cookCount() == 0);  // the whole chain ran once, fused

    net.node(xf[4]).setFloat("tx", 10.0f);  // the first edit splits the chain below the edited node
    CHECK_NEAR(net.cookDisplay()->x()[0], base + 14.0f);
    CHECK(net.node(xf[3]).cookCount() == 1);
    for (int i = 0; i < 3; ++i) {
        net.node(xf[4]).setFloat("tx", 20.0f + static_cast<float>(i));
        CHECK_NEAR(net.cookDisplay()->x()[0], base + 24.0f + static_cast<float>(i));
    }
    CHECK(net.node(xf[3]).cookCount() == 1 && net.node(xf[4]).cookCount() == 5);

    net.node(xf[1]).setFloat("tx", 3.0f);  // an edit further up still reaches everything below it
    CHECK_NEAR(net.cookDisplay()->x()[0], base + 3.0f + 3.0f + 22.0f);
}

TEST(setTimeCacheFollowsTheOwnersParametersIntoSubnets) {
    SubnetRig rig;
    subnetOf(rig.net, rig.sub).addSpareFloat("amount", 2.0f);
    rig.inner().node(rig.xf).setExpression("tx", "parent.amount");
    CHECK(!rig.net.setTime(1.0));                    // builds both caches: nothing depends on time yet
    rig.net.cook(rig.out);
    rig.net.node(rig.sub).setFloat("amount", 3.0f);  // a plain value edit keeps the caches and is still seen by the cook
    CHECK(!rig.net.setTime(1.5));
    CHECK_NEAR(rig.net.cook(rig.out)->x()[0], 3.0f);
    rig.net.node(rig.sub).setKeyframes("amount", {{0.0, 0.0f, KeyInterp::linear}, {2.0, 10.0f, KeyInterp::linear}});
    CHECK(rig.net.setTime(1.0));                     // the inner expression now follows time through the owner
    CHECK_NEAR(rig.net.cook(rig.out)->x()[0], 5.0f);
    rig.net.node(rig.sub).clearAnimation("amount");
    rig.net.cook(rig.out);
    CHECK(!rig.net.setTime(1.25));
    rig.inner().node(rig.xf).setExpression("ty", "time");
    CHECK(rig.net.setTime(1.5));                     // an edit inside the subnet reaches the owner's cache
    CHECK_NEAR(rig.net.cook(rig.out)->y()[0], 1.5f);
}

TEST(snapshotsReuseUntouchedNodesAndSeeEveryKindOfEdit) {
    SubnetRig rig;
    Network& net = rig.net;
    const NodeId wr = net.addNode("wrangle", "wr");
    const NodeId other = net.addNode("transform", "other");
    const GraphSnapshot before = snapshotNetwork(net);

    GraphSnapshot same = snapshotNetwork(net, &before);
    CHECK(sameGraph(before, same));
    for (const auto& [id, node] : before.nodes) CHECK(id == rig.sub || node.state == same.nodes.at(id).state);

    const std::vector<std::pair<const char*, std::function<void()>>> edits = {
        {"plain value", [&] { net.node(other).setFloat("tx", 4.0f); }},
        {"expression", [&] { net.node(other).setExpression("ty", "time * 2"); }},
        {"base value under an expression", [&] { net.node(other).setFloat("ty", 9.0f); }},
        {"keyframes", [&] { net.node(other).setKeyframes("sx", {{0.0, 1.0f, KeyInterp::linear}, {1.0, 2.0f, KeyInterp::linear}}); }},
        {"string", [&] { net.node(wr).setString("code", "y = x;"); }},
        {"spare parameter", [&] { net.node(other).addSpareFloat("mine", 1.5f); }},
        {"position", [&] { net.node(other).setPosition(12.0f, 34.0f); }},
        {"rename", [&] { net.renameNode(other, "renamed"); }},
        {"subnet contents", [&] { rig.inner().node(rig.xf).setFloat("ty", 7.0f); }},
    };
    GraphSnapshot previous = before;
    for (const auto& [what, edit] : edits) {
        edit();
        GraphSnapshot incremental = snapshotNetwork(net, &previous);
        const GraphSnapshot fresh = snapshotNetwork(net);
        CHECK(sameGraph(incremental, fresh));
        CHECK(!sameGraph(incremental, previous));
        for (const auto& [id, node] : fresh.nodes) CHECK(node.state->hasSameStateAs(*incremental.nodes.at(id).state));
        previous = std::move(incremental);
        if (!sameGraph(previous, fresh)) std::printf("edit '%s' lost\n", what);
    }

    net.node(other).setFloat("tx", 4.0f);  // setting the value it already has is no edit
    CHECK(sameGraph(snapshotNetwork(net, &previous), previous));
    const uint64_t revision = net.node(other).revision();
    net.node(other).setPosition(12.0f, 34.0f);
    CHECK(net.node(other).revision() == revision);
}

TEST(snapshotsOfSubnetsRestoreThemInPlace) {
    SubnetRig rig;
    Network& net = rig.net;
    Network& inner = rig.inner();
    const NodeId nestedId = inner.addNode("subnet", "nested");
    Network& nested = subnetOf(inner, nestedId).network();
    const NodeId deep = nested.addNode("transform", "deep");

    const GraphSnapshot start = snapshotNetwork(net);
    const GraphSnapshot fresh = snapshotNetwork(net);
    CHECK(sameGraph(start, fresh));

    inner.node(rig.xf).setFloat("ty", 3.0f);
    nested.node(deep).setFloat("tx", 8.0f);
    const NodeId extra = inner.addNode("transform", "extra");
    inner.connect(rig.in, extra);
    const GraphSnapshot edited = snapshotNetwork(net, &start);
    CHECK(!sameGraph(edited, start));
    CHECK(sameGraph(edited, snapshotNetwork(net)));

    const uint64_t xfRevision = inner.node(rig.xf).revision();
    restoreSnapshot(net, start, &edited);
    CHECK(sameGraph(snapshotNetwork(net), start));
    CHECK(inner.nodeIds().size() == 3 && nested.node(deep).baseFloat("tx") == 0.0f);
    CHECK(&subnetOf(net, rig.sub).network() == &inner);  // the child network object survives, only the differing nodes change
    CHECK(inner.node(rig.xf).revision() != xfRevision);

    restoreSnapshot(net, edited, &start);
    CHECK(sameGraph(snapshotNetwork(net), edited));
    CHECK(inner.node(rig.xf).baseFloat("ty") == 3.0f && nested.node(deep).baseFloat("tx") == 8.0f && inner.nodeIds().size() == 4);

    // A subnet that is deleted and brought back by undo returns with its contents.
    const GraphSnapshot withSub = snapshotNetwork(net);
    net.removeNode(rig.sub);
    const GraphSnapshot without = snapshotNetwork(net, &withSub);
    restoreSnapshot(net, withSub, &without);
    CHECK(sameGraph(snapshotNetwork(net), withSub));
    CHECK(subnetOf(net, rig.sub).network().nodeIds().size() == 4);
}

TEST(snapshotsOfNodesHoldingStrokeGeometryAreTimed) {
    using Clock = std::chrono::steady_clock;
    auto since = [](Clock::time_point s) { return std::chrono::duration<double, std::milli>(Clock::now() - s).count(); };
    auto stroke = [](size_t samples, float phase) {
        auto geo = std::make_shared<Geometry>();
        geo->resizePoints(samples);
        for (const char* name : {"t", "pressure", "tiltx", "tilty", "speed", "width", "angle", "d"}) {
            float* values = geo->attrib(AttribClass::point, name).floats().data();
            for (size_t i = 0; i < samples; ++i) values[i] = std::sin(float(i) * 0.037f + phase) * 12.3456789f;
        }
        for (size_t i = 0; i < samples; ++i) {
            geo->x()[i] = float(i) * 0.731f + phase;
            geo->y()[i] = std::sin(float(i) * 0.11f + phase) * 40.0f;
        }
        Primitive prim;
        prim.start = 0;
        prim.count = static_cast<uint32_t>(samples);
        geo->addPrimitive(prim);
        return geo;
    };
    for (const size_t samples : {size_t(150), size_t(800)}) {
        Network net;
        for (int i = 0; i < 30; ++i) {
            const NodeId id = net.addNode("geometry");
            net.node(id).setGeometryParam("geometry", stroke(samples, float(i)));
            const NodeId xf = net.addNode("transform");
            net.connect(id, xf);
        }
        const GraphSnapshot base = snapshotNetwork(net);
        const GraphSnapshot before = snapshotNetwork(net, &base);

        auto t = Clock::now();
        const NodeId added = net.addNode("geometry");
        net.node(added).setGeometryParam("geometry", stroke(samples, 99.0f));
        const GraphSnapshot withStroke = snapshotNetwork(net, &before);
        const double addMs = since(t);
        t = Clock::now();
        restoreSnapshot(net, before, &withStroke);
        const double undoAdd = since(t);
        t = Clock::now();
        restoreSnapshot(net, withStroke, &before);
        const double redoAdd = since(t);

        const NodeId dragged = net.nodeIds().front();
        t = Clock::now();
        net.node(dragged).setGeometryParam("geometry", stroke(samples, 7.5f));
        const GraphSnapshot dragSnap = snapshotNetwork(net, &withStroke);
        const double dragMs = since(t);
        t = Clock::now();
        restoreSnapshot(net, withStroke, &dragSnap);
        const double undoDrag = since(t);

        NodeId other = invalidNodeId;
        for (const NodeId id : net.nodeIds()) {
            if (net.node(id).typeName() == "transform") {
                other = id;
            }
        }
        t = Clock::now();
        net.node(other).setFloat("tx", 5.0f);
        const GraphSnapshot floatSnap = snapshotNetwork(net, &withStroke);
        const double floatMs = since(t);
        CHECK(!sameGraph(floatSnap, withStroke));
        std::printf("  snapshot cost, %zu-sample strokes: add %.3f ms (undo %.3f, redo %.3f), replace geometry %.3f ms (undo %.3f), float edit %.3f ms\n", samples,
                    addMs, undoAdd, redoAdd, dragMs, undoDrag, floatMs);
    }
}

TEST(snapshotsOfBigSubnetsCostAboutAsMuchAsFlatOnes) {
    using Clock = std::chrono::steady_clock;
    auto since = [](Clock::time_point s) { return std::chrono::duration<double, std::milli>(Clock::now() - s).count(); };
    for (int mode = 0; mode < 2; ++mode) {
        Network net;
        Network* target = &net;
        if (mode == 1) target = &subnetOf(net, net.addNode("subnet", "group")).network();
        std::vector<NodeId> ids;
        for (int i = 0; i < 2000; ++i) ids.push_back(target->addNode("transform"));
        for (int i = 0; i < 2000; ++i) net.addNode("transform");
        const GraphSnapshot base = snapshotNetwork(net);
        const GraphSnapshot before = snapshotNetwork(net, &base);
        auto t = Clock::now();
        target->node(ids[1000]).setFloat("tx", 5.0f);
        const GraphSnapshot after = snapshotNetwork(net, &before);
        const double edit = since(t);
        t = Clock::now();
        restoreSnapshot(net, before, &after);
        const double undo = since(t);
        t = Clock::now();
        restoreSnapshot(net, after, &before);
        const double redo = since(t);
        CHECK(target->node(ids[1000]).baseFloat("tx") == 5.0f);
        std::printf("  snapshot cost, 2000 nodes %s: edit+snapshot %.2f ms, undo %.2f ms, redo %.2f ms\n", mode ? "in a subnet" : "flat", edit, undo, redo);
    }
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
    CHECK(net.cookDisplay()->pointCount() == 0);
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

TEST(bypassedNodePassesItsFirstInputThroughAndWinsOverFreeze) {
    Network net(1);
    NodeId line = net.addNode("line");
    NodeId xf = net.addNode("transform");
    NodeId out = net.addNode("null");
    net.node(line).setFloat("count", 10.0f);
    net.node(xf).setFloat("tx", 5.0f);
    net.connect(line, xf);
    net.connect(xf, out);
    const float moved = net.cook(out)->x()[0];
    CHECK_NEAR(moved, net.cook(line)->x()[0] + 5.0f);

    net.node(xf).setBypassed(true);
    expectSameGeometry(*net.cook(out), *net.cook(line));
    net.node(xf).setBypassed(false);
    CHECK_NEAR(net.cook(out)->x()[0], moved);

    net.node(xf).setBypassed(true);
    CHECK(throwsRuntimeError([&] { net.freezeNode(xf); }));
    net.node(xf).setFrozenGeometry(std::make_shared<const Geometry>());
    CHECK(net.cook(out)->pointCount() == 10);
}

TEST(bypassedNodeWithoutAnInputGivesAnEmptyGeometry) {
    Network net(1);
    NodeId xf = net.addNode("transform");
    CHECK(throwsRuntimeError([&] { net.cook(xf); }));
    net.node(xf).setBypassed(true);
    CHECK(net.cook(xf)->pointCount() == 0);
}

TEST(bypassedNodeLeavesAFusedChainAndTheRestStillFuses) {
    Chain fused(true), plain(false);
    for (Chain* c : {&fused, &plain}) c->net.node(c->wr).setBypassed(true);
    expectSameGeometry(*fused.net.cookDisplay(), *plain.net.cookDisplay());
    CHECK(fused.net.cookDisplay()->pointCount() == 21);
    fused.net.node(fused.wr).setBypassed(false);
    plain.net.node(plain.wr).setBypassed(false);
    expectSameGeometry(*fused.net.cookDisplay(), *plain.net.cookDisplay());
    CHECK(fused.net.fusedChainCount() >= 1);
}

TEST(frozenNodeIsNotCookedAgainAndEditsUpstreamDoNotReachIt) {
    Network net(1);
    net.setFusionEnabled(false);
    NodeId line = net.addNode("line");
    NodeId xf = net.addNode("transform");
    NodeId out = net.addNode("null");
    net.node(line).setFloat("count", 10.0f);
    net.connect(line, xf);
    net.connect(xf, out);
    net.freezeNode(xf);
    CHECK(net.node(xf).frozen());
    const GeometryHandle before = net.cook(out);
    const size_t lineCooks = net.node(line).cookCount();

    net.node(line).setFloat("count", 30.0f);
    CHECK(!net.node(out).dirty());
    CHECK(net.cook(out) == before);
    CHECK(net.node(line).cookCount() == lineCooks);
    CHECK(net.cook(out)->pointCount() == 10);

    net.node(xf).setFrozenGeometry(nullptr);
    CHECK(!net.node(xf).frozen());
    CHECK(net.cook(out)->pointCount() == 30);
}

TEST(frozenMergeIsNotAbsorbedIntoTheMergeAfterIt) {
    Network net(1);
    NodeId a = net.addNode("line"), b = net.addNode("line"), c = net.addNode("line");
    NodeId inner = net.addNode("merge"), outer = net.addNode("merge");
    for (NodeId id : {a, b, c}) net.node(id).setFloat("count", 4.0f);
    net.connect(a, inner, 0);
    net.connect(b, inner, 1);
    net.connect(inner, outer, 0);
    net.connect(c, outer, 1);
    net.freezeNode(inner);
    CHECK(net.cook(outer)->pointCount() == 12);
    const size_t aCooks = net.node(a).cookCount();
    net.node(a).setFloat("count", 20.0f);
    CHECK(net.cook(outer)->pointCount() == 12);
    CHECK(net.node(a).cookCount() == aCooks);
    net.node(inner).setFrozenGeometry(nullptr);
    CHECK(net.cook(outer)->pointCount() == 28);
}

TEST(frozenNodeIgnoresTheTimeThatMovesItsParameters) {
    Network net(1);
    NodeId line = net.addNode("line");
    NodeId xf = net.addNode("transform");
    net.node(line).setFloat("count", 5.0f);
    net.node(xf).setKeyframes("tx", {{0.0, 0.0f}, {1.0, 10.0f}});
    net.connect(line, xf);
    const float atZero = net.cook(xf)->x()[0];
    net.freezeNode(xf);
    CHECK(!net.setTime(1.0));
    CHECK_NEAR(net.cook(xf)->x()[0], atZero);
    net.node(xf).setFrozenGeometry(nullptr);
    CHECK_NEAR(net.cook(xf)->x()[0], atZero + 10.0f);
}

TEST(bypassAndFreezeSurviveSaveLoadSnapshotUndoAndPaste) {
    Network net(1);
    NodeId line = net.addNode("line", "src");
    NodeId xf = net.addNode("transform", "xf");
    NodeId out = net.addNode("null", "out");
    net.node(line).setFloat("count", 10.0f);
    net.node(xf).setFloat("tx", 5.0f);
    net.connect(line, xf);
    net.connect(xf, out);
    net.setDisplayNode(out);
    History history(net);
    net.freezeNode(xf);
    const GeometryHandle frozen = net.cook(out);
    CHECK(history.commit("freeze"));

    Network loaded;
    loadNetwork(loaded, saveNetwork(net));
    NodeId loadedXf = loaded.findNode("xf");
    CHECK(loaded.node(loadedXf).frozen() && !loaded.node(loadedXf).bypassed());
    expectSameGeometry(*loaded.cook(loaded.findNode("out")), *frozen);
    CHECK(loaded.node(loaded.findNode("src")).cookCount() == 0);

    net.node(xf).setFrozenGeometry(nullptr);
    net.node(xf).setBypassed(true);
    CHECK(history.commit("bypass instead"));
    CHECK(history.undo());
    CHECK(net.node(xf).frozen() && !net.node(xf).bypassed());
    CHECK(history.redo());
    CHECK(!net.node(xf).frozen() && net.node(xf).bypassed());

    std::vector<NodeId> pasted = pasteNodes(net, clipsFromText(clipsToText({copyNode(net, xf)})));
    CHECK(pasted.size() == 1 && net.node(pasted[0]).bypassed());
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

TEST(assetMigrationFallbackServesTypesWithoutAHookOfTheirOwn) {
    auto define = [](int version, const std::string& param) {
        SubnetRig rig;
        SubnetNode& s = subnetOf(rig.net, rig.sub);
        s.addSpareFloat(param, 1.0f);
        rig.inner().node(rig.xf).setExpression("tx", "parent." + param);
        registerAsset(saveAsset(rig.net, rig.sub, "fallbackAsset", version));
    };
    NodeRegistry& registry = NodeRegistry::instance();
    int fallbackCalls = 0;
    int ownCalls = 0;
    registry.setAssetMigrationFallback([&](AssetUpgrade& up) {
        ++fallbackCalls;
        up.fresh.setFloat("gain", up.old.baseFloat("amount") * 10.0f);
    });
    define(1, "amount");
    Network net;
    const NodeId a = net.addNode("fallbackAsset", "a");
    net.node(a).setFloat("amount", 2.0f);
    const std::string oldText = saveNetwork(net);

    define(2, "gain");
    CHECK(updateAssetInstances(net) == 1 && fallbackCalls == 1);
    CHECK_NEAR(net.node(a).baseFloat("gain"), 20.0f);

    Network loaded(1);
    loadNetwork(loaded, oldText);
    const int afterLoad = fallbackCalls;   // a load runs the hook twice (once to validate)
    CHECK(afterLoad >= 2);
    CHECK_NEAR(loaded.node(loaded.findNode("a")).baseFloat("gain"), 20.0f);

    registry.setAssetMigration("fallbackAsset", [&](AssetUpgrade&) { ++ownCalls; });
    define(3, "gain");
    CHECK(updateAssetInstances(net) == 1 && ownCalls == 1 && fallbackCalls == afterLoad);

    registry.setAssetMigration("fallbackAsset", nullptr);
    registry.setAssetMigrationFallback(nullptr);
    CHECK(registry.assetMigration("fallbackAsset") == nullptr);
}

TEST(copyNodeParametersCopiesWhatTheTargetHasAndOptionallyTheRest) {
    Network net;
    const NodeId from = net.addNode("null", "from");
    const NodeId to = net.addNode("null", "to");
    net.node(from).addSpareFloat("scale", 5.0f);
    net.node(from).addSpareFloat("kept", 1.0f);
    net.node(from).setExpression("kept", "frame");
    net.node(to).addSpareFloat("kept", 0.0f);

    copyNodeParameters(net.node(from), net.node(to), false);
    CHECK(!net.node(to).hasFloat("scale"));
    CHECK(net.node(to).expression("kept") == "frame");

    copyNodeParameters(net.node(from), net.node(to), true);
    CHECK(net.node(to).hasFloat("scale") && net.node(to).isSpare("scale"));
    CHECK_NEAR(net.node(to).baseFloat("scale"), 5.0f);
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

namespace {

struct OneParticle {
    Network net{2};
    NodeId gen, sim;

    OneParticle(float x, float y) {
        gen = net.addNode("generate");
        net.node(gen).setFloat("count", 1.0f);
        net.node(gen).setFloat("cx", x);
        net.node(gen).setFloat("cy", y);
        net.node(gen).setFloat("width", 0.0f);
        net.node(gen).setFloat("height", 0.0f);
        sim = net.addNode("particles");
        net.connect(gen, sim);
        net.node(sim).setFloat("gravityY", 0.0f);
        net.node(sim).setFloat("drag", 0.0f);
    }

    GeometryHandle at(double t) {
        net.setTime(t);
        return net.cook(sim);
    }
};

}  // namespace

TEST(particleVortexJetAndWindForcesMoveParticlesAsDescribed) {
    // A positive vortex pushes counter-clockwise, only inside its radius.
    OneParticle v(1.0f, 0.0f);
    v.net.node(v.sim).setFloat("vortex", 8.0f);
    v.net.node(v.sim).setFloat("vortexRadius", 2.0f);
    auto swirled = v.at(0.05);
    CHECK(swirled->findAttrib(AttribClass::point, "vy")->at(0) > 0.05f);
    CHECK(std::fabs(swirled->findAttrib(AttribClass::point, "vx")->at(0)) < 1e-4f);
    OneParticle outside(3.0f, 0.0f);
    outside.net.node(outside.sim).setFloat("vortex", 8.0f);
    outside.net.node(outside.sim).setFloat("vortexRadius", 2.0f);
    CHECK_NEAR(outside.at(0.5)->findAttrib(AttribClass::point, "vy")->at(0), 0.0f);
    v.net.node(v.sim).setFloat("vortex", -8.0f);
    CHECK(v.at(0.05)->findAttrib(AttribClass::point, "vy")->at(0) < -0.05f);

    // A jet along +x accelerates what is in front of it and inside its width, and nothing else.
    for (float y : {0.0f, 0.3f, 0.9f, -0.3f}) {
        OneParticle j(1.0f, y);
        j.net.node(j.sim).setFloat("jetAngle", 0.0f);
        j.net.node(j.sim).setFloat("jetForce", 10.0f);
        j.net.node(j.sim).setFloat("jetLength", 3.0f);
        j.net.node(j.sim).setFloat("jetWidth", 2.0f);
        const float vx = j.at(0.1)->findAttrib(AttribClass::point, "vx")->at(0);
        const float vy = j.at(0.1)->findAttrib(AttribClass::point, "vy")->at(0);
        CHECK(vx > 0.0f);
        CHECK_NEAR(vy, 0.0f);
        CHECK(vx <= 10.0f * 0.1f + 1e-4f);
    }
    for (std::pair<float, float> p : {std::pair{-1.0f, 0.0f}, {5.0f, 0.0f}, {1.0f, 1.5f}}) {
        OneParticle j(p.first, p.second);
        j.net.node(j.sim).setFloat("jetAngle", 0.0f);
        j.net.node(j.sim).setFloat("jetForce", 10.0f);
        j.net.node(j.sim).setFloat("jetWidth", 2.0f);
        CHECK_NEAR(j.at(0.1)->findAttrib(AttribClass::point, "vx")->at(0), 0.0f);
    }

    // Wind drags the velocity to the wind's: exponentially, whatever the start.
    OneParticle w(0.0f, 0.0f);
    w.net.node(w.sim).setFloat("windX", 3.0f);
    w.net.node(w.sim).setFloat("windY", -1.0f);
    w.net.node(w.sim).setFloat("windDrag", 4.0f);
    auto gusted = w.at(2.0);
    CHECK(std::fabs(gusted->findAttrib(AttribClass::point, "vx")->at(0) - 3.0f) < 0.05f);
    CHECK(std::fabs(gusted->findAttrib(AttribClass::point, "vy")->at(0) + 1.0f) < 0.05f);
}

TEST(particlesFollowAndCollideWithGuideCurves) {
    // A guide polyline from x = 0 to 10 along y = 0, reversed by a mirror in x for the second case.
    OneParticle p(5.0f, 0.5f);
    NodeId guide = p.net.addNode("line");
    p.net.node(guide).setFloat("count", 11.0f);
    p.net.node(guide).setFloat("length", 10.0f);
    p.net.connect(guide, p.sim, 1);
    p.net.node(p.sim).setFloat("follow", 4.0f);
    p.net.node(p.sim).setFloat("followRadius", 1.0f);
    auto along = p.at(0.2);
    CHECK(along->findAttrib(AttribClass::point, "vx")->at(0) > 0.1f);
    CHECK_NEAR(along->findAttrib(AttribClass::point, "vy")->at(0), 0.0f);

    NodeId flip = p.net.addNode("transform");
    p.net.connect(guide, flip);
    p.net.node(flip).setFloat("sx", -1.0f);
    p.net.connect(flip, p.sim, 1);
    p.net.node(p.gen).setFloat("cx", -5.0f);
    CHECK(p.at(0.2)->findAttrib(AttribClass::point, "vx")->at(0) < -0.1f);

    // Pulling towards the guide closes the distance; outside the radius nothing happens.
    p.net.node(p.sim).setFloat("follow", 0.0f);
    p.net.node(p.sim).setFloat("followPull", 6.0f);
    CHECK(p.at(0.2)->findAttrib(AttribClass::point, "vy")->at(0) < -0.1f);
    p.net.node(p.gen).setFloat("cy", 3.0f);
    CHECK_NEAR(p.at(0.2)->findAttrib(AttribClass::point, "vy")->at(0), 0.0f);

    // A floor made of a guide line: a falling particle never gets below it and bounces back up.
    OneParticle f(5.0f, 0.0f);
    NodeId floor = f.net.addNode("line");
    f.net.node(floor).setFloat("count", 11.0f);
    f.net.node(floor).setFloat("length", 10.0f);
    f.net.node(floor).setFloat("y", -1.0f);
    f.net.connect(floor, f.sim, 1);
    f.net.node(f.sim).setFloat("gravityY", -10.0f);
    f.net.node(f.sim).setFloat("collide", 1.0f);
    f.net.node(f.sim).setFloat("bounce", 1.0f);
    float lowest = 1e9f, highestAfter = -1e9f;
    for (double t = 0.0; t <= 2.0; t += 0.01) {
        const float y = f.at(t)->y()[0];
        lowest = std::min(lowest, y);
        if (t > 0.6) highestAfter = std::max(highestAfter, y);
    }
    CHECK(lowest >= -1.0f);
    CHECK(highestAfter > -0.5f);  // it came back up after the first impact at t = 0.447 s
    f.net.node(f.sim).setFloat("collide", 0.0f);
    CHECK(f.at(2.0)->y()[0] < -10.0f);

    // Running to a time is the same as playing up to it, with guides too, and a guide that is rebuilt with the same lines keeps the run.
    OneParticle g(5.0f, 0.0f);
    NodeId floor2 = g.net.addNode("line");
    g.net.node(floor2).setFloat("count", 11.0f);
    g.net.node(floor2).setFloat("length", 10.0f);
    g.net.node(floor2).setFloat("y", -1.0f);
    g.net.connect(floor2, g.sim, 1);
    for (const char* name : {"gravityY"}) g.net.node(g.sim).setFloat(name, -10.0f);
    g.net.node(g.sim).setFloat("collide", 1.0f);
    g.net.node(g.sim).setFloat("bounce", 0.8f);
    auto played = [&] {
        for (double t = 0.0; t < 1.5; t += 0.1) g.at(t);
        return g.at(1.5);
    }();
    OneParticle h(5.0f, 0.0f);
    NodeId floor3 = h.net.addNode("line");
    h.net.node(floor3).setFloat("count", 11.0f);
    h.net.node(floor3).setFloat("length", 10.0f);
    h.net.node(floor3).setFloat("y", -1.0f);
    h.net.connect(floor3, h.sim, 1);
    h.net.node(h.sim).setFloat("gravityY", -10.0f);
    h.net.node(h.sim).setFloat("collide", 1.0f);
    h.net.node(h.sim).setFloat("bounce", 0.8f);
    CHECK(sameGeometry(*played, *h.at(1.5)));
}

namespace {

struct Smoke {
    Network net;
    NodeId gas;

    explicit Smoke(size_t threads = 1) : net(threads) {
        gas = net.addNode("gasField");
        net.node(gas).setFloat("boundsMinX", -4.0f);
        net.node(gas).setFloat("boundsMaxX", 4.0f);
        net.node(gas).setFloat("boundsMinY", -4.0f);
        net.node(gas).setFloat("boundsMaxY", 4.0f);
        net.node(gas).setFloat("resolution", 32.0f);
        net.node(gas).setFloat("nozzleY", -3.0f);
        net.node(gas).setFloat("nozzleDensity", 20.0f);
        net.node(gas).setFloat("nozzleHeat", 20.0f);
        net.node(gas).setFloat("buoyancy", 3.0f);
    }

    GeometryHandle at(double t) {
        net.setTime(t);
        return net.cook(gas);
    }
};

float densityCentreY(const Geometry& g) {
    const AttribArray* d = g.findAttrib(AttribClass::point, "density");
    double sum = 0.0, weighted = 0.0;
    for (size_t i = 0; i < g.pointCount(); ++i) {
        sum += d->at(i);
        weighted += static_cast<double>(d->at(i)) * g.y()[i];
    }
    return sum > 0.0 ? static_cast<float>(weighted / sum) : 0.0f;
}

}  // namespace

TEST(gasFieldCarriesSmokeUpwardsAndIsAPureFunctionOfTime) {
    Smoke smoke;
    CHECK(smoke.net.dependsOnTime(smoke.gas));
    auto start = smoke.at(0.0);
    CHECK(start->pointCount() == 32u * 32u);
    for (size_t i = 0; i < start->pointCount(); ++i) CHECK(start->findAttrib(AttribClass::point, "density")->at(i) == 0.0f);

    auto early = smoke.at(0.5);
    auto later = smoke.at(2.0);
    const float lowCentre = densityCentreY(*early), highCentre = densityCentreY(*later);
    CHECK(lowCentre < -2.0f && lowCentre > -3.6f);
    CHECK(highCentre > lowCentre + 0.5f);  // hot gas climbs
    float fastest = 0.0f;
    for (size_t i = 0; i < later->pointCount(); ++i) fastest = std::max(fastest, later->findAttrib(AttribClass::point, "speed")->at(i));
    CHECK(fastest > 0.5f);

    // Scrubbing back and forth, and any thread count, give what a fresh run gives.
    smoke.at(1.0);
    auto again = smoke.at(2.0);
    CHECK(sameGeometry(*again, *later));
    Smoke fresh(4);
    CHECK(sameGeometry(*fresh.at(2.0), *later));

    // No heat and no weight: the nozzle only adds smoke that stays near it.
    smoke.net.node(smoke.gas).setFloat("buoyancy", 0.0f);
    smoke.net.node(smoke.gas).setFloat("nozzleHeat", 0.0f);
    CHECK(densityCentreY(*smoke.at(2.0)) < -2.5f);

    smoke.net.node(smoke.gas).setFloat("resolution", 2.0f);
    CHECK(cookThrows(smoke.net, smoke.gas));
    smoke.net.node(smoke.gas).setFloat("resolution", 32.0f);
    smoke.net.node(smoke.gas).setFloat("boundsMaxX", -9.0f);
    CHECK(cookThrows(smoke.net, smoke.gas));
}

TEST(progressCallbackSeesNodesStartAndFinishAndASimulationsSteps) {
    Smoke smoke;
    NodeId pass = smoke.net.addNode("null");
    smoke.net.connect(smoke.gas, pass);
    smoke.net.setDisplayNode(pass);
    std::vector<Network::CookProgress> seen;
    smoke.net.setProgressCallback([&](const Network::CookProgress& p) { seen.push_back(p); });
    smoke.net.setTime(1.0);
    smoke.net.cookDisplay();
    smoke.net.setProgressCallback(nullptr);

    CHECK(seen.size() > 10);
    size_t gasInside = 0;
    double lastGasFraction = -1.0;
    bool monotonic = true;
    for (const auto& p : seen) {
        CHECK(p.network == &smoke.net);
        CHECK(p.nodesTotal == 2);
        if (p.node == smoke.gas && p.fraction > 0.0 && p.fraction < 1.0) {
            ++gasInside;
            monotonic = monotonic && p.fraction >= lastGasFraction;
            lastGasFraction = p.fraction;
        }
    }
    CHECK(gasInside > 20);   // a step loop reports as it goes
    CHECK(monotonic);
    CHECK(seen.back().nodesDone == 2 && seen.back().node == pass && seen.back().fraction == 1.0);
}

TEST(aSimulationNodeCanBeCancelledMidCook) {
    Smoke smoke;
    int calls = 0;
    smoke.net.setProgressCallback([&](const Network::CookProgress& p) {
        if (p.node == smoke.gas && p.fraction > 0.3 && p.fraction < 1.0 && ++calls == 1) smoke.net.cancel();
    });
    smoke.net.setTime(2.0);
    bool threw = false;
    try { smoke.net.cook(smoke.gas); } catch (const CookCancelled&) { threw = true; }
    CHECK(threw);
    smoke.net.setProgressCallback(nullptr);
    CHECK(smoke.net.cook(smoke.gas) != nullptr);   // the next cook is not affected
}

TEST(gasFieldRedBlackIsThreadIndependentAndAgreesWithTheSerialSweep) {
    auto configure = [](Smoke& s, bool redBlack) {
        s.net.node(s.gas).setFloat("resolution", 160.0f);
        s.net.node(s.gas).setFloat("redBlack", redBlack ? 1.0f : 0.0f);
    };
    Smoke one(1), many(4), plain(1);
    configure(one, true);
    configure(many, true);
    configure(plain, false);
    auto a = one.at(1.0);
    CHECK(sameGeometry(*many.at(1.0), *a));  // 25600 cells: the 4-thread run really is threaded
    auto serial = plain.at(1.0);
    CHECK(!sameGeometry(*serial, *a));
    CHECK(std::fabs(densityCentreY(*a) - densityCentreY(*serial)) < 0.1f);
}

TEST(gasFieldBlowsAndKeepsTheVelocityFieldDivergenceFree) {
    Smoke blow;
    blow.net.node(blow.gas).setFloat("nozzleHeat", 0.0f);
    blow.net.node(blow.gas).setFloat("buoyancy", 0.0f);
    blow.net.node(blow.gas).setFloat("nozzleY", 0.0f);
    blow.net.node(blow.gas).setFloat("nozzleX", -3.0f);
    blow.net.node(blow.gas).setFloat("nozzleAngle", 0.0f);
    blow.net.node(blow.gas).setFloat("nozzleSpeed", 6.0f);
    blow.net.node(blow.gas).setFloat("iterations", 100.0f);
    auto g = blow.at(1.0);
    const AttribArray* vx = g->findAttrib(AttribClass::point, "vx");
    const AttribArray* vy = g->findAttrib(AttribClass::point, "vy");
    // The jet reaches across the box: positive vx on the axis downstream of the nozzle.
    const size_t row = 16, w = 32;
    CHECK(vx->at(row * w + 12) > 0.3f);

    // Mean divergence relative to the speed scale is small away from the walls.
    const float h = 8.0f / 32.0f;
    double div = 0.0, mag = 0.0;
    for (size_t j = 3; j < w - 3; ++j) {
        for (size_t i = 3; i < w - 3; ++i) {
            div += std::fabs((vx->at(j * w + i + 1) - vx->at(j * w + i - 1) + vy->at((j + 1) * w + i) - vy->at((j - 1) * w + i)) / (2.0f * h));
            mag += std::hypot(vx->at(j * w + i), vy->at(j * w + i)) / h;
        }
    }
    CHECK(mag > 0.0 && div < 0.15 * mag);

    // Closed walls: no flow through the left and right edge cells' outer faces (mirror), so the edge cell speed is below the interior's.
    float edge = 0.0f, inner = 0.0f;
    for (size_t j = 0; j < w; ++j) {
        edge = std::max(edge, std::fabs(vx->at(j * w + w - 1)));
        inner = std::max(inner, std::fabs(vx->at(j * w + w / 2)));
    }
    CHECK(edge < inner);
}

TEST(gasFieldInjectsFromSourcePointsAndWorksWithoutAnyInput) {
    Smoke s;
    s.net.node(s.gas).setFloat("nozzleDensity", 0.0f);
    s.net.node(s.gas).setFloat("nozzleHeat", 0.0f);
    CHECK(cookThrows(s.net, s.gas) == false);
    NodeId src = s.net.addNode("generate");
    s.net.node(src).setFloat("count", 1.0f);
    s.net.node(src).setFloat("cx", 2.0f);
    s.net.node(src).setFloat("cy", 2.0f);
    s.net.node(src).setFloat("width", 0.0f);
    s.net.node(src).setFloat("height", 0.0f);
    s.net.connect(src, s.gas);
    s.net.node(s.gas).setFloat("density", 10.0f);
    s.net.node(s.gas).setFloat("buoyancy", 0.0f);
    auto g = s.at(0.5);
    float best = 0.0f, bx = 0.0f, by = 0.0f;
    for (size_t i = 0; i < g->pointCount(); ++i) {
        const float d = g->findAttrib(AttribClass::point, "density")->at(i);
        if (d > best) {
            best = d;
            bx = g->x()[i];
            by = g->y()[i];
        }
    }
    CHECK(best > 0.1f);
    CHECK(std::hypot(bx - 2.0f, by - 2.0f) < 0.6f);
}

TEST(liveGasKeepsItsHistoryWhenTheSourcesMove) {
    auto densityNear = [](const Geometry& g, float cx, float cy) {
        const AttribArray* d = g.findAttrib(AttribClass::point, "density");
        float best = 0.0f;
        for (size_t i = 0; i < g.pointCount(); ++i)
            if (std::hypot(g.x()[i] - cx, g.y()[i] - cy) < 0.8f) best = std::max(best, d->at(i));
        return best;
    };
    auto run = [&](float live) {
        Smoke s;
        Node& gas = s.net.node(s.gas);
        gas.setFloat("nozzleDensity", 0.0f);
        gas.setFloat("nozzleHeat", 0.0f);
        gas.setFloat("buoyancy", 0.0f);
        gas.setFloat("density", 10.0f);
        gas.setFloat("live", live);
        NodeId src = s.net.addNode("generate");
        for (const auto& [name, value] : {std::pair<const char*, float>{"count", 1.0f}, {"cx", -2.0f}, {"cy", 0.0f}, {"width", 0.0f}, {"height", 0.0f}})
            s.net.node(src).setFloat(name, value);
        s.net.connect(src, s.gas);
        s.at(0.5);
        s.net.node(src).setFloat("cx", 2.0f);
        auto g = s.at(0.6);
        return std::pair<float, float>{densityNear(*g, -2.0f, 0.0f), densityNear(*g, 2.0f, 0.0f)};
    };
    const auto live = run(1.0f);
    CHECK(live.first > 0.1f && live.second > 0.1f);
    const auto replayed = run(0.0f);
    CHECK(replayed.first < 0.01f && replayed.second > 0.1f);
}

TEST(liveParticlesKeepTheirRunWhenTheNozzleChanges) {
    auto run = [](float live) {
        Network net(1);
        NodeId gen = net.addNode("generate");
        net.node(gen).setFloat("count", 200.0f);
        net.node(gen).setFloat("cx", -2.0f);
        net.node(gen).setFloat("cy", 0.0f);
        net.node(gen).setFloat("width", 1.0f);
        net.node(gen).setFloat("height", 1.0f);
        NodeId sim = net.addNode("particles");
        net.connect(gen, sim);
        Node& n = net.node(sim);
        for (const auto& [name, value] : {std::pair<const char*, float>{"gravityY", 0.0f}, {"drag", 0.0f}, {"gas", 1.0f}, {"live", live}, {"gasNozzleX", -3.5f},
                                          {"gasNozzleAngle", 0.0f}, {"gasNozzleSpeed", 6.0f}, {"gasNozzleRadius", 1.5f}, {"gasResolution", 32.0f},
                                          {"gasInfluence", 4.0f}, {"boundsMinX", -4.0f}, {"boundsMaxX", 8.0f}, {"boundsMinY", -4.0f}, {"boundsMaxY", 4.0f}})
            n.setFloat(name, value);
        net.setTime(0.5);
        net.cook(sim);
        n.setFloat("gasNozzleSpeed", 0.0f);
        net.setTime(0.6);
        auto g = net.cook(sim);
        double sum = 0.0;
        for (size_t i = 0; i < g->pointCount(); ++i) sum += g->findAttrib(AttribClass::point, "vx")->at(i);
        return static_cast<float>(sum / static_cast<double>(g->pointCount()));
    };
    CHECK(run(1.0f) > 0.3f);
    CHECK(std::fabs(run(0.0f)) < 1e-3f);
}

TEST(particlesAreBlownByGasAndCanStirIt) {
    auto make = [](size_t threads, float gasOn) {
        auto net = std::make_unique<Network>(threads);
        NodeId gen = net->addNode("generate");
        net->node(gen).setFloat("count", 200.0f);
        net->node(gen).setFloat("cx", -2.0f);
        net->node(gen).setFloat("cy", 0.0f);
        net->node(gen).setFloat("width", 1.0f);
        net->node(gen).setFloat("height", 1.0f);
        NodeId sim = net->addNode("particles");
        net->connect(gen, sim);
        Node& n = net->node(sim);
        n.setFloat("gravityY", 0.0f);
        n.setFloat("drag", 0.0f);
        n.setFloat("gas", gasOn);
        n.setFloat("gasNozzleX", -3.5f);
        n.setFloat("gasNozzleAngle", 0.0f);
        n.setFloat("gasNozzleSpeed", 6.0f);
        n.setFloat("gasNozzleRadius", 1.5f);
        n.setFloat("gasResolution", 32.0f);
        n.setFloat("gasInfluence", 4.0f);
        n.setFloat("boundsMinX", -4.0f);
        n.setFloat("boundsMaxX", 8.0f);
        n.setFloat("boundsMinY", -4.0f);
        n.setFloat("boundsMaxY", 4.0f);
        return std::pair{std::move(net), sim};
    };
    auto meanVx = [](const Geometry& g) {
        double sum = 0.0;
        for (size_t i = 0; i < g.pointCount(); ++i) sum += g.findAttrib(AttribClass::point, "vx")->at(i);
        return static_cast<float>(sum / static_cast<double>(g.pointCount()));
    };

    auto [off, offSim] = make(1, 0.0f);
    off->setTime(1.0);
    CHECK(std::fabs(meanVx(*off->cook(offSim))) < 1e-4f);
    CHECK(off->cook(offSim)->findAttrib(AttribClass::point, "gas") == nullptr);

    auto [on, onSim] = make(1, 1.0f);
    on->setTime(1.0);
    auto blown = on->cook(onSim);
    CHECK(meanVx(*blown) > 0.5f);
    CHECK(blown->findAttrib(AttribClass::point, "gas") != nullptr);

    auto [many, manySim] = make(4, 1.0f);
    many->setTime(1.0);
    CHECK(sameGeometry(*blown, *many->cook(manySim)));

    // Particles that emit and push put smoke into the gas, which shows in their `gas` attribute.
    on->node(onSim).setFloat("gasNozzleSpeed", 0.0f);
    on->node(onSim).setFloat("gasEmit", 5.0f);
    float total = 0.0f;
    for (size_t i = 0; i < 200; ++i) total += on->cook(onSim)->findAttrib(AttribClass::point, "gas")->at(i);
    CHECK(total > 1.0f);

    on->node(onSim).setFloat("gasResolution", 1.0f);
    CHECK(cookThrows(*on, onSim));
}

TEST(growSplitsLongSegmentsAndPushesNodesApart) {
    Network net(1);
    NodeId circle = net.addNode("circle");
    net.node(circle).setFloat("count", 8.0f);
    net.node(circle).setFloat("radius", 0.3f);
    NodeId grow = net.addNode("grow");
    net.connect(circle, grow);
    Node& n = net.node(grow);
    n.setFloat("attract", 0.0f);
    n.setFloat("smooth", 0.0f);
    n.setFloat("maxLength", 100.0f);
    n.setFloat("repelRadius", 2.0f);
    CHECK(net.dependsOnTime(grow));
    auto meanRadius = [](const Geometry& g) {
        double sum = 0.0;
        for (size_t i = 0; i < g.pointCount(); ++i) sum += std::hypot(g.x()[i], g.y()[i]);
        return sum / static_cast<double>(g.pointCount());
    };

    auto start = net.cook(grow);
    CHECK(start->pointCount() == 8 && start->primitiveCount() == 1 && start->primitives()[0].closed);
    net.setTime(0.5);
    auto pushed = net.cook(grow);
    CHECK(pushed->pointCount() == 8);
    CHECK(meanRadius(*pushed) > 0.3 + 0.05);

    // Without any force and with a huge maxLength nothing moves.
    n.setFloat("repel", 0.0f);
    auto still = net.cook(grow);
    for (size_t i = 0; i < still->pointCount(); ++i) CHECK_NEAR(still->x()[i], start->x()[i]);

    // Long segments are split, the new points are younger, and the bounds hold.
    n.setFloat("repel", 6.0f);
    n.setFloat("attract", 8.0f);
    n.setFloat("smooth", 4.0f);
    n.setFloat("maxLength", 0.2f);
    n.setFloat("repelRadius", 0.5f);
    n.setFloat("useBounds", 1.0f);
    n.setFloat("boundsMinX", -1.0f);
    n.setFloat("boundsMaxX", 1.0f);
    n.setFloat("boundsMinY", -1.0f);
    n.setFloat("boundsMaxY", 1.0f);
    net.setTime(3.0);
    auto grown = net.cook(grow);
    CHECK(grown->pointCount() > 20);
    float youngest = 1e9f;
    for (size_t i = 0; i < grown->pointCount(); ++i) {
        CHECK(grown->x()[i] >= -1.0f && grown->x()[i] <= 1.0f && grown->y()[i] >= -1.0f && grown->y()[i] <= 1.0f);
        youngest = std::min(youngest, grown->findAttrib(AttribClass::point, "age")->at(i));
    }
    CHECK(youngest < 2.9f);
    n.setFloat("maxPoints", 20.0f);
    CHECK(net.cook(grow)->pointCount() <= 20);
    n.setFloat("maxPoints", 20000.0f);

    // Scrubbing back and forth, a fresh network and more threads all give the same curves.
    net.setTime(1.0);
    auto early = net.cook(grow);
    net.setTime(3.0);
    CHECK(sameGeometry(*grown, *net.cook(grow)));
    net.setTime(1.0);
    CHECK(sameGeometry(*early, *net.cook(grow)));
    Network other(4);
    NodeId circle2 = other.addNode("circle");
    other.node(circle2).setFloat("count", 8.0f);
    other.node(circle2).setFloat("radius", 0.3f);
    NodeId grow2 = other.addNode("grow");
    other.connect(circle2, grow2);
    for (const std::string& name : n.floatNames()) other.node(grow2).setFloat(name, n.getFloat(name));
    other.setTime(1.0);
    CHECK(sameGeometry(*early, *other.cook(grow2)));

    NodeId empty = net.addNode("generate");
    NodeId bad = net.addNode("grow");
    net.connect(empty, bad);
    CHECK(cookThrows(net, bad));
}

TEST(fluidStaysAtRestSettlesInTheBoxAndIsDeterministic) {
    auto build = [](Network& net, NodeId& fluid) {
        NodeId merge = net.addNode("merge");
        for (int row = 0; row < 8; ++row) {
            NodeId line = net.addNode("line");
            net.node(line).setFloat("count", 20.0f);
            net.node(line).setFloat("length", 1.9f);
            net.node(line).setFloat("y", 0.1f * static_cast<float>(row));
            net.connect(line, merge, row);
        }
        fluid = net.addNode("fluid");
        net.connect(merge, fluid);
        Node& n = net.node(fluid);
        n.setFloat("boundsMinX", 0.0f);
        n.setFloat("boundsMaxX", 2.0f);
        n.setFloat("boundsMinY", -1.0f);
        n.setFloat("boundsMaxY", 3.0f);
        return merge;
    };
    Network net(1);
    NodeId fluid;
    NodeId source = build(net, fluid);
    Node& n = net.node(fluid);
    CHECK(net.dependsOnTime(fluid));

    // A lattice of the right spacing is at rest density: no gravity, no motion.
    n.setFloat("gravityY", 0.0f);
    auto start = net.cook(fluid);
    CHECK(start->pointCount() == 160);
    net.setTime(0.5);
    auto still = net.cook(fluid);
    for (size_t i = 0; i < still->pointCount(); ++i) {
        CHECK(std::fabs(still->x()[i] - start->x()[i]) < 0.02f);
        CHECK(std::fabs(still->y()[i] - start->y()[i]) < 0.02f);
    }

    // Under gravity the block falls into the floor and settles with roughly rest density, inside the bounds.
    n.setFloat("gravityY", -9.8f);
    net.setTime(2.0);
    auto settled = net.cook(fluid);
    const AttribArray* density = settled->findAttrib(AttribClass::point, "density");
    double meanDensity = 0.0, meanY = 0.0;
    float maxY = -1e9f;
    for (size_t i = 0; i < settled->pointCount(); ++i) {
        CHECK(settled->x()[i] >= 0.0f && settled->x()[i] <= 2.0f && settled->y()[i] >= -1.0f && settled->y()[i] <= 3.0f);
        meanDensity += density->at(i);
        meanY += settled->y()[i];
        maxY = std::max(maxY, settled->y()[i]);
    }
    meanDensity /= static_cast<double>(settled->pointCount());
    meanY /= static_cast<double>(settled->pointCount());
    CHECK(meanY < -0.4 && maxY < 0.1f);
    CHECK(meanDensity > 0.7 && meanDensity < 1.5);

    // Scrubbing, more threads and a changed signature all reproduce the same particles.
    net.setTime(1.0);
    auto early = net.cook(fluid);
    net.setTime(2.0);
    CHECK(sameGeometry(*settled, *net.cook(fluid)));
    Network other(4);
    NodeId fluid2;
    build(other, fluid2);
    for (const std::string& name : n.floatNames()) other.node(fluid2).setFloat(name, n.getFloat(name));
    other.setTime(1.0);
    CHECK(sameGeometry(*early, *other.cook(fluid2)));
    n.setFloat("vorticity", 5.0f);
    n.setFloat("tension", 0.05f);
    auto swirled = net.cook(fluid);
    for (size_t i = 0; i < swirled->pointCount(); ++i) CHECK(std::isfinite(swirled->x()[i]) && std::isfinite(swirled->y()[i]));

    n.setFloat("spacing", 0.0f);
    CHECK(cookThrows(net, fluid));
    n.setFloat("spacing", 0.1f);
    NodeId empty = net.addNode("generate");
    net.node(empty).setFloat("count", 0.0f);
    NodeId bad = net.addNode("fluid");
    net.connect(empty, bad);
    CHECK(cookThrows(net, bad));
    (void)source;
}

TEST(colonizeGrowsBranchesToEveryAttractor) {
    Network net(1);
    NodeId ring = net.addNode("circle");
    net.node(ring).setFloat("count", 40.0f);
    net.node(ring).setFloat("radius", 3.0f);
    NodeId root = net.addNode("generate");
    net.node(root).setFloat("count", 1.0f);
    net.node(root).setFloat("width", 0.0f);
    net.node(root).setFloat("height", 0.0f);
    NodeId tree = net.addNode("colonize");
    net.connect(ring, tree, 0);
    net.connect(root, tree, 1);
    Node& n = net.node(tree);
    n.setFloat("influence", 6.0f);
    auto grown = net.cook(tree);
    CHECK(grown->pointCount() > 40 && grown->primitiveCount() >= 2);
    for (const Primitive& p : grown->primitives()) CHECK(p.count >= 2 && !p.closed);
    const Geometry& attractors = *net.cook(ring);
    for (size_t a = 0; a < attractors.pointCount(); ++a) {
        float nearest = 1e9f;
        for (size_t i = 0; i < grown->pointCount(); ++i)
            nearest = std::min(nearest, std::hypot(grown->x()[i] - attractors.x()[a], grown->y()[i] - attractors.y()[a]));
        CHECK(nearest < 0.3f + 1e-3f);
    }
    int32_t maxDepth = 0, rootSize = 0;
    for (size_t i = 0; i < grown->pointCount(); ++i) {
        maxDepth = std::max(maxDepth, grown->findAttrib(AttribClass::point, "depth")->ints()[i]);
        if (grown->findAttrib(AttribClass::point, "depth")->ints()[i] == 0) rootSize = grown->findAttrib(AttribClass::point, "size")->ints()[i];
    }
    CHECK(maxDepth >= 15);
    CHECK(rootSize > 40);
    CHECK(sameGeometry(*grown, *net.cook(tree)));

    n.setFloat("maxNodes", 10.0f);
    CHECK(net.cook(tree)->pointCount() <= 20);
    n.setFloat("maxNodes", 200000.0f);
    n.setFloat("killDistance", 7.0f);
    CHECK(cookThrows(net, tree));
    n.setFloat("killDistance", 0.3f);
    NodeId none = net.addNode("generate");
    net.node(none).setFloat("count", 0.0f);
    NodeId bad = net.addNode("colonize");
    net.connect(ring, bad, 0);
    net.connect(none, bad, 1);
    CHECK(cookThrows(net, bad));
}

TEST(streamlinesFollowTheFieldKeepTheirSpacingAndReadAGasGrid) {
    auto seeds = [](Network& net, float count, float width, float height) {
        NodeId gen = net.addNode("generate");
        net.node(gen).setFloat("count", count);
        net.node(gen).setFloat("width", width);
        net.node(gen).setFloat("height", height);
        return gen;
    };
    Network net(1);
    NodeId one = seeds(net, 1.0f, 0.0f, 0.0f);
    NodeId lines = net.addNode("streamlines");
    net.connect(one, lines);
    Node& n = net.node(lines);
    // The angle field with no turns points along +x: straight lines.
    n.setString("field", "angle");
    n.setFloat("turns", 0.0f);
    n.setFloat("stepSize", 0.1f);
    n.setFloat("maxSteps", 10.0f);
    n.setString("direction", "forward");
    auto forward = net.cook(lines);
    CHECK(forward->pointCount() == 11 && forward->primitiveCount() == 1);
    CHECK_NEAR(forward->x()[10], 1.0f);
    CHECK_NEAR(forward->y()[10], 0.0f);
    CHECK_NEAR(forward->findAttrib(AttribClass::point, "u")->at(10), 1.0f);
    n.setString("direction", "both");
    auto both = net.cook(lines);
    CHECK(both->pointCount() == 21);
    CHECK_NEAR(both->x()[0], -1.0f);
    CHECK_NEAR(both->x()[10], 0.0f);
    n.setFloat("useBounds", 1.0f);
    n.setFloat("boundsMinX", -0.35f);
    n.setFloat("boundsMaxX", 0.55f);
    auto clipped = net.cook(lines);
    CHECK(clipped->pointCount() == 9);
    n.setFloat("useBounds", 0.0f);

    // Many seeds: without spacing every seed gives a line, with it the lines keep their distance.
    Network crowd(1);
    NodeId many = seeds(crowd, 400.0f, 6.0f, 6.0f);
    NodeId cl = crowd.addNode("streamlines");
    crowd.connect(many, cl);
    Node& c = crowd.node(cl);
    c.setFloat("maxSteps", 60.0f);
    auto free = crowd.cook(cl);
    CHECK(free->primitiveCount() == 400);
    c.setFloat("separation", 0.4f);
    auto spaced = crowd.cook(cl);
    CHECK(spaced->primitiveCount() > 5 && spaced->primitiveCount() < 400);
    const AttribArray* lineOf = spaced->findAttrib(AttribClass::point, "line");
    float closest = 1e9f;
    for (size_t i = 0; i < spaced->pointCount(); ++i) {
        for (size_t j = i + 1; j < spaced->pointCount(); ++j) {
            if (lineOf->ints()[i] == lineOf->ints()[j]) continue;
            closest = std::min(closest, std::hypot(spaced->x()[i] - spaced->x()[j], spaced->y()[i] - spaced->y()[j]));
        }
    }
    CHECK(closest >= 0.4f * 0.5f * 0.99f);

    // A gas grid is a field: the nozzle blows along +x, so a line seeded in it runs along +x.
    Network sim(1);
    NodeId gas = sim.addNode("gasField");
    Node& g = sim.node(gas);
    g.setFloat("nozzleX", 0.0f);
    g.setFloat("nozzleY", 0.0f);
    g.setFloat("nozzleAngle", 0.0f);
    g.setFloat("nozzleSpeed", 6.0f);
    g.setFloat("nozzleDensity", 0.0f);
    g.setFloat("nozzleHeat", 0.0f);
    g.setFloat("buoyancy", 0.0f);
    g.setFloat("boundsMinX", -4.0f);
    g.setFloat("boundsMaxX", 4.0f);
    g.setFloat("boundsMinY", -4.0f);
    g.setFloat("boundsMaxY", 4.0f);
    g.setFloat("resolution", 32.0f);
    NodeId seed = seeds(sim, 1.0f, 0.0f, 0.0f);
    NodeId sl = sim.addNode("streamlines");
    sim.connect(seed, sl, 0);
    sim.connect(gas, sl, 1);
    sim.node(sl).setString("direction", "forward");
    sim.node(sl).setFloat("maxSteps", 30.0f);
    sim.setTime(0.5);
    auto flow = sim.cook(sl);
    CHECK(flow->primitiveCount() == 1 && flow->pointCount() > 5);
    CHECK(flow->x()[flow->pointCount() - 1] > 0.5f);
    CHECK(std::fabs(flow->y()[flow->pointCount() - 1]) < 0.5f * flow->x()[flow->pointCount() - 1]);

    n.setString("field", "nonesuch");
    CHECK(cookThrows(net, lines));
    n.setString("field", "curl");
    n.setFloat("stepSize", 0.0f);
    CHECK(cookThrows(net, lines));
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

namespace {

struct MovingRock {
    Network net{2};
    NodeId gen, rock, move, sized, sim;

    explicit MovingRock(const char* obstacleTime) {
        gen = net.addNode("generate");
        net.node(gen).setFloat("count", 300.0f);
        net.node(gen).setFloat("width", 10.0f);
        net.node(gen).setFloat("height", 10.0f);
        rock = net.addNode("generate");
        net.node(rock).setFloat("count", 1.0f);
        net.node(rock).setFloat("width", 0.0f);
        net.node(rock).setFloat("height", 0.0f);
        move = net.addNode("transform");
        net.connect(rock, move);
        net.node(move).setExpression("tx", "1.5 * time");
        sized = net.addNode("wrangle");
        net.connect(move, sized);
        net.node(sized).setString("code", "radius = 1.5;");
        sim = net.addNode("boids");
        net.connect(gen, sim);
        net.connect(sized, sim, 1);
        net.node(sim).setFloat("radius", 1.5f);
        for (const char* p : {"boundsMinX", "boundsMinY"}) net.node(sim).setFloat(p, -12.0f);
        for (const char* p : {"boundsMaxX", "boundsMaxY"}) net.node(sim).setFloat(p, 12.0f);
        net.node(sim).setString("obstacleTime", obstacleTime);
    }
};

}  // namespace

TEST(boidsSampledObstaclesFollowAMovingInputAsAPureFunctionOfTime) {
    MovingRock stepped("sampled");
    std::vector<GeometryHandle> walked;
    for (double t : {0.5, 1.0, 2.0, 3.0}) {
        stepped.net.setTime(t);
        const GeometryHandle g = stepped.net.cook(stepped.sim);
        const float cx = 1.5f * static_cast<float>(t);
        for (size_t i = 0; i < g->pointCount(); ++i) CHECK(std::hypot(g->x()[i] - cx, g->y()[i]) >= 1.5f - 0.01f);
        walked.push_back(g);
    }

    MovingRock jump("sampled");
    jump.net.setTime(3.0);
    CHECK(sameGeometry(*walked.back(), *jump.net.cook(jump.sim)));

    // Going back in time and forward again gives the same picture.
    stepped.net.setTime(1.0);
    CHECK(sameGeometry(*walked[1], *stepped.net.cook(stepped.sim)));
    stepped.net.setTime(3.0);
    CHECK(sameGeometry(*walked.back(), *stepped.net.cook(stepped.sim)));

    // Editing anything upstream of the obstacle changes the run: a rock that moves faster pushes the flock elsewhere.
    stepped.net.node(stepped.move).setExpression("tx", "3 * time");
    CHECK(!sameGeometry(*walked.back(), *stepped.net.cook(stepped.sim)));

    // 'start' keeps its old meaning: the rock is where the input has it now, for the whole run.
    MovingRock frozen("start");
    frozen.net.setTime(3.0);
    CHECK(!sameGeometry(*walked.back(), *frozen.net.cook(frozen.sim)));

    MovingRock bad("sometimes");
    bad.net.setTime(1.0);
    bool threw = false;
    try {
        bad.net.cook(bad.sim);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
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
        if (net.node(node).usesValues()) continue;  // image ports: geometry cannot be wired in
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
                if (type == "attribString" || name == shapeKeyAttrib) continue;  // new text by design
                for (size_t i = 0; i < array.size(); ++i) CHECK(std::find(words.begin(), words.end(), array.stringAt(i)) != words.end());
            }
        }
    }
    CHECK(cooked > 20);
    CHECK(kept > 5);
}

namespace {

bool wrangleThrows(const std::string& code, const Geometry& input) {
    Network net(1);
    net.setExternalInputs({std::make_shared<Geometry>(input)});
    const NodeId in = net.addNode("subnetInput", "in");
    const NodeId w = net.addNode("wrangle", "w");
    net.node(w).setString("code", code);
    net.connect(in, w);
    try {
        net.cook(w);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

}  // namespace

TEST(wrangleDeclaredParametersWorkInFusedChains) {
    for (bool jit : {true, false}) {
        Network net(2);
        net.setFusionEnabled(true);
        net.compiler().setJitEnabled(jit);
        net.setExternalInputs({std::make_shared<Geometry>(labelled({"q", "r", "q", "q", "r"}, {"q", "r"}))});
        const NodeId in = net.addNode("subnetInput", "in");
        const NodeId w1 = net.addNode("wrangle", "w1");
        const NodeId w2 = net.addNode("wrangle", "w2");
        net.node(w1).setString("code", "| gain = 2 |\nx = x * gain;");
        net.node(w2).setString("code", "| off = 10, gain = 3 |\nok = label == \"q\"; x = x * gain + off + a;");
        net.node(w2).setFloat("a", 100.0f);
        net.connect(in, w1);
        net.connect(w1, w2);
        net.setDisplayNode(w2);
        GeometryHandle g = net.cookDisplay();
        CHECK(net.fusedChainCount() >= 1);
        CHECK(g->findAttrib(AttribClass::point, "x")->at(2) == 2.0f * 2.0f * 3.0f + 10.0f + 100.0f);
        CHECK(g->findAttrib(AttribClass::point, "ok")->at(1) == 0.0f && g->findAttrib(AttribClass::point, "ok")->at(2) == 1.0f);
        net.node(w1).setFloat("gain", 1.0f);
        net.node(w2).setFloat("gain", 2.0f);
        g = net.cookDisplay();
        CHECK(g->findAttrib(AttribClass::point, "x")->at(2) == 2.0f * 1.0f * 2.0f + 10.0f + 100.0f);
    }
}

TEST(wrangleComparesStringAttributesWithLiterals) {
    const Geometry input = labelled({"tree", "rock", "", "tree", "bush", "rock"}, {"rock", "tree"});
    const std::vector<float> hit = {1, 0, 0, 1, 0, 0}, both = {0, 0, 1, 1, 1, 0}, rocks = {0, 1, 0, 0, 0, 1}, empty = {0, 0, 1, 0, 0, 0};
    for (bool jit : {true, false}) {
        for (bool fusion : {true, false}) {
            Network net(2);
            net.setFusionEnabled(fusion);
            net.compiler().setJitEnabled(jit);
            net.setExternalInputs({std::make_shared<Geometry>(input)});
            const NodeId in = net.addNode("subnetInput", "in");
            const NodeId w1 = net.addNode("wrangle", "w1");
            const NodeId w2 = net.addNode("wrangle", "w2");
            const NodeId out = net.addNode("null", "out");
            net.node(w1).setString("code", "hit = label == \"tree\"; both = \"rock\" != label && x > 1;");
            net.node(w2).setString("code", "rocks = label == \"rock\"; miss = label == \"nothing\"; gone = label != \"nothing\"; blank = label == \"\"; same = hit;");
            net.connect(in, w1);
            net.connect(w1, w2);
            net.connect(w2, out);
            net.setDisplayNode(out);
            const GeometryHandle g = net.cookDisplay();
            if (fusion) CHECK(net.fusedChainCount() == 1);
            for (size_t i = 0; i < 6; ++i) {
                CHECK(g->findAttrib(AttribClass::point, "hit")->at(i) == hit[i]);
                CHECK(g->findAttrib(AttribClass::point, "both")->at(i) == both[i]);
                CHECK(g->findAttrib(AttribClass::point, "rocks")->at(i) == rocks[i]);
                CHECK(g->findAttrib(AttribClass::point, "blank")->at(i) == empty[i]);
                CHECK(g->findAttrib(AttribClass::point, "miss")->at(i) == 0.0f);
                CHECK(g->findAttrib(AttribClass::point, "gone")->at(i) == 1.0f);
                CHECK(g->findAttrib(AttribClass::point, "same")->at(i) == hit[i]);
            }
            g->checkStrings();
            CHECK(g->findAttrib(AttribClass::point, "label")->stringAt(4) == "bush");
        }
    }

    // The kernel is shared by different texts; each geometry resolves its own ids.
    Network net(1);
    net.setExternalInputs({std::make_shared<Geometry>(input)});
    const NodeId in = net.addNode("subnetInput", "in");
    const NodeId w = net.addNode("wrangle", "w");
    net.node(w).setString("code", "hit = label == \"tree\";");
    net.connect(in, w);
    net.cook(w);
    const size_t compiled = net.compiler().compileCount();
    net.node(w).setString("code", "hit = label == \"rock\";");
    const GeometryHandle again = net.cook(w);
    CHECK(net.compiler().compileCount() == compiled);
    CHECK(again->findAttrib(AttribClass::point, "hit")->at(1) == 1.0f && again->findAttrib(AttribClass::point, "hit")->at(0) == 0.0f);

    CHECK(wrangleThrows("hit = label + \"a\";", input));
    CHECK(wrangleThrows("hit = x == \"a\";", input));
    CHECK(wrangleThrows("hit = (label + 1) == \"a\";", input));
    CHECK(wrangleThrows("hit = label < \"a\";", input));
    CHECK(wrangleThrows("hit = label == \"unterminated;", input));
    CHECK(wrangleThrows("label = 1;", input));
    CHECK(!wrangleThrows("hit = label == \"quote\\\"d\";", input));
}

namespace {

Geometry stringSample() {
    Geometry g = sampleGeometry();
    AttribArray& label = g.attrib(AttribClass::point, "label", AttribType::string);
    label.intern("unused");
    const char* words[] = {"tree", "caf\xc3\xa9", "quote\"back\\slash\n", "", "\xf0\x9f\x8c\xb3"};
    for (size_t i = 0; i < 37; ++i) label.ints()[i] = label.intern(words[i % 5]);
    AttribArray& cls = g.attrib(AttribClass::primitive, "class", AttribType::string);
    cls.ints()[0] = cls.intern("road");
    cls.ints()[1] = cls.intern("river");
    return g;
}

bool sameTables(const Geometry& a, const Geometry& b) {
    for (AttribClass cls : {AttribClass::point, AttribClass::primitive}) {
        for (const auto& [name, array] : a.attribs(cls)) {
            if (array.type() != AttribType::string) continue;
            const AttribArray* other = b.findAttrib(cls, name);
            if (other == nullptr || other->table().strings != array.table().strings) return false;
        }
    }
    return true;
}

size_t sectionAt(const std::vector<uint8_t>& bytes, uint32_t id) {
    uint64_t offset;
    std::memcpy(&offset, bytes.data() + 64 + 24 * (id - 1) + 8, 8);
    return static_cast<size_t>(offset);
}

}  // namespace

TEST(geoFileKeepsStringAttributesWithTheirTables) {
    const Geometry g = stringSample();
    const std::vector<uint8_t> bytes = saveGeometry(g);
    CHECK(bytes.size() % 64 == 0);
    const Geometry back = loadGeometry(bytes.data(), bytes.size());
    back.checkStrings();
    CHECK(sameBits(g, back));
    CHECK(sameTables(g, back));
    CHECK(saveGeometry(back) == bytes);
    CHECK(back.findAttrib(AttribClass::point, "label")->stringAt(2) == "quote\"back\\slash\n");
    CHECK(back.findAttrib(AttribClass::primitive, "class")->stringAt(1) == "river");

    // A file without string attributes has the same layout as 1.0 except the minor version.
    const std::vector<uint8_t> plain = saveGeometry(sampleGeometry());
    CHECK(plain[20] == 6 && bytes[20] == 7);
    CHECK(plain[8] == 1 && plain[10] == 1);
    const Geometry none = labelled({}, {});
    const std::vector<uint8_t> noneBytes = saveGeometry(none);
    CHECK(sameBits(none, loadGeometry(noneBytes.data(), noneBytes.size())));

    // Merged geometries save and load like any other: the table is the merged one.
    const Geometry first = labelled({"a", "b"}, {}), second = labelled({"b", "c"}, {"c"});
    const Geometry merged = Geometry::concat({&first, &second}, nullptr);
    const std::vector<uint8_t> mergedBytes = saveGeometry(merged);
    CHECK(labelsOf(loadGeometry(mergedBytes.data(), mergedBytes.size())) == labelsOf(merged));
}

TEST(geoFileRejectsStringDataThatDoesNotFitItsTable) {
    const Geometry small = labelled({"a", "b", "a"}, {});
    const std::vector<uint8_t> bytes = saveGeometry(small);
    const size_t tables = sectionAt(bytes, 7), payload = sectionAt(bytes, 6);
    CHECK(!loadFails(bytes, bytes.size()));
    const auto patched = [&](size_t at, std::vector<uint8_t> values) {
        std::vector<uint8_t> mutated = bytes;
        for (size_t i = 0; i < values.size(); ++i) mutated[at + i] = values[i];
        refreshChecksum(mutated);
        return loadFails(mutated, mutated.size());
    };
    // Layout: u64 table count, then u32 attribute, u32 count, u32 offsets[4], text "ab".
    CHECK(patched(payload, {3, 0, 0, 0}));
    CHECK(patched(payload, {0xff, 0xff, 0xff, 0xff}));
    CHECK(patched(payload + 8, {0xff, 0xff, 0xff, 0xff}));
    CHECK(!patched(payload, {2, 0, 0, 0}));
    CHECK(patched(tables, {0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(patched(tables, {2, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(patched(tables + 8, {1, 0, 0, 0}));
    CHECK(patched(tables + 8, {9, 0, 0, 0}));
    CHECK(patched(tables + 12, {0, 0, 0, 0}));
    CHECK(patched(tables + 12, {0xff, 0xff, 0xff, 0x7f}));
    CHECK(patched(tables + 12, {0xff, 0xff, 0xff, 0xff}));
    CHECK(patched(tables + 12, {4, 0, 0, 0}));
    CHECK(patched(tables + 20, {1, 0, 0, 0}));
    CHECK(patched(tables + 32, {0xff}));
    CHECK(patched(tables + 32, {0xc3}));
    CHECK(patched(tables + 33, {'a'}));
    CHECK(!patched(tables + 33, {'c'}));
    CHECK(!patched(tables + 32, {0}));
}

TEST(geoFileRejectsEveryTruncationAndByteFlipWithStrings) {
    const std::vector<uint8_t> bytes = saveGeometry(stringSample());
    for (size_t length = 0; length < bytes.size(); ++length) CHECK(loadFails(bytes, length));
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i == 8 || i == 9) continue;
        std::vector<uint8_t> mutated = bytes;
        mutated[i] ^= 0x5a;
        CHECK(loadFails(mutated, mutated.size()));
    }
}

TEST(geoFileWithStringsSurvivesMutationsWithAValidChecksum) {
    const std::vector<uint8_t> bytes = saveGeometry(stringSample());
    size_t rejected = 0;
    for (size_t i = 64; i < bytes.size(); ++i) {
        for (uint8_t value : {uint8_t(0x00), uint8_t(0x01), uint8_t(0x7f), uint8_t(0x80), uint8_t(0xff)}) {
            std::vector<uint8_t> mutated = bytes;
            if (mutated[i] == value) continue;
            mutated[i] = value;
            refreshChecksum(mutated);
            try {
                const Geometry g = loadGeometry(mutated.data(), mutated.size());
                g.checkStrings();
                for (AttribClass cls : {AttribClass::point, AttribClass::primitive})
                    for (const auto& [name, array] : g.attribs(cls)) CHECK(array.size() == g.count(cls));
            } catch (const std::runtime_error&) {
                ++rejected;
            }
        }
    }
    CHECK(rejected > 1000);
}

TEST(stringTextThatIsNotUtf8IsNotSaved) {
    Geometry g = labelled({"ok"}, {});
    g.attrib(AttribClass::point, "label", AttribType::string).intern("bad\xff");
    bool threw = false;
    try {
        saveGeometry(g);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

namespace {

GeometryHandle cookNodeOn(const std::string& type, const std::vector<std::pair<std::string, std::string>>& params, const Geometry& input) {
    Network net(1);
    net.setExternalInputs({std::make_shared<Geometry>(input)});
    const NodeId in = net.addNode("subnetInput", "in");
    const NodeId node = net.addNode(type, "under");
    for (const auto& [name, value] : params) net.node(node).setString(name, value);
    net.connect(in, node);
    return net.cook(node);
}

std::vector<std::string> textsOf(const Geometry& g, AttribClass cls, const std::string& name) {
    std::vector<std::string> out;
    const AttribArray* a = g.findAttrib(cls, name);
    for (size_t i = 0; i < g.count(cls); ++i) out.push_back(a->stringAt(i));
    return out;
}

std::vector<int> membersOf(const Geometry& g, AttribClass cls, const std::string& group) {
    std::vector<int> out;
    for (size_t i = 0; i < g.count(cls); ++i) out.push_back(g.inGroup(cls, group, i) ? 1 : 0);
    return out;
}

}  // namespace

TEST(stringNodesCreateSetAndGroupAttributes) {
    Geometry input = labelled({"tree", "rock", "treehouse", "", "a tree"}, {});
    input.attrib(AttribClass::point, "kind", AttribType::int32);
    const int kinds[] = {0, 1, 2, 5, -1};
    for (size_t i = 0; i < 5; ++i) input.attrib(AttribClass::point, "kind", AttribType::int32).ints()[i] = kinds[i];
    input.createGroup(AttribClass::point, "top").ints()[1] = 1;
    input.createGroup(AttribClass::point, "top").ints()[4] = 1;
    input.addPrimitive({0, 3, false, PrimKind::polyline});
    input.addPrimitive({3, 2, false, PrimKind::polyline});

    const auto created = cookNodeOn("attribCreate", {{"name", "tag"}, {"type", "string"}, {"text", "wood"}}, input);
    created->checkStrings();
    CHECK(textsOf(*created, AttribClass::point, "tag") == std::vector<std::string>(5, "wood"));
    CHECK(created->findAttrib(AttribClass::point, "tag")->table().strings == std::vector<std::string>({"", "wood"}));

    const auto named = cookNodeOn("attribString", {{"name", "type"}, {"source", "ints"}, {"from", "kind"}, {"names", "rock, tree ,bush"}}, input);
    named->checkStrings();
    CHECK(textsOf(*named, AttribClass::point, "type") == std::vector<std::string>({"rock", "tree", "bush", "", ""}));

    const auto numbered = cookNodeOn("attribString", {{"name", "pt"}, {"source", "pattern"}, {"text", "id$P-$P"}}, input);
    CHECK(textsOf(*numbered, AttribClass::point, "pt") == std::vector<std::string>({"id0-0", "id1-1", "id2-2", "id3-3", "id4-4"}));

    const auto grouped = cookNodeOn("attribString", {{"name", "label"}, {"source", "group"}, {"group", "top"}, {"text", "top"}}, input);
    grouped->checkStrings();
    CHECK(textsOf(*grouped, AttribClass::point, "label") == std::vector<std::string>({"tree", "top", "treehouse", "", "top"}));

    const auto prims = cookNodeOn("attribString", {{"name", "class"}, {"class", "primitive"}, {"text", "road$P"}}, input);
    CHECK(textsOf(*prims, AttribClass::primitive, "class") == std::vector<std::string>({"road0", "road1"}));

    const auto equals = cookNodeOn("groupByString", {{"attrib", "label"}, {"text", "tree"}, {"group", "g"}}, input);
    CHECK(membersOf(*equals, AttribClass::point, "g") == std::vector<int>({1, 0, 0, 0, 0}));
    const auto starts = cookNodeOn("groupByString", {{"attrib", "label"}, {"match", "startsWith"}, {"text", "tree"}, {"group", "g"}}, input);
    CHECK(membersOf(*starts, AttribClass::point, "g") == std::vector<int>({1, 0, 1, 0, 0}));
    const auto contains = cookNodeOn("groupByString", {{"attrib", "label"}, {"match", "contains"}, {"text", "tree"}, {"group", "g"}}, input);
    CHECK(membersOf(*contains, AttribClass::point, "g") == std::vector<int>({1, 0, 1, 0, 1}));
    const auto blank = cookNodeOn("groupByString", {{"attrib", "label"}, {"text", ""}, {"group", "g"}}, input);
    CHECK(membersOf(*blank, AttribClass::point, "g") == std::vector<int>({0, 0, 0, 1, 0}));
    const auto blankStarts = cookNodeOn("groupByString", {{"attrib", "label"}, {"match", "startsWith"}, {"text", ""}, {"group", "g"}}, input);
    CHECK(membersOf(*blankStarts, AttribClass::point, "g") == std::vector<int>({1, 1, 1, 1, 1}));
    CHECK(blank->findAttrib(AttribClass::point, "label")->stringAt(2) == "treehouse");

    const auto missing = [&](const std::string& type, const std::vector<std::pair<std::string, std::string>>& params) {
        try {
            cookNodeOn(type, params, input);
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    CHECK(missing("groupByString", {{"attrib", "kind"}}));
    CHECK(missing("groupByString", {{"attrib", "nothing"}}));
    CHECK(missing("groupByString", {{"attrib", "label"}, {"match", "sounds like"}}));
    CHECK(missing("attribString", {{"name", "kind"}, {"source", "pattern"}}));
    CHECK(missing("attribString", {{"name", "t"}, {"source", "ints"}, {"from", "label"}}));
    CHECK(missing("attribString", {{"name", "t"}, {"source", "invented"}}));
    CHECK(missing("attribCreate", {{"name", "t"}, {"type", "text"}}));

    // The result feeds a wrangle comparison like any other string attribute.
    Network net(1);
    net.setExternalInputs({std::make_shared<Geometry>(input)});
    const NodeId in = net.addNode("subnetInput", "in");
    const NodeId named2 = net.addNode("attribString", "named");
    net.node(named2).setString("name", "type");
    net.node(named2).setString("source", "ints");
    net.node(named2).setString("from", "kind");
    net.node(named2).setString("names", "rock, tree, bush");
    const NodeId w = net.addNode("wrangle", "w");
    net.node(w).setString("code", "hit = type == \"tree\";");
    net.connect(in, named2);
    net.connect(named2, w);
    const GeometryHandle out = net.cook(w);
    CHECK(out->findAttrib(AttribClass::point, "hit")->at(1) == 1.0f && out->findAttrib(AttribClass::point, "hit")->at(0) == 0.0f);
}

namespace {

Geometry symmetrySample() {
    Geometry g;
    g.resizePoints(3);
    const float xs[] = {1, 3, 2}, ys[] = {2, 4, 5};
    for (size_t i = 0; i < 3; ++i) {
        g.x()[i] = xs[i];
        g.y()[i] = ys[i];
        g.attrib(AttribClass::point, "vx").floats()[i] = static_cast<float>(i) + 1.0f;
        g.attrib(AttribClass::point, "vy").floats()[i] = 10.0f;
        g.attrib(AttribClass::point, "N.x").floats()[i] = 1.0f;
        g.attrib(AttribClass::point, "N.y").floats()[i] = 0.0f;
        g.attrib(AttribClass::point, "w").floats()[i] = 7.0f + static_cast<float>(i);
    }
    g.addPrimitive({0, 3, true, PrimKind::polyline});
    g.attrib(AttribClass::primitive, "area").floats()[0] = 5.0f;
    AttribArray& label = g.attrib(AttribClass::primitive, "label", AttribType::string);
    label.ints()[0] = label.intern("tree");
    return g;
}

bool nearly(float a, float b) { return std::fabs(a - b) < 1e-4f; }

}  // namespace

TEST(symmetryMirrorsRotatesAndKeepsAttributes) {
    const Geometry in = symmetrySample();
    SymmetryOptions o;
    const Geometry m = symmetry(in, o);
    CHECK(m.pointCount() == 6 && m.primitiveCount() == 2);
    m.checkStrings();
    CHECK(nearly(m.x()[0], 1) && nearly(m.x()[3], -1) && nearly(m.y()[3], 2) && nearly(m.x()[4], -3));
    CHECK(m.primitives()[1].start == 3 && m.primitives()[1].count == 3 && m.primitives()[1].closed);
    const AttribArray* vx = m.findAttrib(AttribClass::point, "vx");
    CHECK(nearly(vx->at(1), 2) && nearly(vx->at(4), -2) && nearly(m.findAttrib(AttribClass::point, "vy")->at(4), 10));
    CHECK(nearly(m.findAttrib(AttribClass::point, "N.x")->at(3), 1));  // not listed in `vectors`, so unchanged
    CHECK(nearly(m.findAttrib(AttribClass::point, "w")->at(4), 8));
    CHECK(nearly(m.findAttrib(AttribClass::primitive, "area")->at(1), 5));
    CHECK(m.findAttrib(AttribClass::primitive, "label")->stringAt(1) == "tree");

    o.vectors = {"N"};
    const Geometry withNormals = symmetry(in, o);
    CHECK(nearly(withNormals.findAttrib(AttribClass::point, "N.x")->at(3), -1) && nearly(withNormals.findAttrib(AttribClass::point, "N.x")->at(0), 1));

    // The reflected copy alone, twice, is the input again.
    o.keepOriginal = false;
    const Geometry twice = symmetry(symmetry(in, o), o);
    CHECK(twice.pointCount() == 3);
    for (size_t i = 0; i < 3; ++i) CHECK(nearly(twice.x()[i], in.x()[i]) && nearly(twice.y()[i], in.y()[i]) && nearly(twice.findAttrib(AttribClass::point, "vx")->at(i), in.findAttrib(AttribClass::point, "vx")->at(i)));

    // Other axes: 0 degrees flips y, 45 swaps x and y, an origin moves the line.
    SymmetryOptions flat;
    flat.angle = 0.0f;
    flat.keepOriginal = false;
    CHECK(nearly(symmetry(in, flat).y()[0], -2) && nearly(symmetry(in, flat).x()[0], 1));
    flat.angle = 45.0f;
    CHECK(nearly(symmetry(in, flat).x()[1], 4) && nearly(symmetry(in, flat).y()[1], 3));
    SymmetryOptions shifted;
    shifted.originX = 2.0f;
    shifted.keepOriginal = false;
    CHECK(nearly(symmetry(in, shifted).x()[0], 3) && nearly(symmetry(in, shifted).x()[1], 1));

    SymmetryOptions turn;
    turn.mode = SymmetryMode::rotate;
    turn.folds = 4;
    const Geometry fourFold = symmetry(in, turn);
    CHECK(fourFold.pointCount() == 12 && fourFold.primitiveCount() == 4);
    CHECK(nearly(fourFold.x()[3], -2) && nearly(fourFold.y()[3], 1));  // (1,2) turned 90 degrees
    CHECK(nearly(fourFold.findAttrib(AttribClass::point, "vx")->at(3), -10) && nearly(fourFold.findAttrib(AttribClass::point, "vy")->at(3), 1));
    turn.keepOriginal = false;
    CHECK(symmetry(in, turn).pointCount() == 9);
    turn.folds = 1;
    CHECK(symmetry(in, turn).pointCount() == 0);

    SymmetryOptions kal;
    kal.mode = SymmetryMode::kaleidoscope;
    kal.folds = 3;
    const Geometry six = symmetry(in, kal);
    CHECK(six.pointCount() == 18 && six.primitiveCount() == 6);
    six.checkStrings();
    // Every copy keeps its distances to the origin, and the whole set maps onto itself when mirrored.
    for (size_t i = 0; i < 18; ++i) CHECK(nearly(std::hypot(six.x()[i], six.y()[i]), std::hypot(in.x()[i % 3], in.y()[i % 3])));
    const Geometry again = symmetry(six, SymmetryOptions{});
    std::multiset<std::pair<long, long>> seen;
    for (size_t i = 0; i < 18; ++i) seen.insert({std::lround(six.x()[i] * 1000), std::lround(six.y()[i] * 1000)});
    for (size_t i = 18; i < 36; ++i) CHECK(seen.count({std::lround(again.x()[i] * 1000), std::lround(again.y()[i] * 1000)}) > 0);

    CHECK(symmetry(Geometry(), SymmetryOptions{}).pointCount() == 0);
}

TEST(mirrorNodeReadsItsParametersAndRejectsABadMode) {
    Network net(1);
    net.setExternalInputs({std::make_shared<Geometry>(symmetrySample())});
    const NodeId in = net.addNode("subnetInput", "in");
    const NodeId node = net.addNode("mirror", "m");
    net.connect(in, node);
    CHECK(net.cook(node)->pointCount() == 6);
    net.node(node).setString("mode", "kaleidoscope");
    net.node(node).setFloat("folds", 5.0f);
    CHECK(net.cook(node)->pointCount() == 30);
    net.node(node).setString("mode", "rotate");
    net.node(node).setFloat("keepOriginal", 0.0f);
    CHECK(net.cook(node)->pointCount() == 12);
    net.node(node).setString("mode", "mirror");
    net.node(node).setString("vectors", " N , ");
    CHECK(nearly(net.cook(node)->findAttrib(AttribClass::point, "N.x")->at(0), -1.0f));  // keepOriginal is still off: only the reflected copy
    net.node(node).setString("mode", "fold");
    bool threw = false;
    try {
        net.cook(node);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(svgExportWritesStringAttributesAsIdAndClass) {
    Geometry g = labelled({"", "", "", "", "", ""}, {});
    g.addPrimitive({0, 2, false, PrimKind::polyline});
    g.addPrimitive({2, 2, true, PrimKind::polyline});
    g.addPrimitive({4, 2, false, PrimKind::polyline});
    AttribArray& id = g.attrib(AttribClass::primitive, "id", AttribType::string);
    id.ints()[0] = id.intern("a&b\"<c>");
    id.ints()[1] = id.intern("plain");
    AttribArray& cls = g.attrib(AttribClass::primitive, "class", AttribType::string);
    cls.ints()[0] = cls.intern("road main");
    cls.ints()[1] = cls.intern("tab\tline\nbreak");
    const std::string svg = geometryToSvg(g, SvgOptions{});
    CHECK(svg.find("<polyline id=\"a&amp;b&quot;&lt;c&gt;\" class=\"road main\" points=") != std::string::npos);
    CHECK(svg.find("<polygon id=\"plain\" class=\"tab&#9;line&#10;break\" points=") != std::string::npos);
    CHECK(svg.find("<polyline points=") != std::string::npos);

    const std::string without = geometryToSvg(labelled({"x"}, {}), SvgOptions{});
    CHECK(without.find(" id=") == std::string::npos);
}

TEST(svgExportColorsPrimitivesFromCdAndFillsClosedShapes) {
    Geometry g;
    g.resizePoints(6);
    for (int i = 0; i < 6; ++i) {
        g.x()[i] = static_cast<float>(i);
        g.y()[i] = static_cast<float>(i % 2);
    }
    g.addPrimitive({0, 3, true, PrimKind::polyline});
    g.addPrimitive({3, 3, false, PrimKind::polyline});
    const float red[] = {1, 1, 1, 0, 0, 0}, green[] = {0, 0, 0, 0.5f, 0.5f, 0.5f}, blue[] = {0, 0, 0, 1, 1, 1}, alpha[] = {0.5f, 0.5f, 0.5f, 1, 1, 1};
    const char* names[] = {"Cd.x", "Cd.y", "Cd.z", "alpha"};
    const float* values[] = {red, green, blue, alpha};
    for (int c = 0; c < 4; ++c) {
        float* dst = g.attrib(AttribClass::point, names[c]).floats().data();
        for (int i = 0; i < 6; ++i) dst[i] = values[c][i];
    }

    const std::string plain = geometryToSvg(g);
    CHECK(plain.find("#ff0000") == std::string::npos);

    SvgOptions options;
    options.useColor = true;
    const std::string stroked = geometryToSvg(g, options);
    CHECK(stroked.find("stroke=\"#ff0000\" stroke-opacity=\"0.5\"") != std::string::npos);
    CHECK(stroked.find("stroke=\"#0080ff\" stroke-opacity=\"1\"") != std::string::npos);
    CHECK(stroked.find("fill-opacity") == std::string::npos);

    g.setDetail("render.fill", 1.0f);
    const std::string filled = geometryToSvg(g, options);
    CHECK(filled.find("<polygon points=\"") != std::string::npos);
    CHECK(filled.find("fill=\"#ff0000\" fill-opacity=\"0.5\" stroke=\"none\"") != std::string::npos);
    CHECK(filled.find("stroke=\"#0080ff\"") != std::string::npos);
}

TEST(graphJsonKeepsStringAttributesOfInlineGeometry) {
    if (!NodeRegistry::instance().has("testGeoParam")) NodeRegistry::instance().registerType("testGeoParam", [] { return std::make_unique<GeoParamNode>(); });
    Geometry g = labelled({"tree", "caf\xc3\xa9", "quote\"back\\slash\n", "", "tree", "\xf0\x9f\x8c\xb3"}, {"unused"});
    g.addPrimitive({0, 3, false, PrimKind::polyline});
    g.addPrimitive({3, 3, true, PrimKind::polyline});
    AttribArray& cls = g.attrib(AttribClass::primitive, "class", AttribType::string);
    cls.ints()[0] = cls.intern("road");
    cls.ints()[1] = cls.intern("river");
    Network net;
    const NodeId id = net.addNode("testGeoParam");
    net.node(id).setGeometryParam("curve", std::make_shared<Geometry>(g));
    const std::string text = saveNetwork(net);
    CHECK(text.find("primStrings") != std::string::npos);

    Network loaded;
    loadNetwork(loaded, text);
    const GeometryHandle back = loaded.node(loaded.nodeIds()[0]).geometryParam("curve");
    back->checkStrings();
    CHECK(sameGeometry(g, *back));
    CHECK(back->findAttrib(AttribClass::point, "label")->stringAt(2) == "quote\"back\\slash\n");
    CHECK(back->findAttrib(AttribClass::primitive, "class")->stringAt(1) == "river");
    CHECK(saveNetwork(loaded) == text);

    AttribArray& label = g.attrib(AttribClass::point, "label", AttribType::string);
    label.ints()[0] = label.intern("bad\xff");
    net.node(id).setGeometryParam("curve", std::make_shared<Geometry>(g));
    bool threw = false;
    try {
        saveNetwork(net);
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

using Polyline = std::vector<std::pair<double, double>>;

Geometry polylines(const std::vector<Polyline>& lines, bool closed = false) {
    Geometry g;
    size_t total = 0;
    for (const Polyline& l : lines) total += l.size();
    g.resizePoints(total);
    size_t at = 0;
    for (const Polyline& l : lines) {
        g.addPrimitive({static_cast<uint32_t>(at), static_cast<uint32_t>(l.size()), closed, PrimKind::polyline});
        for (const auto& p : l) {
            g.x()[at] = static_cast<float>(p.first);
            g.y()[at] = static_cast<float>(p.second);
            ++at;
        }
    }
    return g;
}

struct ShapeRig {
    Network net{1};
    NodeId node;
    ShapeRig(Geometry g, const char* type) {
        net.setExternalInputs({std::make_shared<Geometry>(std::move(g))});
        const NodeId in = net.addNode("subnetInput", "in");
        node = net.addNode(type, "n");
        net.connect(in, node);
    }
    Node& n() { return net.node(node); }
    GeometryHandle cook() { return net.cook(node); }
};

TEST(smoothShrinksARingOnlyWithoutPreserveAndKeepsOpenEnds) {
    Network net(1);
    const NodeId ring = net.addNode("circle");
    net.node(ring).setFloat("count", 64.0f);
    const NodeId smooth = net.addNode("smooth");
    net.connect(ring, smooth);
    net.node(smooth).setFloat("iterations", 40.0f);
    net.node(smooth).setFloat("preserve", 0.0f);
    auto plain = net.cook(smooth);
    CHECK(plain->pointCount() == 64);
    CHECK(std::hypot(plain->x()[0], plain->y()[0]) < 0.93f);
    net.node(smooth).setFloat("preserve", 1.0f);
    auto kept = net.cook(smooth);
    for (size_t i = 0; i < 64; ++i) CHECK(std::fabs(std::hypot(kept->x()[i], kept->y()[i]) - 1.0f) < 0.01f);

    ShapeRig zigzag(polylines({{{0, 0}, {1, 1}, {2, 0}, {3, 1}, {4, 0}}}), "smooth");
    auto flat = zigzag.cook();
    CHECK(nearly(flat->x()[0], 0.0f) && nearly(flat->y()[0], 0.0f) && nearly(flat->x()[4], 4.0f) && nearly(flat->y()[4], 0.0f));
    CHECK(flat->y()[1] < 0.99f && flat->y()[1] > 0.0f);
    zigzag.n().setFloat("iterations", 0.0f);
    CHECK(nearly(zigzag.cook()->y()[1], 1.0f));
}

TEST(offsetMakesParallelCopiesOnTheLeftWithMiteredCorners) {
    Network net(1);
    const NodeId ring = net.addNode("circle");
    net.node(ring).setFloat("count", 64.0f);
    const NodeId off = net.addNode("offset");
    net.connect(ring, off);
    net.node(off).setFloat("distance", 0.2f);
    net.node(off).setFloat("count", 2.0f);
    net.node(off).setFloat("keepOriginal", 1.0f);
    auto g = net.cook(off);
    CHECK(g->primitiveCount() == 3 && g->pointCount() == 192);
    const AttribArray* index = g->findAttrib(AttribClass::primitive, "offsetIndex");
    CHECK(index && index->ints()[0] == 0 && index->ints()[2] == 2);
    const float miter = 1.0f / std::cos(3.14159265f / 64.0f);
    CHECK(std::fabs(std::hypot(g->x()[0], g->y()[0]) - 1.0f) < 1e-5f);
    CHECK(std::fabs(std::hypot(g->x()[64], g->y()[64]) - (1.0f - 0.2f * miter)) < 1e-4f);
    CHECK(std::fabs(std::hypot(g->x()[128], g->y()[128]) - (1.0f - 0.4f * miter)) < 1e-4f);

    ShapeRig line(polylines({{{0, 0}, {1, 0}, {2, 0}}}), "offset");
    line.n().setFloat("distance", -0.5f);
    auto below = line.cook();
    CHECK(below->primitiveCount() == 1 && nearly(below->y()[1], -0.5f) && nearly(below->x()[1], 1.0f));
}

TEST(sortReordersPrimitivesWithTheirPointsAndAttributes) {
    Geometry g = polylines({{{2, 0}, {2, 1}}, {{0, 0}, {0, 1}}, {{1, 0}, {1, 1}}});
    AttribArray& id = g.attrib(AttribClass::primitive, "id", AttribType::int32);
    id.ints()[0] = 20;
    id.ints()[1] = 0;
    id.ints()[2] = 10;
    g.attrib(AttribClass::point, "tag").floats()[0] = 7.0f;
    ShapeRig rig(g, "sort");
    auto sorted = rig.cook();
    CHECK(sorted->primitiveCount() == 3 && sorted->pointCount() == 6);
    for (size_t p = 0; p < 3; ++p) {
        CHECK(sorted->primitives()[p].start == 2 * p);
        CHECK(nearly(sorted->x()[2 * p], static_cast<float>(p)));
        CHECK(sorted->findAttrib(AttribClass::primitive, "id")->ints()[p] == static_cast<int32_t>(10 * p));
    }
    CHECK(nearly(sorted->findAttrib(AttribClass::point, "tag")->floats()[4], 7.0f));

    rig.n().setFloat("reverse", 1.0f);
    CHECK(nearly(rig.cook()->x()[0], 2.0f));
    rig.n().setFloat("reverse", 0.0f);
    rig.n().setFloat("shift", 1.0f);
    CHECK(nearly(rig.cook()->x()[0], 1.0f) && nearly(rig.cook()->x()[4], 0.0f));
    rig.n().setFloat("shift", 0.0f);
    rig.n().setString("key", "attrib");
    rig.n().setString("attrib", "id");
    CHECK(nearly(rig.cook()->x()[0], 0.0f));
    rig.n().setString("key", "angle");
    CHECK(rig.cook()->primitiveCount() == 3);
    rig.n().setString("class", "point");
    CHECK(cookThrows(rig.net, rig.node));
    rig.n().setString("class", "primitive");
    rig.n().setString("key", "nonsense");
    CHECK(cookThrows(rig.net, rig.node));

    Geometry cloud;
    cloud.resizePoints(4);
    const float xs[] = {3, 1, 2, 0};
    for (int i = 0; i < 4; ++i) cloud.x()[i] = xs[i];
    AttribArray& label = cloud.attrib(AttribClass::point, "label", AttribType::string);
    const char* names[] = {"d", "b", "c", "a"};
    for (int i = 0; i < 4; ++i) label.ints()[i] = label.intern(names[i]);
    ShapeRig points(cloud, "sort");
    points.n().setString("class", "point");
    auto byX = points.cook();
    for (int i = 0; i < 4; ++i) CHECK(nearly(byX->x()[i], static_cast<float>(i)));
    points.n().setString("key", "attrib");
    points.n().setString("attrib", "label");
    auto byText = points.cook();
    for (int i = 0; i < 4; ++i) CHECK(byText->findAttrib(AttribClass::point, "label")->stringAt(i) == std::string(1, static_cast<char>('a' + i)));
    byText->checkStrings();
}

TEST(joinChainsCurvesThatMeetAndClosesLoops) {
    ShapeRig chain(polylines({{{0, 0}, {1, 0}}, {{2, 0}, {1, 0}}, {{2, 0}, {2, 1}}}), "join");
    auto joined = chain.cook();
    CHECK(joined->primitiveCount() == 1 && joined->pointCount() == 4 && !joined->primitives()[0].closed);
    const float wantX[] = {0, 1, 2, 2}, wantY[] = {0, 0, 0, 1};
    for (int i = 0; i < 4; ++i) CHECK(nearly(joined->x()[i], wantX[i]) && nearly(joined->y()[i], wantY[i]));

    ShapeRig loop(polylines({{{0, 0}, {1, 0}}, {{1, 0}, {1, 1}}, {{1, 1}, {0, 1}}, {{0, 1}, {0, 0}}}), "join");
    auto ring = loop.cook();
    CHECK(ring->primitiveCount() == 1 && ring->pointCount() == 4 && ring->primitives()[0].closed);
    loop.n().setFloat("distance", 0.0f);
    CHECK(loop.cook()->primitiveCount() == 1);

    ShapeRig apart(polylines({{{0, 0}, {1, 0}}, {{1.5f, 0}, {2, 0}}}), "join");
    CHECK(apart.cook()->primitiveCount() == 2);
    apart.n().setFloat("distance", 0.6f);
    CHECK(apart.cook()->primitiveCount() == 1);
    apart.n().setFloat("distance", 0.001f);
    apart.n().setString("mode", "all");
    apart.n().setFloat("close", 1.0f);
    auto all = apart.cook();
    CHECK(all->primitiveCount() == 1 && all->pointCount() == 4 && all->primitives()[0].closed);
    apart.n().setString("mode", "weave");
    CHECK(cookThrows(apart.net, apart.node));

    ShapeRig square(polylines({{{0, 0}, {1, 0}, {1, 1}, {0, 1}}}, true), "join");
    CHECK(square.cook()->primitiveCount() == 1 && square.cook()->primitives()[0].closed);
}

TEST(bevelCutsCornersAndRoundsThemWithSegments) {
    ShapeRig square(polylines({{{0, 0}, {2, 0}, {2, 2}, {0, 2}}}, true), "bevel");
    square.n().setFloat("distance", 0.5f);
    auto chamfer = square.cook();
    CHECK(chamfer->pointCount() == 8 && chamfer->primitives()[0].closed);
    const float corners[4][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
    for (size_t i = 0; i < 8; ++i) {
        float nearest = 1e9f;
        for (const auto& c : corners) nearest = std::min(nearest, std::hypot(chamfer->x()[i] - c[0], chamfer->y()[i] - c[1]));
        CHECK(std::fabs(nearest - 0.5f) < 1e-5f);
    }
    square.n().setFloat("segments", 4.0f);
    auto round = square.cook();
    CHECK(round->pointCount() == 20);
    for (size_t i = 0; i < 20; ++i) {
        const float x = round->x()[i], y = round->y()[i];
        CHECK(x >= -1e-5f && x <= 2.00001f && y >= -1e-5f && y <= 2.00001f);
    }
    square.n().setFloat("distance", 5.0f);
    auto clamped = square.cook();
    CHECK(clamped->pointCount() == 20 && std::fabs(clamped->x()[0] - 0.0f) < 1e-5f && std::fabs(clamped->y()[0] - 1.0f) < 1e-5f);

    ShapeRig bend(polylines({{{0, 0}, {1, 0}, {1, 1}}}), "bevel");
    bend.n().setFloat("distance", 0.25f);
    auto elbow = bend.cook();
    CHECK(elbow->pointCount() == 4 && nearly(elbow->x()[0], 0.0f) && nearly(elbow->x()[1], 0.75f) && nearly(elbow->y()[2], 0.25f) && nearly(elbow->y()[3], 1.0f));
    bend.n().setFloat("minAngle", 120.0f);
    CHECK(bend.cook()->pointCount() == 3);
}

TEST(twistRotatesByDistanceAndFadesAtTheRadius) {
    ShapeRig rig(polylines({{{1, 0}, {0, 2}, {0, 0}}}), "twist");
    auto linear = rig.cook();
    CHECK(nearly(linear->x()[0], 0.0f) && nearly(linear->y()[0], 1.0f));
    CHECK(std::fabs(std::hypot(linear->x()[1], linear->y()[1]) - 2.0f) < 1e-4f);
    CHECK(nearly(linear->x()[2], 0.0f) && nearly(linear->y()[2], 0.0f));
    rig.n().setFloat("radius", 2.0f);
    auto swirl = rig.cook();
    CHECK(std::fabs(swirl->x()[0] - std::cos(3.14159265f / 4.0f)) < 1e-4f && std::fabs(swirl->y()[0] - std::sin(3.14159265f / 4.0f)) < 1e-4f);
    CHECK(nearly(swirl->x()[1], 0.0f) && nearly(swirl->y()[1], 2.0f));
    CHECK(nearly(swirl->x()[2], 0.0f));
    rig.n().setFloat("amount", 0.0f);
    CHECK(nearly(rig.cook()->x()[0], 1.0f));
}

TEST(peakPushesAlongTheLeftNormalOrRadially) {
    Network net(1);
    const NodeId ring = net.addNode("circle");
    net.node(ring).setFloat("count", 32.0f);
    const NodeId peak = net.addNode("peak");
    net.connect(ring, peak);
    net.node(peak).setFloat("amount", -0.25f);
    auto grown = net.cook(peak);
    for (size_t i = 0; i < 32; ++i) CHECK(std::fabs(std::hypot(grown->x()[i], grown->y()[i]) - 1.25f) < 1e-4f);
    net.node(peak).setString("mode", "radial");
    net.node(peak).setFloat("amount", 1.0f);
    auto radial = net.cook(peak);
    CHECK(std::fabs(std::hypot(radial->x()[5], radial->y()[5]) - 2.0f) < 1e-4f);
    net.node(peak).setString("scaleAttrib", "missing");
    CHECK(cookThrows(net, peak));

    Geometry line = polylines({{{0, 0}, {1, 0}, {2, 0}}});
    line.attrib(AttribClass::point, "w").floats()[1] = 3.0f;
    ShapeRig rig(line, "peak");
    rig.n().setString("scaleAttrib", "w");
    rig.n().setFloat("amount", 0.5f);
    auto lifted = rig.cook();
    CHECK(nearly(lifted->y()[0], 0.0f) && nearly(lifted->y()[1], 1.5f) && nearly(lifted->x()[1], 1.0f));
    rig.n().setString("mode", "sideways");
    CHECK(cookThrows(rig.net, rig.node));
}

TEST(latticeMovesPointsWithTheControlGridAndRejectsAWrongCount) {
    const auto grid = [](float dx, float dy, float stretch) {
        Polyline pts;
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i) pts.push_back({i * stretch + dx, j * stretch + dy});
        return polylines({pts});
    };
    Network net(1);
    net.setExternalInputs({std::make_shared<Geometry>(polylines({{{0, 0}, {1, 1}, {2, 2}, {0.5, 1.5}}})), std::make_shared<Geometry>(grid(1.0f, -2.0f, 1.0f))});
    const NodeId body = net.addNode("subnetInput", "body");
    const NodeId cage = net.addNode("subnetInput", "cage");
    net.node(cage).setFloat("index", 1.0f);
    const NodeId lat = net.addNode("lattice", "lat");
    net.connect(body, lat, 0);
    net.connect(cage, lat, 1);
    net.node(lat).setFloat("columns", 3.0f);
    net.node(lat).setFloat("rows", 3.0f);
    auto moved = net.cook(lat);
    for (size_t i = 0; i < 4; ++i) {
        const Geometry& rest = *net.cook(body);
        CHECK(nearly(moved->x()[i], rest.x()[i] + 1.0f) && nearly(moved->y()[i], rest.y()[i] - 2.0f));
    }
    net.node(lat).setFloat("amount", 0.5f);
    CHECK(nearly(net.cook(lat)->x()[2], 2.5f));
    net.node(lat).setFloat("amount", 1.0f);
    net.node(lat).setFloat("minX", -1.0f);
    net.node(lat).setFloat("maxX", 3.0f);
    net.node(lat).setFloat("minY", -1.0f);
    net.node(lat).setFloat("maxY", 3.0f);
    CHECK(net.cook(lat)->pointCount() == 4);
    net.node(lat).setFloat("columns", 4.0f);
    CHECK(cookThrows(net, lat));

    Network scaled(1);
    scaled.setExternalInputs({std::make_shared<Geometry>(polylines({{{0, 0}, {2, 0}, {1, 1}}})), std::make_shared<Geometry>(grid(0.0f, 0.0f, 2.0f))});
    const NodeId b2 = scaled.addNode("subnetInput", "b");
    const NodeId c2 = scaled.addNode("subnetInput", "c");
    scaled.node(c2).setFloat("index", 1.0f);
    const NodeId l2 = scaled.addNode("lattice");
    scaled.connect(b2, l2, 0);
    scaled.connect(c2, l2, 1);
    scaled.node(l2).setFloat("columns", 3.0f);
    scaled.node(l2).setFloat("rows", 3.0f);
    auto stretched = scaled.cook(l2);
    CHECK(std::fabs(stretched->x()[1] - 4.0f) < 1e-3f && std::fabs(stretched->y()[1]) < 1e-3f);
    CHECK(std::fabs(stretched->x()[2] - 2.0f) < 1e-3f && std::fabs(stretched->y()[2] - 4.0f) < 1e-3f);
}

TEST(switchCooksOnlyTheSelectedInput) {
    Network net(1);
    const NodeId a = net.addNode("generate");
    net.node(a).setFloat("count", 5.0f);
    const NodeId b = net.addNode("generate");
    net.node(b).setFloat("count", 9.0f);
    const NodeId sw = net.addNode("switch");
    net.connect(a, sw, 0);
    net.connect(b, sw, 1);
    CHECK(net.cook(sw)->pointCount() == 5);
    CHECK(net.node(a).cookCount() == 1 && net.node(b).cookCount() == 0);
    net.node(sw).setFloat("index", 1.0f);
    CHECK(net.cook(sw)->pointCount() == 9);
    CHECK(net.node(b).cookCount() == 1);
    net.node(sw).setFloat("index", 7.0f);
    CHECK(net.cook(sw)->pointCount() == 9);
    net.node(sw).setFloat("index", -3.0f);
    CHECK(net.cook(sw)->pointCount() == 5);
    net.node(sw).setExpression("index", "time");
    net.setTime(1.0);
    CHECK(net.cook(sw)->pointCount() == 9);
    net.setTime(0.0);
    CHECK(net.cook(sw)->pointCount() == 5);

    Network lone(1);
    const NodeId empty = lone.addNode("switch");
    CHECK(cookThrows(lone, empty));
}

TEST(polarBendsARowIntoARingAndBackAgain) {
    Polyline row;
    for (int i = 0; i < 8; ++i) row.push_back({static_cast<float>(i) / 8.0f, 2.0f});
    ShapeRig rig(polylines({row}), "polar");
    auto ring = rig.cook();
    for (size_t i = 0; i < 8; ++i) CHECK(std::fabs(std::hypot(ring->x()[i], ring->y()[i]) - 2.0f) < 1e-5f);
    CHECK(nearly(ring->x()[0], 2.0f) && std::fabs(ring->y()[2] - 2.0f) < 1e-5f);

    Network net(1);
    net.setExternalInputs({ring});
    const NodeId in = net.addNode("subnetInput", "in");
    const NodeId back = net.addNode("polar", "back");
    net.connect(in, back);
    net.node(back).setString("mode", "fromPolar");
    auto flat = net.cook(back);
    for (size_t i = 0; i < 8; ++i) CHECK(std::fabs(flat->x()[i] - static_cast<float>(i) / 8.0f) < 1e-4f && std::fabs(flat->y()[i] - 2.0f) < 1e-4f);

    rig.n().setFloat("amount", 0.0f);
    CHECK(nearly(rig.cook()->x()[3], 3.0f / 8.0f));
    rig.n().setFloat("width", 0.0f);
    CHECK(cookThrows(rig.net, rig.node));
    rig.n().setFloat("width", 1.0f);
    rig.n().setString("mode", "spiral");
    CHECK(cookThrows(rig.net, rig.node));
}

// Four primitives of three points each; point (p, j) sits at x = p * 10 + j, and the primitive attribute "kind" is 0, 1, 0, 1.
GeometryHandle loopInput() {
    auto geo = std::make_shared<Geometry>();
    geo->resizePoints(12);
    for (uint32_t p = 0; p < 4; ++p) {
        for (uint32_t j = 0; j < 3; ++j) {
            geo->x()[p * 3 + j] = static_cast<float>(p * 10 + j);
            geo->y()[p * 3 + j] = 0.0f;
        }
        geo->addPrimitive({p * 3, 3, false, PrimKind::polyline});
    }
    AttribArray& kind = geo->attrib(AttribClass::primitive, "kind");
    for (size_t p = 0; p < 4; ++p) kind.floats()[p] = static_cast<float>(p % 2);
    return geo;
}

// input -> loop node -> null, with `subnetInput -> transform` inside the loop node. Returns the transform's network for edits.
struct LoopRig {
    Network net{1};
    NodeId in, loop, out, inInner, xf;
    explicit LoopRig(const char* type) {
        net.setExternalInputs({loopInput()});
        in = net.addNode("subnetInput", "in");
        loop = net.addNode(type, "loop");
        out = net.addNode("null", "out");
        Network& inner = dynamic_cast<SubnetNode&>(net.node(loop)).network();
        inInner = inner.addNode("subnetInput", "in");
        xf = inner.addNode("transform", "xf");
        inner.connect(inInner, xf);
        inner.setDisplayNode(xf);
        net.connect(in, loop);
        net.connect(loop, out);
        net.setDisplayNode(out);
    }
    Network& inner() { return dynamic_cast<SubnetNode&>(net.node(loop)).network(); }
};

TEST(foreachCooksTheChildGraphOncePerPrimitiveAndKnowsTheIteration) {
    LoopRig rig("foreach");
    rig.inner().node(rig.xf).setExpression("ty", "parent.iteration + 1");
    auto out = rig.net.cookDisplay();
    CHECK(out->pointCount() == 12 && out->primitiveCount() == 4);
    for (size_t p = 0; p < 4; ++p) {
        CHECK(out->primitives()[p].start == p * 3);
        for (size_t j = 0; j < 3; ++j) {
            CHECK_NEAR(out->x()[p * 3 + j], static_cast<float>(p * 10 + j));
            CHECK_NEAR(out->y()[p * 3 + j], static_cast<float>(p + 1));
        }
    }
    CHECK_NEAR(out->findAttrib(AttribClass::primitive, "kind")->floats()[3], 1.0f);
    CHECK(rig.net.node(rig.loop).getFloat("iteration") == 0.0f && rig.net.node(rig.loop).getFloat("numIterations") == 0.0f);

    rig.net.cookDisplay();
    CHECK(rig.net.node(rig.loop).cookCount() == 1);
    rig.inner().node(rig.xf).setExpression("ty", "parent.numIterations");
    CHECK_NEAR(rig.net.cookDisplay()->y()[7], 4.0f);
}

TEST(foreachGroupsPrimitivesByAnAttributeAndWalksPoints) {
    LoopRig rig("foreach");
    rig.net.node(rig.loop).setString("pieceAttrib", "kind");
    rig.inner().node(rig.xf).setExpression("ty", "parent.iteration * 5");
    auto out = rig.net.cookDisplay();
    CHECK(out->primitiveCount() == 4);
    const auto& kind = out->findAttrib(AttribClass::primitive, "kind")->floats();
    CHECK(kind[0] == 0.0f && kind[1] == 0.0f && kind[2] == 1.0f && kind[3] == 1.0f);
    CHECK_NEAR(out->y()[0], 0.0f);
    CHECK_NEAR(out->y()[11], 5.0f);
    CHECK_NEAR(out->x()[3], 20.0f);  // the first piece holds primitives 0 and 2

    rig.net.node(rig.loop).setString("pieceAttrib", "missing");
    CHECK(cookThrows(rig.net, rig.out));

    rig.net.node(rig.loop).setString("pieceAttrib", "");
    rig.net.node(rig.loop).setString("mode", "points");
    out = rig.net.cookDisplay();
    CHECK(out->pointCount() == 12 && out->primitiveCount() == 0);
    CHECK_NEAR(out->y()[11], 55.0f);
}

TEST(foreachCopiesAndFeedbackRepeatTheChildGraph) {
    LoopRig rig("foreach");
    rig.inner().node(rig.xf).setFloat("tx", 100.0f);
    rig.net.node(rig.loop).setString("mode", "copies");
    rig.net.node(rig.loop).setFloat("count", 3.0f);
    auto out = rig.net.cookDisplay();
    CHECK(out->pointCount() == 36 && out->primitiveCount() == 12);
    CHECK_NEAR(out->x()[12], 100.0f);

    rig.net.node(rig.loop).setString("mode", "feedback");
    out = rig.net.cookDisplay();
    CHECK(out->pointCount() == 12);
    CHECK_NEAR(out->x()[0], 300.0f);
    rig.net.node(rig.loop).setFloat("count", 0.0f);
    CHECK_NEAR(rig.net.cookDisplay()->x()[0], 0.0f);
    rig.net.node(rig.loop).setFloat("count", 1e7f);
    CHECK(cookThrows(rig.net, rig.out));
    rig.net.node(rig.loop).setString("mode", "sideways");
    CHECK(cookThrows(rig.net, rig.out));
}

TEST(foreachCooksPiecesInParallelWithTheSameResultAsSerially) {
    LoopRig rig("foreach");
    rig.net.setThreadCount(4);
    rig.net.node(rig.loop).setString("mode", "points");
    rig.inner().node(rig.xf).setExpression("ty", "parent.iteration * 2 + parent.numIterations");
    auto expected = rig.net.cookDisplay();

    rig.net.node(rig.loop).setFloat("parallel", 1.0f);
    auto out = rig.net.cookDisplay();
    CHECK(out->pointCount() == 12);
    for (size_t i = 0; i < 12; ++i) {
        CHECK_NEAR(out->x()[i], expected->x()[i]);
        CHECK_NEAR(out->y()[i], expected->y()[i]);
    }
    CHECK_NEAR(out->y()[5], 22.0f);
    CHECK(rig.net.node(rig.loop).getFloat("iteration") == 0.0f && rig.net.node(rig.loop).getFloat("numIterations") == 0.0f);

    rig.inner().node(rig.xf).setFloat("tx", 100.0f);
    out = rig.net.cookDisplay();
    CHECK_NEAR(out->x()[3], 110.0f);
    CHECK_NEAR(out->y()[11], 34.0f);

    rig.net.node(rig.loop).setString("mode", "copies");
    rig.net.node(rig.loop).setFloat("count", 6.0f);
    out = rig.net.cookDisplay();
    CHECK(out->pointCount() == 72);
    CHECK_NEAR(out->y()[12 * 5], 16.0f);
}

TEST(foreachSurvivesSavingAndLoading) {
    LoopRig rig("foreach");
    rig.inner().node(rig.xf).setExpression("ty", "parent.iteration");
    rig.net.node(rig.loop).setString("mode", "points");
    const std::string text = saveNetwork(rig.net);
    Network copy(1);
    loadNetwork(copy, text);
    CHECK(saveNetwork(copy) == text);
    copy.setExternalInputs({loopInput()});
    auto out = copy.cookDisplay();
    CHECK(out->pointCount() == 12 && copy.node(copy.findNode("loop")).typeName() == "foreach");
    CHECK_NEAR(out->y()[11], 11.0f);
}

// Scrubbing back resumes from a saved copy of the simulation state, and everything after that matches a run from the start.
TEST(simulationsResumeFromSavedStatesWhenScrubbedBack) {
    struct Case {
        const char* name;
        std::function<NodeId(Network&)> build;
    };
    const std::vector<Case> cases = {
        {"particles",
         [](Network& net) {
             NodeId gen = net.addNode("generate"), sim = net.addNode("particles");
             net.node(gen).setFloat("count", 300.0f);
             net.connect(gen, sim);
             net.node(sim).setFloat("noiseForce", 2.0f);
             net.node(sim).setFloat("lifetime", 0.7f);
             return sim;
         }},
        {"boids",
         [](Network& net) {
             NodeId gen = net.addNode("generate"), sim = net.addNode("boids");
             net.node(gen).setFloat("count", 120.0f);
             net.connect(gen, sim);
             return sim;
         }},
        {"springs",
         [](Network& net) {
             NodeId line = net.addNode("line"), sim = net.addNode("springs");
             net.node(line).setFloat("count", 20.0f);
             net.connect(line, sim);
             net.node(sim).setFloat("pinEnds", 1.0f);
             return sim;
         }},
        {"reactionDiffusion",
         [](Network& net) {
             NodeId sim = net.addNode("reactionDiffusion");
             net.node(sim).setFloat("width", 32.0f);
             net.node(sim).setFloat("height", 24.0f);
             net.node(sim).setFloat("stepsPerSecond", 200.0f);
             return sim;
         }},
        {"gasField",
         [](Network& net) {
             NodeId sim = net.addNode("gasField");
             net.node(sim).setFloat("nozzleSpeed", 6.0f);
             net.node(sim).setFloat("nozzleDensity", 1.0f);
             return sim;
         }},
        {"fluid",
         [](Network& net) {
             NodeId merge = net.addNode("merge"), sim = net.addNode("fluid");
             for (int row = 0; row < 6; ++row) {
                 NodeId line = net.addNode("line");
                 net.node(line).setFloat("count", 16.0f);
                 net.node(line).setFloat("length", 1.5f);
                 net.node(line).setFloat("y", 0.1f * static_cast<float>(row));
                 net.connect(line, merge, row);
             }
             net.connect(merge, sim);
             net.node(sim).setFloat("boundsMinX", 0.0f);
             net.node(sim).setFloat("boundsMaxX", 2.0f);
             net.node(sim).setFloat("boundsMinY", -1.0f);
             net.node(sim).setFloat("boundsMaxY", 3.0f);
             return sim;
         }},
        {"grow",
         [](Network& net) {
             NodeId circle = net.addNode("circle"), sim = net.addNode("grow");
             net.node(circle).setFloat("count", 8.0f);
             net.node(circle).setFloat("radius", 0.3f);
             net.connect(circle, sim);
             net.node(sim).setFloat("repelRadius", 2.0f);
             return sim;
         }},
    };

    const int64_t savedInterval = SimulationCache::checkpointInterval;
    SimulationCache::checkpointInterval = 4;
    for (const Case& c : cases) {
        Network net(2);
        const NodeId sim = c.build(net);
        auto fresh = [&](double t) {
            Network other(2);
            const NodeId s = c.build(other);
            other.setTime(t);
            return other.cook(s);
        };
        net.setTime(1.0);
        net.cook(sim);
        const size_t before = SimulationCache::restores;
        net.setTime(0.45);
        auto back = net.cook(sim);
        if (SimulationCache::restores <= before) std::printf("%s did not resume from a saved state\n", c.name);
        CHECK(SimulationCache::restores > before);
        if (!sameGeometry(*back, *fresh(0.45))) std::printf("%s differs after scrubbing back\n", c.name);
        CHECK(sameGeometry(*back, *fresh(0.45)));
        net.setTime(1.3);
        if (!sameGeometry(*net.cook(sim), *fresh(1.3))) std::printf("%s differs after playing on\n", c.name);
        CHECK(sameGeometry(*net.cook(sim), *fresh(1.3)));
        net.setTime(0.0);
        CHECK(sameGeometry(*net.cook(sim), *fresh(0.0)));

        // An edit drops the saved states, so the next run is a new one that saves its own.
        const std::string first = net.node(sim).floatNames().front();
        net.node(sim).setFloat(first, net.node(sim).baseFloat(first) + 0.25f);
        net.setTime(0.9);
        net.cook(sim);
        const size_t afterEdit = SimulationCache::restores;
        net.setTime(0.5);
        auto edited = net.cook(sim);
        CHECK(SimulationCache::restores > afterEdit);
        Network other(2);
        const NodeId s = c.build(other);
        other.node(s).setFloat(first, other.node(s).baseFloat(first) + 0.25f);
        other.setTime(0.5);
        CHECK(sameGeometry(*edited, *other.cook(s)));
    }
    SimulationCache::checkpointInterval = savedInterval;
}

TEST(renderFillsClosedShapesWithCoverageAntiAliasing) {
    Geometry g;
    g.resizePoints(4);
    const float xs[4] = {-2.05f, 2.0f, 2.0f, -2.05f}, ys[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
    for (size_t i = 0; i < 4; ++i) {
        g.x()[i] = xs[i];
        g.y()[i] = ys[i];
    }
    g.addPrimitive({0, 4, true});
    for (const char* name : {"Cd.x", "alpha"}) {
        float* values = g.attrib(AttribClass::point, name).floats().data();
        for (size_t i = 0; i < 4; ++i) values[i] = 1.0f;
    }

    RenderSettings s;
    s.width = 100;
    s.height = 50;
    s.minX = -5.0f;
    s.maxX = 5.0f;
    s.minY = -2.5f;
    s.maxY = 2.5f;
    s.additive = false;
    Image off = renderSplats(g, s, nullptr);
    CHECK((off.pixels[25 * 100 + 50] & 0xFFFFFFu) == 0u);  // shapes are only drawn when fillShapes is set

    s.fillShapes = true;
    Image on = renderSplats(g, s, nullptr);
    auto red = [&](int x, int y) { return static_cast<int>((on.pixels[static_cast<size_t>(y) * 100 + static_cast<size_t>(x)] >> 16) & 255u); };
    CHECK(red(50, 25) >= 253);                        // inside
    CHECK(red(10, 25) <= 2 && red(50, 5) <= 2);       // outside
    CHECK(std::abs(red(29, 25) - 128) <= 12);         // the left edge at x = 29.5 px covers half of column 29
    CHECK(red(30, 25) >= 253 && red(70, 25) <= 2);    // the right edge sits exactly on a pixel boundary
    CHECK(red(50, 14) <= 2 && red(50, 15) >= 253);    // top edge on a row boundary
    ThreadPool pool(4);
    Image threaded = renderSplats(g, s, &pool);
    CHECK(threaded.pixels == on.pixels);              // bands do not change the result
}

TEST(checkpointIntervalShrinksForSlowSteps) {
    const int64_t savedInterval = SimulationCache::checkpointInterval;
    const double savedBudget = SimulationCache::replayBudgetMs;
    SimulationCache::checkpointInterval = 16;
    auto run = [](double budgetMs, int64_t stepMs) {
        SimulationCache::replayBudgetMs = budgetMs;
        StateCheckpoints<int> cp;
        for (int64_t s = 0; s < 33; ++s) {
            std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
            cp.offer(s, int(s), sizeof(int));
        }
        return cp.size();
    };
    CHECK(run(0.0, 3) == 3);  // fixed interval of 16: steps 0, 16, 32
    CHECK(run(100.0, 0) == 3);  // cheap steps keep the configured interval
    CHECK(run(24.0, 3) > 3);  // 3 ms steps: a replay of 16 would take 48 ms, so the interval drops to 4 or less
    SimulationCache::checkpointInterval = savedInterval;
    SimulationCache::replayBudgetMs = savedBudget;
}

TEST(solverCarriesItsStateFromStepToStepAndRestartsOnEdits) {
    LoopRig rig("solver");
    rig.net.node(rig.loop).setFloat("stepsPerSecond", 10.0f);
    rig.inner().node(rig.xf).setFloat("tx", 1.0f);
    rig.net.setTime(0.0);
    CHECK_NEAR(rig.net.cookDisplay()->x()[0], 0.0f);
    CHECK(rig.inner().node(rig.xf).cookCount() == 0);
    rig.net.setTime(0.5);
    CHECK_NEAR(rig.net.cookDisplay()->x()[0], 5.0f);
    CHECK(rig.inner().node(rig.xf).cookCount() == 5);
    rig.net.setTime(0.7);
    CHECK_NEAR(rig.net.cookDisplay()->x()[0], 7.0f);
    CHECK(rig.inner().node(rig.xf).cookCount() == 7);
    CHECK_NEAR(rig.net.cookDisplay()->x()[4], 18.0f);

    const size_t restoresBefore = SimulationCache::restores;
    rig.net.setTime(0.3);
    CHECK_NEAR(rig.net.cookDisplay()->x()[0], 3.0f);
    CHECK(SimulationCache::restores == restoresBefore + 1);  // resumed from the saved start state, no need to restart
    rig.inner().node(rig.xf).setFloat("tx", 2.0f);
    CHECK_NEAR(rig.net.cookDisplay()->x()[0], 6.0f);

    rig.inner().node(rig.xf).setExpression("tx", "parent.step");
    rig.net.setTime(0.4);
    CHECK_NEAR(rig.net.cookDisplay()->x()[0], 10.0f);
    CHECK(rig.net.node(rig.loop).getFloat("step") == 0.0f);
}

TEST(networkLifetimeExpiresWhenTheNetworkIsDestroyed) {
    auto net = std::make_unique<Network>(1);
    const std::weak_ptr<void> lifetime = net->lifetime();
    CHECK(!lifetime.expired());
    CHECK(!net->lifetime().expired());
    net.reset();
    CHECK(lifetime.expired());
}

TEST(generationChangesOnClearAndNotOnOtherEdits) {
    Network net(1);
    const std::uint64_t first = net.generation();
    const NodeId a = net.addNode("line");
    net.node(a).setFloat("count", 4.0f);
    net.removeNode(a);
    CHECK(net.generation() == first);
    net.clear();
    CHECK(net.generation() != first);
    // The ids start over, which is why a handle remembers the generation.
    CHECK(net.addNode("line") == a);
}

TEST(aNestedNetworkLifetimeExpiresWithItsSubnetNode) {
    Network net(1);
    const NodeId sub = net.addNode("subnet", "sub");
    const std::weak_ptr<void> inner = subnetOf(net, sub).network().lifetime();
    const std::weak_ptr<void> outer = net.lifetime();
    CHECK(!inner.expired());
    net.removeNode(sub);
    CHECK(inner.expired());
    CHECK(!outer.expired());
}

// Asset definitions as values, the library, parameter renames.

TEST(assetDefinitionsRoundTripAndKeepUnknownKeys) {
    SubnetRig rig;
    subnetOf(rig.net, rig.sub).addSpareFloat("amount", 2.0f);
    rig.inner().node(rig.xf).setExpression("tx", "parent.amount");
    AssetDefinition d = AssetDefinition::fromNode(rig.net, rig.sub, "defRoundTripAsset", 3);
    CHECK(d.name == "defRoundTripAsset" && d.version == 3 && d.inputs == 1);
    CHECK(d.params.size() == 1 && d.params[0].name == "amount" && d.params[0].defaultValue == 2.0f);
    CHECK(!d.graphText.empty());

    d.label = "Round trip";
    d.category = "Test";
    d.help = "Moves things.";
    d.inputLabels = {"geometry"};
    d.params[0].label = "Amount";
    d.params[0].minimum = 0.0f;
    d.params[0].maximum = 5.0f;
    d.params[0].group = "Main";
    d.params[0].tooltip = "How much";
    d.scripts["onCreated"] = "print('hi')";
    d.renames["old"] = "amount";
    d.files["notes.txt"] = "text";
    CHECK(d.validate().empty());

    std::string text = d.toText();
    const size_t close = text.rfind('}');
    text.insert(close, ", \"futureKey\": { \"a\": [1, 2] }");
    const AssetDefinition back = AssetDefinition::fromText(text);
    CHECK(back.label == d.label && back.category == d.category && back.help == d.help && back.inputLabels == d.inputLabels);
    CHECK(back.params == d.params && back.scripts == d.scripts && back.renames == d.renames && back.files == d.files);
    CHECK(back.version == 3 && back.graphText == d.graphText);
    CHECK(back.unknown.count("futureKey") == 1);
    CHECK(back.toText().find("futureKey") != std::string::npos);
    CHECK(AssetDefinition::fromText(back.toText()).toText() == back.toText());
}

TEST(assetFilesFromBeforeParamsStillRead) {
    const std::string path = std::string(JITNODES_SOURCE_DIR) + "/examples/assets/wobble.asset.json";
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const AssetDefinition d = AssetDefinition::fromText(text);
    CHECK(d.name == "wobbleAsset" && d.params.size() == 2 && d.params[0].name == "amount" && d.params[1].name == "speed");
    CHECK(std::fabs(d.params[0].defaultValue - 0.3f) < 1e-6f && d.params[1].defaultValue == 3.0f);
    CHECK(d.validate().empty());
    CHECK(d.toText().find("\"promote\"") != std::string::npos && d.toText().find("\"params\"") != std::string::npos);
}

TEST(assetDefinitionValidationReportsEachProblem) {
    SubnetRig rig;
    subnetOf(rig.net, rig.sub).addSpareFloat("amount", 2.0f);
    const AssetDefinition good = AssetDefinition::fromNode(rig.net, rig.sub, "validateGoodAsset");
    CHECK(good.validate().empty());

    auto problem = [&](const std::function<void(AssetDefinition&)>& change) {
        AssetDefinition d = good;
        change(d);
        return d.validate();
    };
    CHECK(problem([](AssetDefinition& d) { d.name = ""; }).find("type name") != std::string::npos);
    CHECK(problem([](AssetDefinition& d) { d.name = "9lives"; }).find("valid type name") != std::string::npos);
    CHECK(problem([](AssetDefinition& d) { d.name = "line"; }).find("built-in") != std::string::npos);
    CHECK(problem([](AssetDefinition& d) { d.inputs = 99; }).find("between 0 and 16") != std::string::npos);
    CHECK(problem([](AssetDefinition& d) { d.inputLabels = {"a", "b"}; }).find("input label") != std::string::npos);
    CHECK(problem([](AssetDefinition& d) { d.params.push_back(d.params[0]); }).find("twice") != std::string::npos);
    CHECK(problem([](AssetDefinition& d) { d.params[0].name = "bad name"; }).find("parameter name") != std::string::npos);
    CHECK(problem([](AssetDefinition& d) { d.params[0].minimum = 5.0f; d.params[0].maximum = 1.0f; }).find("minimum is above") != std::string::npos);
    CHECK(problem([](AssetDefinition& d) { d.params[0].maximum = 1.0f; }).find("outside its limits") != std::string::npos);
    CHECK(problem([](AssetDefinition& d) { d.renames["a"] = "a"; }).find("rename") != std::string::npos);
    CHECK(problem([](AssetDefinition& d) { d.scripts["onSomethingElse"] = "x"; }).find("script name") != std::string::npos);
    CHECK(problem([](AssetDefinition& d) { d.graphText = "{ nodes: [ { id: 1, type: 'noSuchNodeType', name: 'x', inputs: [] } ] }"; }).find("noSuchNodeType") != std::string::npos);

    AssetDefinition::setScriptValidator([](const AssetDefinition& d) { return d.scripts.count("onFrameChanged") ? std::string("onFrameChanged 3:5 cannot change the asset") : std::string(); });
    CHECK(problem([](AssetDefinition&) {}).empty());
    CHECK(problem([](AssetDefinition& d) { d.scripts["onFrameChanged"] = "x"; }) == "onFrameChanged 3:5 cannot change the asset");
    AssetDefinition::setScriptValidator({});
    CHECK(problem([](AssetDefinition& d) { d.scripts["onFrameChanged"] = "x"; }).empty());
}

TEST(assetParameterLimitsAndLabelsReachInstances) {
    SubnetRig rig;
    subnetOf(rig.net, rig.sub).addSpareFloat("amount", 2.0f);
    subnetOf(rig.net, rig.sub).addSpareFloat("other", 1.0f);
    AssetDefinition d = AssetDefinition::fromNode(rig.net, rig.sub, "limitsAsset");
    d.params[0].minimum = 0.0f;
    d.params[0].maximum = 5.0f;
    d.params[0].group = "Main";
    registerAsset(d.toText());

    Network net(1);
    const NodeId a = net.addNode("limitsAsset");
    float value = -1.0f;
    CHECK(net.node(a).floatMinimum("amount", value) && value == 0.0f);
    CHECK(net.node(a).floatMaximum("amount", value) && value == 5.0f);
    CHECK(!net.node(a).floatMinimum("other", value));
    const auto& asset = dynamic_cast<const AssetNode&>(net.node(a));
    CHECK(asset.assetParam("amount") && asset.assetParam("amount")->group == "Main" && asset.assetParam("nope") == nullptr);
    CHECK(net.node(a).baseFloat("amount") == 2.0f);
}

TEST(renamedAssetParametersKeepValuesAcrossVersions) {
    SubnetRig rig;
    subnetOf(rig.net, rig.sub).addSpareFloat("scale", 1.0f);
    rig.inner().node(rig.xf).setExpression("tx", "parent.scale * 10");
    registerAsset(AssetDefinition::fromNode(rig.net, rig.sub, "renameChainAsset", 1).toText());

    Network net(1);
    const NodeId line = net.addNode("line");
    const NodeId a = net.addNode("renameChainAsset", "inst");
    net.connect(line, a);
    net.node(a).setFloat("scale", 3.0f);
    net.node(a).setKeyframes("scale", {{0.0, 3.0f, KeyInterp::linear}, {2.0, 5.0f, KeyInterp::linear}});
    const std::string oldGraph = saveNetwork(net);

    // Version 2 renames scale to gain; the contents follow.
    CHECK(renameSpareParameter(rig.net, rig.sub, "scale", "gain") == 1);
    AssetDefinition v2 = AssetDefinition::fromNode(rig.net, rig.sub, "renameChainAsset", 2);
    v2.renames["scale"] = "gain";
    registerAsset(v2.toText());
    CHECK(updateAssetInstances(net) == 1);
    CHECK(net.node(a).hasFloat("gain") && !net.node(a).hasFloat("scale"));
    CHECK(net.node(a).keyframes("gain").size() == 2 && net.node(a).keyframes("gain")[1].value == 5.0f);

    // Version 3 renames it again: an instance of version 1 follows the chain.
    CHECK(renameSpareParameter(rig.net, rig.sub, "gain", "strength") == 1);
    AssetDefinition v3 = AssetDefinition::fromNode(rig.net, rig.sub, "renameChainAsset", 3);
    v3.renames["scale"] = "gain";
    v3.renames["gain"] = "strength";
    registerAsset(v3.toText());

    // A graph file saved with version 1 migrates by the renames when it is loaded.
    Network loaded(1);
    loadNetwork(loaded, oldGraph);
    const NodeId b = loaded.findNode("inst");
    CHECK(loaded.node(b).hasFloat("strength") && loaded.node(b).keyframes("strength").size() == 2);
    CHECK(updateAssetInstances(net) == 1 && net.node(a).hasFloat("strength"));
}

TEST(renamingAPromotedParameterRewritesTheExpressionsThatReadIt) {
    SubnetRig rig;
    SubnetNode& sub = subnetOf(rig.net, rig.sub);
    sub.addSpareFloat("amount", 0.5f);
    sub.addSpareFloat("amountMore", 1.0f);
    rig.inner().node(rig.xf).setExpression("tx", "parent.amount * 10");
    rig.inner().node(rig.xf).setExpression("ty", "parent.amountMore + parent.amount");
    CHECK_NEAR(rig.net.cook(rig.out)->x()[0], 5.0f);

    std::string message;
    CHECK(throwsRuntimeError([&] { renameSpareParameter(rig.net, rig.sub, "nope", "x"); }, &message) && message.find("not a promoted") != std::string::npos);
    CHECK(throwsRuntimeError([&] { renameSpareParameter(rig.net, rig.sub, "amount", "amountMore"); }, &message) && message.find("already exists") != std::string::npos);
    CHECK(throwsRuntimeError([&] { renameSpareParameter(rig.net, rig.sub, "amount", "bad name"); }));
    CHECK(rig.inner().node(rig.xf).expression("tx") == "parent.amount * 10");

    CHECK(renameSpareParameter(rig.net, rig.sub, "amount", "size") == 2);
    CHECK(rig.inner().node(rig.xf).expression("tx") == "parent.size * 10");
    CHECK(rig.inner().node(rig.xf).expression("ty") == "parent.amountMore + parent.size");
    CHECK(sub.isSpare("size") && !sub.isSpare("amount") && sub.baseFloat("size") == 0.5f);
    CHECK_NEAR(rig.net.cook(rig.out)->x()[0], 5.0f);
}

TEST(renamingAParameterOfADefinitionRewritesItsContentsAndRecordsTheRename) {
    SubnetRig rig;
    subnetOf(rig.net, rig.sub).addSpareFloat("amount", 2.0f);
    subnetOf(rig.net, rig.sub).addSpareFloat("other", 1.0f);
    rig.inner().node(rig.xf).setExpression("tx", "parent.amount * 10");
    AssetDefinition d = AssetDefinition::fromNode(rig.net, rig.sub, "renameParamAsset");
    d.renames["first"] = "amount";

    CHECK(d.renameParam("amount", "size") == 1);
    CHECK(d.params[0].name == "size" && d.renames["amount"] == "size" && d.renames["first"] == "size");
    CHECK(d.graphText.find("parent.size * 10") != std::string::npos && d.graphText.find("parent.amount") == std::string::npos);
    CHECK(d.validate().empty());

    AssetDefinition fresh = d;
    CHECK(fresh.renameParam("size", "span", false) == 1 && fresh.renames.count("size") == 0 && fresh.renames["amount"] == "size");
    std::string message;
    CHECK(throwsRuntimeError([&] { d.renameParam("nope", "x"); }, &message) && message.find("not a parameter") != std::string::npos);
    CHECK(throwsRuntimeError([&] { d.renameParam("size", "other"); }, &message) && message.find("already exists") != std::string::npos);
    CHECK(throwsRuntimeError([&] { d.renameParam("size", "bad name"); }));
    CHECK(d.params[0].name == "size");
}

TEST(applyingADefinitionReplacesTheContentsAndParametersOfASubnet) {
    SubnetRig rig;
    subnetOf(rig.net, rig.sub).addSpareFloat("amount", 2.0f);
    subnetOf(rig.net, rig.sub).addSpareFloat("gone", 1.0f);
    SubnetRig other;
    subnetOf(other.net, other.sub).addSpareFloat("amount", 7.0f);
    subnetOf(other.net, other.sub).addSpareFloat("added", 4.0f);
    other.inner().node(other.xf).setFloat("tx", 9.0f);
    const AssetDefinition d = AssetDefinition::fromNode(other.net, other.sub, "applyToAsset");

    d.applyTo(rig.net, rig.sub);
    CHECK(subnetOf(rig.net, rig.sub).isSpare("added") && !subnetOf(rig.net, rig.sub).isSpare("gone"));
    CHECK(subnetOf(rig.net, rig.sub).baseFloat("amount") == 2.0f);   // an existing parameter keeps its value
    CHECK(subnetOf(rig.net, rig.sub).baseFloat("added") == 4.0f);
    CHECK_NEAR(rig.net.cook(rig.out)->x()[0], 9.0f);
    CHECK(throwsRuntimeError([&] { d.applyTo(rig.net, rig.line); }));
}

TEST(theAssetLibraryInstallsSavesUpdatesAndUninstalls) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "jitnodes_asset_library_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    AssetLibrary& library = AssetLibrary::instance();
    int notified = 0;
    const int listener = library.addListener([&] { ++notified; });

    SubnetRig rig;
    subnetOf(rig.net, rig.sub).addSpareFloat("amount", 2.0f);
    rig.inner().node(rig.xf).setExpression("tx", "parent.amount");
    AssetDefinition d = AssetDefinition::fromNode(rig.net, rig.sub, "libraryTestAsset");
    d.label = "Library test";
    const std::string path = (dir / "libraryTestAsset.asset.json").string();
    library.save(d, path);
    CHECK(library.has("libraryTestAsset") && d.version == 1 && d.source == path && fs::exists(path) && notified == 1);

    Network net(1);
    const NodeId line = net.addNode("line");
    const NodeId a = net.addNode("libraryTestAsset");
    net.connect(line, a);
    std::vector<AssetEntry> entries = library.list(&net);
    const auto find = [&](const std::string& name) {
        return std::find_if(entries.begin(), entries.end(), [&](const AssetEntry& e) { return e.name == name; });
    };
    CHECK(find("libraryTestAsset") != entries.end());
    CHECK(find("libraryTestAsset")->instances == 1 && find("libraryTestAsset")->outdated == 0 && !find("libraryTestAsset")->embedded);
    CHECK(find("libraryTestAsset")->label == "Library test" && find("libraryTestAsset")->params == std::vector<std::string>{"amount"});

    // Saving again writes the next version and leaves the instance outdated until it is updated.
    AssetDefinition edited = library.definition("libraryTestAsset");
    edited.params[0].defaultValue = 4.0f;
    library.save(edited, path);
    CHECK(edited.version == 2 && NodeRegistry::instance().assetVersion("libraryTestAsset") == 2);
    entries = library.list(&net);
    CHECK(find("libraryTestAsset")->outdated == 1);
    CHECK(library.updateInstances(net, "libraryTestAsset") == 1);
    entries = library.list(&net);
    CHECK(find("libraryTestAsset")->outdated == 0);

    std::string message;
    CHECK(throwsRuntimeError([&] { library.uninstall("libraryTestAsset", &net); }, &message) && message.find("1 instance") != std::string::npos);
    net.removeNode(a);
    library.uninstall("libraryTestAsset", &net);
    CHECK(!library.has("libraryTestAsset") && fs::exists(path));
    CHECK(throwsRuntimeError([&] { library.uninstall("libraryTestAsset"); }));

    // Reinstalling from the file, reloading, and the directory scan.
    library.installFile(path);
    CHECK(library.has("libraryTestAsset") && library.source("libraryTestAsset") == path);
    library.reload("libraryTestAsset");
    {
        AssetDefinition second = AssetDefinition::fromNode(rig.net, rig.sub, "libraryTestAssetTwo");
        std::ofstream(dir / "two.asset.json", std::ios::binary) << second.toText();
        std::ofstream(dir / "broken.asset.json", std::ios::binary) << "{ not an asset";
        std::ofstream(dir / "ignored.txt", std::ios::binary) << "x";
    }
    size_t installed = 0;
    const std::string error = library.installDirectory(dir.string(), &installed);
    CHECK(installed == 2 && error.find("broken.asset.json") != std::string::npos);   // the broken file is reported, the others still load
    CHECK(library.has("libraryTestAssetTwo"));
    CHECK(library.installDirectory((dir / "missing").string()).empty());

    // Apply registers in memory: the asset is "embedded" (no file) until it is saved.
    AssetDefinition memory = AssetDefinition::fromNode(rig.net, rig.sub, "libraryMemoryAsset");
    library.apply(memory);
    entries = library.list();
    CHECK(find("libraryMemoryAsset")->embedded);
    AssetDefinition invalid = memory;
    invalid.name = "line";
    CHECK(throwsRuntimeError([&] { library.apply(invalid); }));
    CHECK(throwsRuntimeError([&] { library.save(invalid, (dir / "line.asset.json").string()); }) && !fs::exists(dir / "line.asset.json"));
    CHECK(throwsRuntimeError([&] { library.reload("libraryMemoryAsset"); }));

    library.removeListener(listener);
    const int before = notified;
    library.uninstall("libraryMemoryAsset");
    CHECK(notified == before);
    for (const char* name : {"libraryTestAsset", "libraryTestAssetTwo"}) {
        library.uninstall(name);
    }
    fs::remove_all(dir);
}

// One open polyline whose points are (x, y, t); extra point attributes are added by the caller.
std::shared_ptr<Geometry> strokeOf(const std::vector<std::array<float, 3>>& samples) {
    auto geo = std::make_shared<Geometry>();
    geo->resizePoints(samples.size());
    float* t = geo->attrib(AttribClass::point, "t").floats().data();
    for (size_t i = 0; i < samples.size(); ++i) {
        geo->x()[i] = samples[i][0];
        geo->y()[i] = samples[i][1];
        t[i] = samples[i][2];
    }
    Primitive prim;
    prim.start = 0;
    prim.count = static_cast<uint32_t>(samples.size());
    geo->addPrimitive(prim);
    return geo;
}

NodeId strokeSource(Network& net, std::shared_ptr<Geometry> geo) {
    NodeId id = net.addNode("geometry");
    net.node(id).setGeometryParam("geometry", geo);
    return id;
}

TEST(oscillatorWaveformsAtKnownPhases) {
    Network net(1);
    auto geo = std::make_shared<Geometry>();
    geo->resizePoints(5);
    float* d = geo->attrib(AttribClass::point, "d").floats().data();
    for (int i = 0; i < 5; ++i) {
        d[i] = 0.25f * static_cast<float>(i);  // 0, 1/4, 1/2, 3/4, 1 cycle
    }
    NodeId src = strokeSource(net, geo);
    NodeId osc = net.addNode("oscillator");
    net.connect(src, osc);
    net.node(osc).setString("driver", "d");
    net.node(osc).setString("name", "o");

    auto values = [&](const char* wave) {
        net.node(osc).setString("waveform", wave);
        auto out = net.cook(osc);
        const AttribArray* o = out->findAttrib(AttribClass::point, "o");
        CHECK(o != nullptr);
        std::vector<float> v;
        for (size_t i = 0; i < 5; ++i) {
            v.push_back(o->at(i));
        }
        return v;
    };
    auto sine = values("sine");
    CHECK_NEAR(sine[0], 0.0f);
    CHECK_NEAR(sine[1], 1.0f);
    CHECK_NEAR(sine[3], -1.0f);
    auto saw = values("saw");
    CHECK_NEAR(saw[0], -1.0f);
    CHECK_NEAR(saw[2], 0.0f);
    CHECK_NEAR(saw[4], -1.0f);
    auto square = values("square");
    CHECK_NEAR(square[0], 1.0f);
    CHECK_NEAR(square[1], 1.0f);
    CHECK_NEAR(square[2], -1.0f);
    auto triangle = values("triangle");
    CHECK_NEAR(triangle[0], 0.0f);
    CHECK_NEAR(triangle[1], 1.0f);
    CHECK_NEAR(triangle[3], -1.0f);

    net.node(osc).setFloat("amplitude", 2.0f);
    net.node(osc).setFloat("offset", 10.0f);
    net.node(osc).setFloat("phase", 0.25f);
    net.node(osc).setString("waveform", "sine");
    CHECK_NEAR(net.cook(osc)->findAttrib(AttribClass::point, "o")->at(0), 12.0f);

    net.node(osc).setString("waveform", "noise");
    net.node(osc).setFloat("amplitude", 1.0f);
    net.node(osc).setFloat("offset", 0.0f);
    net.node(osc).setFloat("phase", 0.0f);
    auto noise = values("noise");
    for (float v : noise) {
        CHECK(v >= -1.0f && v <= 1.0f);
    }
    CHECK(values("noise") == noise);
}

TEST(oscillatorDriverTimeDetailAndErrors) {
    Network net(1);
    auto geo = strokeOf({{0, 0, 0}, {1, 0, 1}, {2, 0, 2}});
    NodeId src = strokeSource(net, geo);
    NodeId osc = net.addNode("oscillator");
    net.connect(src, osc);
    CHECK(net.node(osc).dependsOnPlayhead());
    net.setTime(0.25);
    auto out = net.cook(osc);  // sine of 0.25 cycles, one value on every point
    for (size_t i = 0; i < 3; ++i) {
        CHECK_NEAR(out->findAttrib(AttribClass::point, "osc")->at(i), 1.0f);
    }

    net.node(osc).setString("class", "detail");
    net.setTime(0.5);
    CHECK_NEAR(net.cook(osc)->detail("osc", 99.0f), 0.0f);

    net.node(osc).setString("class", "point");
    net.node(osc).setString("driver", "t");
    CHECK(!net.node(osc).dependsOnPlayhead());
    net.node(osc).setFloat("frequency", 0.25f);
    CHECK_NEAR(net.cook(osc)->findAttrib(AttribClass::point, "osc")->at(1), 1.0f);

    net.node(osc).setString("driver", "missing");
    bool threw = false;
    try {
        net.cook(osc);
    } catch (const std::exception& e) {
        threw = std::string(e.what()).find("missing") != std::string::npos;
    }
    CHECK(threw);
    net.node(osc).setString("driver", "t");
    net.node(osc).setString("waveform", "wobble");
    threw = false;
    try {
        net.cook(osc);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(oscillatorSurvivesSerialisation) {
    Network net(1);
    NodeId osc = net.addNode("oscillator");
    net.node(osc).setString("waveform", "triangle");
    net.node(osc).setString("driver", "s");
    net.node(osc).setFloat("frequency", 4.0f);
    Network loaded(1);
    loadNetwork(loaded, saveNetwork(net));
    CHECK(loaded.nodeIds().size() == 1);
    NodeId again = loaded.nodeIds()[0];
    CHECK(loaded.node(again).getString("waveform") == "triangle" && loaded.node(again).getString("driver") == "s");
    CHECK_NEAR(loaded.node(again).getFloat("frequency"), 4.0f);
}

TEST(pathMotionOfASteadyStroke) {
    // 10 units per second along x, then a stop with no time between two samples, then a turn upwards.
    auto geo = strokeOf({{0, 0, 0.0f}, {1, 0, 0.1f}, {2, 0, 0.2f}, {3, 0, 0.3f}});
    PathMotionParams params;
    params.smoothing = 1.0f;
    PathMotion::compute(*geo, params);
    const auto& s = geo->pointAttrib("s");
    const auto& speed = geo->pointAttrib("speed");
    const auto& heading = geo->pointAttrib("heading");
    CHECK_NEAR(s[3], 3.0f);
    for (size_t i = 0; i < 4; ++i) {
        CHECK_NEAR(speed[i], 10.0f);  // the first point takes the second's values
        CHECK_NEAR(heading[i], 0.0f);
        CHECK_NEAR(geo->pointAttrib("accel")[i], 0.0f);
    }
    CHECK_NEAR(geo->pointAttrib("dt")[0], 0.0f);
    CHECK_NEAR(geo->pointAttrib("dt")[2], 0.1f);

    auto turn = strokeOf({{0, 0, 0.0f}, {1, 0, 0.1f}, {1, 1, 0.2f}});
    PathMotion::compute(*turn, params);
    CHECK_NEAR(turn->pointAttrib("heading")[2], 1.5707963f);
    CHECK_NEAR(turn->pointAttrib("vy")[2], 10.0f);
    CHECK_NEAR(turn->pointAttrib("ax")[2], -100.0f);
}

TEST(pathMotionSmoothsAndHandlesStops) {
    auto geo = strokeOf({{0, 0, 0.0f}, {1, 0, 0.1f}, {1, 0, 0.1f}, {1, 0, 0.2f}});
    PathMotionParams params;
    params.smoothing = 0.5f;
    PathMotion::compute(*geo, params);
    const auto& speed = geo->pointAttrib("speed");
    CHECK_NEAR(speed[1], 10.0f);
    CHECK_NEAR(speed[2], 10.0f);   // no time between samples 1 and 2: repeats
    CHECK_NEAR(speed[3], 5.0f);    // 0.5 * 0 + 0.5 * 10
    CHECK(geo->pointAttrib("accel")[3] < 0.0f);

    auto noTime = std::make_shared<Geometry>();
    noTime->resizePoints(2);
    bool threw = false;
    try {
        PathMotion::compute(*noTime, params);
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("'t'") != std::string::npos;
    }
    CHECK(threw);
    params.smoothing = 0.0f;
    threw = false;
    try {
        PathMotion::compute(*geo, params);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(pathMotionNodeWritesTheAttributes) {
    Network net(1);
    NodeId src = strokeSource(net, strokeOf({{0, 0, 0.0f}, {2, 0, 0.5f}, {4, 0, 1.0f}}));
    NodeId motion = net.addNode("pathMotion");
    net.connect(src, motion);
    auto out = net.cook(motion);
    CHECK_NEAR(out->findAttrib(AttribClass::point, "speed")->at(2), 4.0f);
    CHECK_NEAR(out->findAttrib(AttribClass::point, "s")->at(2), 4.0f);
}

TEST(removeAttribKeepsPositions) {
    Geometry geo;
    geo.resizePoints(2);
    geo.attrib(AttribClass::point, "a");
    CHECK(geo.removeAttrib(AttribClass::point, "a"));
    CHECK(!geo.removeAttrib(AttribClass::point, "a"));
    bool threw = false;
    try {
        geo.removeAttrib(AttribClass::point, "x");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(strokeCleanRemovesAndKeepsAttributes) {
    Network net(1);
    auto geo = strokeOf({{0, 0, 0}, {1, 0, 1}, {2, 0, 2}});
    for (const char* name : {"tiltX", "tiltY", "twist", "pressure"}) {
        geo->attrib(AttribClass::point, name);
    }
    NodeId src = strokeSource(net, geo);
    NodeId clean = net.addNode("strokeClean");
    net.connect(src, clean);

    auto untouched = net.cook(clean);
    CHECK(untouched->pointCount() == 3 && untouched->hasPointAttrib("tiltX"));

    net.node(clean).setString("remove", "tilt* twist");
    auto out = net.cook(clean);
    CHECK(!out->hasPointAttrib("tiltX") && !out->hasPointAttrib("tiltY") && !out->hasPointAttrib("twist"));
    CHECK(out->hasPointAttrib("pressure") && out->hasPointAttrib("t") && out->pointCount() == 3);

    net.node(clean).setString("remove", "");
    net.node(clean).setString("keep", "pressure");
    out = net.cook(clean);
    CHECK(out->hasPointAttrib("pressure") && !out->hasPointAttrib("t") && !out->hasPointAttrib("tiltX"));
    CHECK(out->hasPointAttrib("x") && out->hasPointAttrib("y"));

    net.node(clean).setString("remove", "x");  // positions are never removed
    CHECK(net.cook(clean)->hasPointAttrib("x"));
}

TEST(strokeCleanSimplifiesWithinTolerance) {
    // A line with a tiny bump, then a circle.
    std::vector<std::array<float, 3>> samples;
    for (int i = 0; i <= 20; ++i) {
        samples.push_back({static_cast<float>(i), i == 10 ? 0.05f : 0.0f, 0.01f * static_cast<float>(i)});
    }
    Network net(1);
    NodeId src = strokeSource(net, strokeOf(samples));
    NodeId clean = net.addNode("strokeClean");
    net.connect(src, clean);
    net.node(clean).setFloat("simplify", 0.1f);
    auto out = net.cook(clean);
    CHECK(out->pointCount() == 2);
    CHECK(out->primitiveCount() == 1 && out->primitives()[0].count == 2);
    CHECK_NEAR(out->x()[0], 0.0f);
    CHECK_NEAR(out->x()[1], 20.0f);
    CHECK_NEAR(out->findAttrib(AttribClass::point, "t")->at(1), 0.2f);  // the kept end keeps its own value

    net.node(clean).setFloat("simplify", 0.01f);
    const size_t tighter = net.cook(clean)->pointCount();
    CHECK(tighter > 2 && tighter < 21);  // the bump survives a tighter tolerance

    samples.clear();
    for (int i = 0; i <= 100; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 100.0f;
        samples.push_back({10.0f * std::cos(a), 10.0f * std::sin(a), static_cast<float>(i)});
    }
    Network circle(1);
    NodeId source = strokeSource(circle, strokeOf(samples));
    NodeId cleaned = circle.addNode("strokeClean");
    circle.connect(source, cleaned);
    circle.node(cleaned).setFloat("simplify", 0.2f);
    auto kept = circle.cook(cleaned);
    CHECK(kept->pointCount() < 101 && kept->pointCount() > 10);
    // Every original point lies within the tolerance of the simplified polyline.
    for (const auto& sample : samples) {
        float best = 1e9f;
        for (size_t i = 0; i + 1 < kept->pointCount(); ++i) {
            const float ax = kept->x()[i], ay = kept->y()[i], bx = kept->x()[i + 1], by = kept->y()[i + 1];
            const float dx = bx - ax, dy = by - ay;
            const float t = std::clamp(((sample[0] - ax) * dx + (sample[1] - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
            best = std::min(best, std::hypot(sample[0] - ax - t * dx, sample[1] - ay - t * dy));
        }
        CHECK(best <= 0.2f + 1e-3f);
    }
}

TEST(strokeCleanSimplifyKeepsPointsWhereTheFollowedAttributeChanges) {
    auto geo = strokeOf(std::vector<std::array<float, 3>>(21, {0.0f, 0.0f, 0.0f}));
    {
        float* w = geo->attrib(AttribClass::point, "w").floats().data();
        for (int i = 0; i <= 20; ++i) {
            geo->x()[i] = static_cast<float>(i);
            w[i] = i > 7 && i < 13 ? 1.0f : 0.25f;
        }
    }
    Network net(1);
    NodeId src = strokeSource(net, geo);
    NodeId clean = net.addNode("strokeClean");
    net.connect(src, clean);
    net.node(clean).setFloat("simplify", 0.5f);
    auto out = net.cook(clean);
    CHECK(out->pointCount() == 6);   // the ends and both sides of the two steps
    const AlignedArray<float>& w = *out->findPointAttrib("w");
    CHECK_NEAR(out->x()[2], 8.0f);
    CHECK_NEAR(w[1], 0.25f);
    CHECK_NEAR(w[2], 1.0f);
    CHECK_NEAR(w[3], 1.0f);

    net.node(clean).setString("follow", "");
    CHECK(net.cook(clean)->pointCount() == 2);
    net.node(clean).setString("follow", "w");
    net.node(clean).setFloat("followTolerance", 0.0f);
    CHECK(net.cook(clean)->pointCount() == 2);
    net.node(clean).setFloat("followTolerance", 1.0f);
    CHECK(net.cook(clean)->pointCount() == 2);
}

TEST(strokeCleanDropsCloseAndQuickPointsButKeepsTheEnds) {
    Network net(1);
    auto geo = strokeOf({{0, 0, 0.00f}, {0.1f, 0, 0.01f}, {0.2f, 0, 0.02f}, {5, 0, 0.5f}, {5.05f, 0, 0.51f}});
    NodeId src = strokeSource(net, geo);
    NodeId clean = net.addNode("strokeClean");
    net.connect(src, clean);
    net.node(clean).setFloat("minDistance", 1.0f);
    auto out = net.cook(clean);
    CHECK(out->pointCount() == 2);
    CHECK_NEAR(out->x()[0], 0.0f);
    CHECK_NEAR(out->x()[1], 5.05f);  // the end replaces the point before it that was too close

    net.node(clean).setFloat("keepEnds", 0.0f);
    out = net.cook(clean);
    CHECK(out->pointCount() == 2);
    CHECK_NEAR(out->x()[1], 5.0f);

    net.node(clean).setFloat("keepEnds", 1.0f);
    net.node(clean).setFloat("minDistance", 0.0f);
    net.node(clean).setFloat("minTime", 0.1f);
    out = net.cook(clean);
    CHECK(out->pointCount() == 2);
    CHECK_NEAR(out->findAttrib(AttribClass::point, "t")->at(1), 0.51f);

    net.node(clean).setString("timeAttr", "nothing");
    bool threw = false;
    try {
        net.cook(clean);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(strokeCleanLeavesShortAndCurvedPrimitivesWhole) {
    Network net(1);
    auto geo = strokeOf({{0, 0, 0.0f}, {0.01f, 0, 0.1f}});  // two points: never thinned
    NodeId src = strokeSource(net, geo);
    NodeId clean = net.addNode("strokeClean");
    net.connect(src, clean);
    net.node(clean).setFloat("minDistance", 5.0f);
    net.node(clean).setFloat("simplify", 5.0f);
    CHECK(net.cook(clean)->pointCount() == 2);
}

TEST(rampParsesPrintsAndSorts) {
    Ramp ramp = Ramp::parse("1 1 linear, 0 0 monotone; 0.5 0.25");
    CHECK(ramp.size() == 3);
    CHECK_NEAR(ramp.points()[0].pos, 0.0f);
    CHECK(ramp.points()[0].interp == RampInterp::monotone);
    CHECK(ramp.points()[1].interp == RampInterp::linear);  // omitted: linear
    CHECK(ramp.toText() == "0 0 monotone, 0.5 0.25 linear, 1 1 linear");
    CHECK(Ramp::parse(ramp.toText()).toText() == ramp.toText());
    CHECK(Ramp::interpFromName("Catmull-Rom") == RampInterp::catmullRom);
    CHECK(Ramp().toText() == "0 0 linear, 1 1 linear");

    for (const char* bad : {"", "0", "a b", "0 1 wobbly", "0 1 linear extra", "0 nan"}) {
        CHECK(!Ramp::check(bad).empty());
    }
    CHECK(Ramp::check("0 0, 1 1").empty());
    bool threw = false;
    try {
        Ramp::parse("0 0, 1 oops");
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("point 2") != std::string::npos;
    }
    CHECK(threw);
}

TEST(rampInterpolationKinds) {
    CHECK_NEAR(Ramp::parse("0 0 linear, 1 10").eval(0.25f), 2.5f);
    CHECK_NEAR(Ramp::parse("0 0 linear, 1 10").eval(-5.0f), 0.0f);   // the ends hold
    CHECK_NEAR(Ramp::parse("0 0 linear, 1 10").eval(5.0f), 10.0f);
    CHECK_NEAR(Ramp::parse("0 3 constant, 1 10").eval(0.99f), 3.0f);
    CHECK_NEAR(Ramp::parse("0 0 smooth, 1 10").eval(0.5f), 5.0f);
    CHECK_NEAR(Ramp::parse("0 0 smooth, 1 10").eval(0.25f), 1.5625f);  // 3t^2 - 2t^3 at 0.25 is 0.15625

    // A step: two points at one position.
    Ramp step = Ramp::parse("0 0 constant, 0.5 0 linear, 0.5 1 linear, 1 1");
    CHECK_NEAR(step.eval(0.49f), 0.0f);
    CHECK_NEAR(step.eval(0.5f), 1.0f);

    // Through every point, whatever the interpolation.
    for (const char* kind : {"catmullRom", "monotone", "smooth"}) {
        Ramp r = Ramp::parse(std::string("0 0 ") + kind + ", 0.3 0.8 " + kind + ", 0.6 0.9 " + kind + ", 1 0.2");
        CHECK_NEAR(r.eval(0.3f), 0.8f);
        CHECK_NEAR(r.eval(0.6f), 0.9f);
    }

    // Monotone data never overshoots under monotone interpolation; Catmull-Rom does.
    Ramp monotone = Ramp::parse("0 0 monotone, 0.5 0.95 monotone, 1 1");
    Ramp cardinal = Ramp::parse("0 0 catmullRom, 0.5 0.95 catmullRom, 1 1");
    float highest = 0.0f, previous = 0.0f;
    bool rises = true;
    for (int i = 0; i <= 200; ++i) {
        const float v = monotone.eval(static_cast<float>(i) / 200.0f);
        highest = std::max(highest, v);
        rises = rises && v >= previous - 1e-6f;
        previous = v;
    }
    CHECK(highest <= 1.0f + 1e-5f && rises);
    float cardinalHighest = 0.0f;
    for (int i = 0; i <= 200; ++i) {
        cardinalHighest = std::max(cardinalHighest, cardinal.eval(static_cast<float>(i) / 200.0f));
    }
    CHECK(cardinalHighest > 1.0f);

    // A flat stretch stays flat under monotone interpolation.
    Ramp flat = Ramp::parse("0 0 monotone, 0.4 0.5 monotone, 0.6 0.5 monotone, 1 1");
    CHECK_NEAR(flat.eval(0.5f), 0.5f);

    CHECK(Ramp().bake(5) == std::vector<float>({0.0f, 0.25f, 0.5f, 0.75f, 1.0f}));
    CHECK(Ramp::parse("0.5 7").size() == 1);
    CHECK_NEAR(Ramp::parse("0.5 7").eval(100.0f), 7.0f);
}

TEST(rampParameterIsAStringThatSurvivesSerialisation) {
    Network net(1);
    NodeId remap = net.addNode("remap");
    CHECK(net.node(remap).isRampParam("ramp") && !net.node(remap).isRampParam("attribute"));
    CHECK(net.node(remap).rampNames() == std::vector<std::string>({"ramp"}));
    net.node(remap).setString("ramp", "0 0 monotone, 0.4 0.9 monotone, 1 1");
    Network loaded(1);
    loadNetwork(loaded, saveNetwork(net));
    NodeId again = loaded.nodeIds()[0];
    CHECK(loaded.node(again).isRampParam("ramp"));
    CHECK(loaded.node(again).getRamp("ramp").size() == 3);
    bool threw = false;
    try {
        loaded.node(again).getRamp("attribute");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(remapShapesPressureAndClamps) {
    Network net(1);
    auto geo = std::make_shared<Geometry>();
    geo->resizePoints(5);
    float* pressure = geo->attrib(AttribClass::point, "pressure").floats().data();
    const float given[] = {-1.0f, 0.0f, 0.5f, 1.0f, 2.0f};
    for (int i = 0; i < 5; ++i) {
        pressure[i] = given[i];
    }
    NodeId src = strokeSource(net, geo);
    NodeId remap = net.addNode("remap");
    net.connect(src, remap);
    auto out = net.cook(remap);  // defaults: pressure -> w through a straight ramp
    const AttribArray* w = out->findAttrib(AttribClass::point, "w");
    CHECK(w != nullptr);
    CHECK_NEAR(w->at(0), 0.0f);  // clamped by the ramp ends
    CHECK_NEAR(w->at(2), 0.5f);
    CHECK_NEAR(w->at(4), 1.0f);

    net.node(remap).setString("ramp", "0 0.2 linear, 0.5 1 linear, 1 0.2");
    net.node(remap).setFloat("outMin", 1.0f);
    net.node(remap).setFloat("outMax", 11.0f);
    w = net.cook(remap)->findAttrib(AttribClass::point, "w");
    CHECK_NEAR(w->at(1), 3.0f);    // 1 + 0.2 * 10
    CHECK_NEAR(w->at(2), 11.0f);

    net.node(remap).setFloat("outMin", 0.0f);
    net.node(remap).setFloat("outMax", 1.0f);
    net.node(remap).setFloat("inMin", 1.0f);  // an inverted range reverses the curve's direction
    net.node(remap).setFloat("inMax", 0.0f);
    net.node(remap).setString("ramp", "0 0, 1 1");
    w = net.cook(remap)->findAttrib(AttribClass::point, "w");
    CHECK_NEAR(w->at(1), 1.0f);
    CHECK_NEAR(w->at(3), 0.0f);

    net.node(remap).setString("output", "");  // empty: overwrite the input attribute
    CHECK_NEAR(net.cook(remap)->findAttrib(AttribClass::point, "pressure")->at(1), 1.0f);
    net.node(remap).setString("output", "w");

    net.node(remap).setFloat("inMax", 1.0f);
    for (const char* problem : {"nothing", "0"}) {
        bool threw = false;
        try {
            if (std::string(problem) == "nothing") {
                net.node(remap).setString("attribute", "nothing");
            } else {
                net.node(remap).setString("attribute", "pressure");
                net.node(remap).setString("ramp", "0");
            }
            net.cook(remap);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
    }
}

TEST(oscillatorRampWaveformIsTheDrawnCurve) {
    Network net(1);
    auto geo = std::make_shared<Geometry>();
    geo->resizePoints(3);
    float* d = geo->attrib(AttribClass::point, "d").floats().data();
    d[0] = 0.0f;
    d[1] = 0.5f;
    d[2] = 1.25f;  // a second cycle: 0.25 into it
    NodeId src = strokeSource(net, geo);
    NodeId osc = net.addNode("oscillator");
    net.connect(src, osc);
    net.node(osc).setString("driver", "d");
    net.node(osc).setString("waveform", "ramp");
    net.node(osc).setString("ramp", "0 0 linear, 0.5 4 linear, 1 0");
    auto out = net.cook(osc);
    const AttribArray* o = out->findAttrib(AttribClass::point, "osc");
    CHECK_NEAR(o->at(0), 0.0f);
    CHECK_NEAR(o->at(1), 4.0f);
    CHECK_NEAR(o->at(2), 2.0f);
    net.node(osc).setFloat("amplitude", 0.5f);
    net.node(osc).setFloat("offset", 1.0f);
    CHECK_NEAR(net.cook(osc)->findAttrib(AttribClass::point, "osc")->at(1), 3.0f);
}

// The extent of a geometry's points in x and y.
std::array<float, 4> pointBox(const Geometry& g) {
    std::array<float, 4> box{1e9f, 1e9f, -1e9f, -1e9f};
    for (uint32_t i = 0; i < g.pointCount(); ++i) {
        box[0] = std::min(box[0], g.x()[i]);
        box[1] = std::min(box[1], g.y()[i]);
        box[2] = std::max(box[2], g.x()[i]);
        box[3] = std::max(box[3], g.y()[i]);
    }
    return box;
}

static std::vector<CurveFit::Point> curveFitArc(int count) {
    std::vector<CurveFit::Point> pts;
    for (int i = 0; i < count; ++i) {
        const double a = 1.5 * double(i) / double(count - 1);
        pts.push_back({100.0 * std::cos(a), 100.0 * std::sin(a)});
    }
    return pts;
}

TEST(curveFitTurnsASmoothArcIntoAFewCurvesThroughItsEnds) {
    const auto samples = curveFitArc(120);
    const auto anchors = CurveFit::fit(samples, 1.0);
    CHECK(anchors.size() >= 2 && anchors.size() < 8);
    CHECK_NEAR(float(anchors.front().point.x), float(samples.front().x));
    CHECK_NEAR(float(anchors.back().point.y), float(samples.back().y));
    CHECK(CurveFit::controlPoints(anchors).size() == 3 * (anchors.size() - 1) + 1);
    CHECK(anchors.front().index == 0 && anchors.back().index == 119);
    for (size_t i = 1; i < anchors.size(); ++i) {
        CHECK(anchors[i].index > anchors[i - 1].index);
    }
}

static std::vector<CurveFit::Point> curveFitWave(int count) {
    std::vector<CurveFit::Point> pts;
    for (int i = 0; i < count; ++i) {
        pts.push_back({double(i) * 2.0, 40.0 * std::sin(double(i) * 0.05)});
    }
    return pts;
}

TEST(curveFitForwardStaysWithinTheToleranceOfEverySample) {
    const auto samples = curveFitWave(300);
    const auto anchors = CurveFit::fit(samples, 1.0, {}, CurveFit::Mode::forward);
    CHECK(anchors.size() >= 3 && anchors.size() < 24);
    CHECK(anchors.front().index == 0 && anchors.back().index == 299);
    const auto control = CurveFit::controlPoints(anchors);
    std::vector<CurveFit::Point> dense;
    for (size_t seg = 0; seg + 3 < control.size(); seg += 3) {
        for (int k = 0; k <= 200; ++k) {
            const double t = k / 200.0, u = 1.0 - t;
            dense.push_back({u * u * u * control[seg].x + 3 * u * u * t * control[seg + 1].x + 3 * u * t * t * control[seg + 2].x + t * t * t * control[seg + 3].x,
                             u * u * u * control[seg].y + 3 * u * u * t * control[seg + 1].y + 3 * u * t * t * control[seg + 2].y + t * t * t * control[seg + 3].y});
        }
    }
    double worst = 0.0;
    for (const CurveFit::Point& s : samples) {
        double best = 1e18;
        for (const CurveFit::Point& p : dense) {
            best = std::min(best, std::hypot(p.x - s.x, p.y - s.y));
        }
        worst = std::max(worst, best);
    }
    CHECK(worst < 1.1);
}

TEST(curveFitForwardKeepsTheEarlyCurvesWhenSamplesAreAddedAtTheEnd) {
    const auto all = curveFitWave(300);
    const auto full = CurveFit::fit(all, 1.0, {}, CurveFit::Mode::forward);
    const std::vector<CurveFit::Point> prefix(all.begin(), all.begin() + 200);
    const auto part = CurveFit::fit(prefix, 1.0, {}, CurveFit::Mode::forward);
    CHECK(part.size() >= 4);
    // Everything but the live tip curves (the last two) is the same, to the pixel.
    for (size_t i = 0; i + 2 < part.size(); ++i) {
        CHECK(i < full.size());
        CHECK(part[i].index == full[i].index);
        CHECK_NEAR(float(part[i].point.x), float(full[i].point.x));
        CHECK_NEAR(float(part[i].out.x), float(full[i].out.x));
        CHECK_NEAR(float(part[i].out.y), float(full[i].out.y));
    }
    // The global fit does not promise that: it is not asserted here.
}

TEST(curveFitForcedBreakpointsCutTheLineWhereAskedAndReportTheirIndex) {
    const auto samples = curveFitArc(120);
    const auto plain = CurveFit::fit(samples, 1.0);
    const auto cut = CurveFit::fit(samples, 1.0, {40, 80});
    bool at40 = false, at80 = false;
    for (const auto& a : cut) {
        at40 = at40 || a.index == 40;
        at80 = at80 || a.index == 80;
    }
    CHECK(at40 && at80);
    CHECK(cut.size() >= plain.size());
    for (const auto& a : cut) {
        if (a.index == 40 || a.index == 80) {
            CHECK(a.hasIn && a.hasOut);
        }
    }
}

TEST(curveFitIgnoresBreakpointsAtTheEndsOrOutOfRange) {
    const auto samples = curveFitArc(60);
    const auto plain = CurveFit::fit(samples, 1.0);
    const auto cut = CurveFit::fit(samples, 1.0, {0, 59, 500});
    CHECK(cut.size() == plain.size());
}

TEST(curveFitGivesWhatThereIsForFewPointsAndKeepsInputIndices) {
    CHECK(CurveFit::fit({}, 1.0).empty());
    CHECK(CurveFit::fit({{1, 1}, {1, 1}}, 1.0).size() == 1);
    const auto two = CurveFit::fit({{0, 0}, {0, 0}, {5, 5}}, 1.0);
    CHECK(two.size() == 2 && two[1].index == 2);
}

static double distanceToPolygon(const std::vector<CurveFit::Point>& poly, double x, double y) {
    double best = 1e300;
    for (size_t i = 0; i + 1 < poly.size(); ++i) {
        const double dx = poly[i + 1].x - poly[i].x, dy = poly[i + 1].y - poly[i].y, len2 = dx * dx + dy * dy;
        const double t = len2 > 0.0 ? std::clamp(((x - poly[i].x) * dx + (y - poly[i].y) * dy) / len2, 0.0, 1.0) : 0.0;
        best = std::min(best, std::hypot(x - (poly[i].x + t * dx), y - (poly[i].y + t * dy)));
    }
    return best;
}

TEST(trajectoryFitOfAStraightLineIsOneCurveWithLinearArcLength) {
    std::vector<CurveFit::Point> pts;
    std::vector<double> w;
    for (int i = 0; i <= 50; ++i) {
        pts.push_back({double(i) * 2.0, 0.0});
        w.push_back(double(i) / 50.0);
    }
    const Trajectory t = TrajectoryFit::fit(pts, {w});
    CHECK(t.anchors.size() == 2);
    CHECK(std::abs(t.length - 100.0) < 1e-6);
    CHECK(t.arc.front() == 0.0 && std::abs(t.arc.back() - 1.0) < 1e-12);
    for (size_t i = 0; i < t.vertices.size(); ++i) {
        CHECK(std::abs(t.attributes[0][i] - t.arc[i]) < 1e-9);
        if (i > 0) {
            CHECK(t.arc[i] > t.arc[i - 1]);
        }
    }
    CHECK(t.anchorVertex.size() == 2 && t.anchorVertex.back() == t.vertices.size() - 1);
}

TEST(trajectoryFitOfACircleStaysWithinToleranceOfTheSamples) {
    std::vector<CurveFit::Point> pts;
    for (int i = 0; i <= 200; ++i) {
        const double a = 4.5 * double(i) / 200.0;
        pts.push_back({50.0 * std::cos(a), 50.0 * std::sin(a)});
    }
    TrajectoryParams params;
    params.tolerance = 0.3;
    const Trajectory t = TrajectoryFit::fit(pts, {}, params);
    CHECK(t.anchors.size() < 12);
    double worst = 0.0;
    for (const auto& p : pts) {
        worst = std::max(worst, distanceToPolygon(t.vertices, p.x, p.y));
    }
    CHECK(worst < params.tolerance + params.flatness + 1e-6);
    CHECK(std::abs(t.length - 225.0) < 2.0);
    CHECK(std::abs(t.arc.back() - 1.0) < 1e-12);
}

TEST(trajectoryFitOfANoisyLineIsFewerCurvesThanSamples) {
    std::vector<CurveFit::Point> pts;
    for (int i = 0; i <= 300; ++i) {
        pts.push_back({double(i), 0.1 * ((i * 7919) % 11 - 5) / 5.0});
    }
    const Trajectory t = TrajectoryFit::fit(pts, {});
    CHECK(t.anchors.size() <= 3);   // was 19 for 301 samples when the end tangent came from 4 noisy samples
}

TEST(trajectoryFitMapsAPressureRampOntoTheVerticesByArcLength) {
    std::vector<CurveFit::Point> pts;
    std::vector<double> w;
    for (int i = 0; i <= 100; ++i) {
        const double a = 3.0 * double(i) / 100.0;
        pts.push_back({40.0 * std::cos(a), 40.0 * std::sin(a)});
        w.push_back(i < 50 ? 0.2 : 1.0);
    }
    const Trajectory t = TrajectoryFit::fit(pts, {w});
    CHECK(std::abs(t.attributes[0].front() - 0.2) < 1e-9);
    CHECK(std::abs(t.attributes[0].back() - 1.0) < 1e-9);
    CHECK(std::abs(t.attributes[0][t.vertices.size() / 4] - 0.2) < 1e-9);
    CHECK(std::abs(t.attributes[0][t.vertices.size() * 3 / 4] - 1.0) < 1e-9);
}

TEST(trajectoryFitKeepsAnAttributeThatBendsAlongAStraightStretch) {
    std::vector<CurveFit::Point> pts;
    std::vector<double> bump, vee;
    for (int i = 0; i <= 100; ++i) {
        pts.push_back({double(i) * 3.0, 0.0});
        bump.push_back(i > 35 && i < 65 ? 1.0 : 0.25);
        vee.push_back(0.1 + 0.018 * std::abs(i - 50));
    }
    const Trajectory t = TrajectoryFit::fit(pts, {bump, vee});
    CHECK(t.anchorVertex.size() == t.anchors.size() && t.anchorVertex.back() == t.vertices.size() - 1);
    for (int i = 0; i <= 100; ++i) {
        const double s = double(i) / 100.0;
        size_t v = 0;
        while (v + 2 < t.vertices.size() && t.arc[v + 1] < s) {
            ++v;
        }
        const double u = (s - t.arc[v]) / (t.arc[v + 1] - t.arc[v]);
        CHECK(std::abs(t.attributes[0][v] + (t.attributes[0][v + 1] - t.attributes[0][v]) * u - bump[i]) < 0.02 + 1e-6);
        CHECK(std::abs(t.attributes[1][v] + (t.attributes[1][v + 1] - t.attributes[1][v]) * u - vee[i]) < 0.02 + 1e-6);
    }
    CHECK(t.vertices.size() < 20);
}

TEST(trajectoryFitHandlesOneTwoAndThreePoints) {
    CHECK(TrajectoryFit::fit({}, {}).vertices.empty());
    const Trajectory one = TrajectoryFit::fit({{3, 4}}, {{0.5}});
    CHECK(one.vertices.size() == 1 && one.length == 0.0 && one.attributes[0][0] == 0.5);
    const Trajectory two = TrajectoryFit::fit({{0, 0}, {10, 0}}, {{0.0, 1.0}});
    CHECK(two.vertices.size() >= 2 && std::abs(two.length - 10.0) < 1e-9);
    CHECK(std::abs(two.attributes[0].back() - 1.0) < 1e-9);
    const Trajectory three = TrajectoryFit::fit({{0, 0}, {5, 1}, {10, 0}}, {{0.0, 0.5, 1.0}});
    CHECK(three.vertices.size() >= 3 && std::abs(three.arc.back() - 1.0) < 1e-12);
    bool threw = false;
    try {
        TrajectoryFit::fit({{0, 0}, {1, 1}}, {{0.0}});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

static double polygonArea(const std::vector<CurveFit::Point>& p) {
    double sum = 0.0;
    for (size_t i = 0; i < p.size(); ++i) {
        const auto& a = p[i];
        const auto& b = p[(i + 1) % p.size()];
        sum += a.x * b.y - b.x * a.y;
    }
    return 0.5 * sum;
}

static std::vector<CurveFit::Point> lineVertices(double length, int steps) {
    std::vector<CurveFit::Point> v;
    for (int i = 0; i <= steps; ++i) {
        v.push_back({length * i / steps, 0.0});
    }
    return v;
}

TEST(brushEnvelopeOfARoundNibOnALineHasParallelBordersAndRoundEnds) {
    const NibBrush nib = NibBrush::oval(10.0, 0.0, 100.0);
    const BrushBorders b = BrushEnvelope::sweep(nib, lineVertices(100.0, 10), {}, false);
    CHECK(b.left.size() == 11 && b.right.size() == 11);
    for (const auto& p : b.left) {
        CHECK(std::abs(p.y - 5.0) < 1e-9);
    }
    for (const auto& p : b.right) {
        CHECK(std::abs(p.y + 5.0) < 1e-9);
    }
    CHECK(b.leftBreaks.empty() && b.rightBreaks.empty());
    const auto poly = BrushEnvelope::polygon(b);
    const double expected = 100.0 * 10.0 + 0.5 * 16 * 25.0 * std::sin(2.0 * 3.14159265358979 / 16.0);
    CHECK(std::abs(std::abs(polygonArea(poly)) - expected) < 1e-6);
}

TEST(brushEnvelopeOfAFlatNibIsThinAcrossItsAngleAndThickAlongIt) {
    const NibBrush flat = NibBrush::oval(20.0, 0.0, 5.0);   // 20 long, 1 thick, horizontal
    const BrushBorders horizontal = BrushEnvelope::sweep(flat, lineVertices(100.0, 5), {}, false);
    CHECK(std::abs(horizontal.left[2].y - horizontal.right[2].y) < 1.0 + 1e-9);
    std::vector<CurveFit::Point> up;
    for (int i = 0; i <= 5; ++i) {
        up.push_back({0.0, 20.0 * i});
    }
    const BrushBorders vertical = BrushEnvelope::sweep(flat, up, {}, false);
    CHECK(std::abs(vertical.left[2].x - vertical.right[2].x) > 19.9);
}

TEST(brushEnvelopeWidensWithAPressureRamp) {
    const NibBrush nib = NibBrush::oval(10.0, 0.0, 100.0);
    std::vector<double> scale;
    for (int i = 0; i <= 10; ++i) {
        scale.push_back(1.0 + 0.2 * i);
    }
    const BrushBorders b = BrushEnvelope::sweep(nib, lineVertices(100.0, 10), scale, false);
    CHECK(std::abs(b.left.front().y - 5.0) < 1e-9);
    CHECK(std::abs(b.left.back().y - 15.0) < 1e-9);
    CHECK(std::abs(b.right.back().y + 15.0) < 1e-9);
}

TEST(brushEnvelopeRoundsTheOutsideOfATurnAndCrossesOnTheInside) {
    const NibBrush nib = NibBrush::oval(10.0, 0.0, 100.0);
    const BrushBorders b = BrushEnvelope::sweep(nib, {{0, 0}, {50, 0}, {50, 50}}, {}, false);   // turns left at (50, 0)
    CHECK(b.leftBreaks.empty() && b.rightBreaks.empty());
    CHECK(b.left.size() == 3);                       // the inside (left) is one crossing point
    CHECK(std::abs(b.left[1].x - 45.0) < 1e-9 && std::abs(b.left[1].y - 5.0) < 1e-9);
    CHECK(b.right.size() > 3);                       // the outside follows the tip round the corner
    for (const auto& p : b.right) {
        CHECK(std::hypot(p.x - 50.0, p.y) <= 5.0 + 1e-9 || std::abs(p.y + 5.0) < 1e-9 || std::abs(p.x - 55.0) < 1e-9);
    }
}

TEST(brushEnvelopeMarksBreakpointsWhereASharpTurnHasNoCrossing) {
    const NibBrush nib = NibBrush::oval(20.0, 0.0, 100.0);
    const BrushBorders b = BrushEnvelope::sweep(nib, {{0, 0}, {30, 0}, {30, 2}, {0, 2}}, {}, false);   // a hairpin much tighter than the tip
    CHECK(!b.leftBreaks.empty() || !b.rightBreaks.empty());
    CHECK((b.leftBreaks.size() + b.rightBreaks.size()) % 2 == 0);
    const BrushBorders smooth = BrushEnvelope::sweep(nib, {{0, 0}, {30, 5}, {60, 20}, {90, 45}}, {}, false);
    CHECK(smooth.leftBreaks.empty() && smooth.rightBreaks.empty());
}

TEST(brushEnvelopeFollowsAFittedCurveWithAboutOnePointPerVertex) {
    const NibBrush nib = NibBrush::oval(8.0, 30.0, 40.0);
    std::vector<CurveFit::Point> arc;
    for (int n : {100, 1000}) {
        arc.clear();
        for (int i = 0; i <= n; ++i) {
            const double a = 3.0 * double(i) / n;
            arc.push_back({80.0 * std::cos(a), 80.0 * std::sin(a)});
        }
        const BrushBorders b = BrushEnvelope::sweep(nib, arc, {}, false);
        CHECK(b.left.size() <= size_t(n + 1) + 40 && b.right.size() <= size_t(n + 1) + 40);
        CHECK(b.leftBreaks.empty() && b.rightBreaks.empty());
    }
}

TEST(brushEnvelopeOfAClosedCircleGivesTwoConcentricLoops) {
    const NibBrush nib = NibBrush::oval(10.0, 0.0, 100.0);
    std::vector<CurveFit::Point> ring;
    for (int i = 0; i < 200; ++i) {
        const double a = 2.0 * 3.14159265358979 * i / 200.0;
        ring.push_back({50.0 * std::cos(a), 50.0 * std::sin(a)});
    }
    const BrushBorders b = BrushEnvelope::sweep(nib, ring, {}, true);
    CHECK(b.closed && b.startCap.empty() && b.endCap.empty());
    double leftMin = 1e9, leftMax = 0, rightMin = 1e9, rightMax = 0;
    for (const auto& p : b.left) {
        leftMin = std::min(leftMin, std::hypot(p.x, p.y));
        leftMax = std::max(leftMax, std::hypot(p.x, p.y));
    }
    for (const auto& p : b.right) {
        rightMin = std::min(rightMin, std::hypot(p.x, p.y));
        rightMax = std::max(rightMax, std::hypot(p.x, p.y));
    }
    // the tip is a 16-gon of radius 5, so a border is at most 5 and at least 5 cos(11.25 degrees) from the path
    CHECK(leftMin > 44.0 && leftMax < 45.1);   // counter-clockwise travel: the left border is the inner loop
    CHECK(rightMin > 54.9 && rightMax < 55.1);
    CHECK(b.leftBreaks.empty() && b.rightBreaks.empty());
}

TEST(brushEnvelopeOfAClosedRectangleRoundsTheOutsideAndCrossesTheInside) {
    const NibBrush nib = NibBrush::oval(10.0, 0.0, 100.0);
    const BrushBorders b = BrushEnvelope::sweep(nib, {{0, 0}, {100, 0}, {100, 60}, {0, 60}}, {}, true);   // counter-clockwise
    CHECK(b.leftBreaks.empty() && b.rightBreaks.empty());
    CHECK(b.left.size() == 4);   // inside: four crossings, the inset rectangle
    for (const auto& p : b.left) {
        CHECK(std::abs(std::abs(p.x - 50.0) - 45.0) < 1e-9 && std::abs(std::abs(p.y - 30.0) - 25.0) < 1e-9);
    }
    CHECK(b.right.size() > 4 + 8);   // outside: the tip's corners round each corner
}

static bool insideConvexHull(std::vector<CurveFit::Point> pts, double px, double py) {
    std::sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    const auto cross3 = [](const CurveFit::Point& o, const CurveFit::Point& a, const CurveFit::Point& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    std::vector<CurveFit::Point> hull(pts.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross3(hull[k - 2], hull[k - 1], pts[i]) <= 0.0) {
            --k;
        }
        hull[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross3(hull[k - 2], hull[k - 1], pts[i - 1]) <= 0.0) {
            --k;
        }
        hull[k++] = pts[i - 1];
    }
    hull.resize(k - 1);
    for (size_t i = 0; i < hull.size(); ++i) {
        if (cross3(hull[i], hull[(i + 1) % hull.size()], {px, py}) < 0.0) {
            return false;
        }
    }
    return true;
}

static bool insidePolygonNonZero(const std::vector<CurveFit::Point>& poly, double px, double py) {
    int winding = 0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const auto& a = poly[i];
        const auto& b = poly[(i + 1) % poly.size()];
        const double side = (b.x - a.x) * (py - a.y) - (px - a.x) * (b.y - a.y);
        if (a.y <= py && b.y > py && side > 0.0) {
            ++winding;
        } else if (a.y > py && b.y <= py && side < 0.0) {
            --winding;
        }
    }
    return winding != 0;
}

TEST(brushEnvelopeCoversTheSameAreaAsTheUnionOfTheOldPerStepHulls) {
    const NibBrush nib = NibBrush::oval(12.0, 35.0, 30.0);
    std::vector<CurveFit::Point> path;
    std::vector<double> scale;
    for (int i = 0; i <= 100; ++i) {
        const double x = double(i);
        path.push_back({x, 12.0 * std::sin(x / 14.0)});
        scale.push_back(1.0 + 0.4 * std::sin(x / 9.0));
    }
    const auto poly = BrushEnvelope::polygon(BrushEnvelope::sweep(nib, path, scale, false));
    size_t tested = 0, differing = 0;
    for (double y = -30.0; y <= 30.0; y += 0.37) {
        for (double x = -15.0; x <= 115.0; x += 0.41) {
            bool inHulls = false;
            for (size_t i = 0; i + 1 < path.size() && !inHulls; ++i) {
                if (std::hypot(x - path[i].x, y - path[i].y) > 14.0) {
                    continue;
                }
                std::vector<CurveFit::Point> both;
                for (const auto& c : nib.corners) {
                    both.push_back({path[i].x + scale[i] * c.x, path[i].y + scale[i] * c.y});
                    both.push_back({path[i + 1].x + scale[i + 1] * c.x, path[i + 1].y + scale[i + 1] * c.y});
                }
                inHulls = insideConvexHull(both, x, y);
            }
            ++tested;
            differing += inHulls != insidePolygonNonZero(poly, x, y) ? 1 : 0;
        }
    }
    CHECK(tested > 20000);
    CHECK(differing * 500 < tested);   // under 0.2 percent, all of it on the boundary
}

TEST(brushEnvelopeOfOnePointIsTheTipAtItsScale) {
    const NibBrush nib = NibBrush::oval(10.0, 0.0, 100.0);
    const BrushBorders b = BrushEnvelope::sweep(nib, {{20, 30}, {20, 30}}, {2.0, 2.0}, false);
    CHECK(b.single);
    const auto poly = BrushEnvelope::polygon(b);
    CHECK(poly.size() == 16);
    CHECK(std::abs(std::abs(polygonArea(poly)) - 0.5 * 16 * 100.0 * std::sin(2.0 * 3.14159265358979 / 16.0)) < 1e-6);
    CHECK(BrushEnvelope::sweep(nib, {}, {}, false).left.empty());
}

static std::array<float, 4> curveBox(const Geometry& g) {
    return pointBox(flattenCurves(g, 0.01f));
}

static int windingAt(const Geometry& flat, float px, float py) {
    int winding = 0;
    for (const Primitive& prim : flat.primitives()) {
        for (uint32_t k = 0; k < prim.count; ++k) {
            const uint32_t a = prim.start + k, b = prim.start + (k + 1) % prim.count;
            const double ax = flat.x()[a], ay = flat.y()[a], bx = flat.x()[b], by = flat.y()[b];
            const double side = (bx - ax) * (py - ay) - (px - ax) * (by - ay);
            if (ay <= py && by > py && side > 0.0) {
                ++winding;
            } else if (ay > py && by <= py && side < 0.0) {
                --winding;
            }
        }
    }
    return winding;
}

TEST(calligraphySweepsAPathIntoOneClosedCubicOfTheNibsSize) {
    Network net(1);
    auto geo = strokeOf({{0, 0, 0}, {50, 0, 1}, {100, 0, 2}});
    NodeId src = strokeSource(net, geo);
    NodeId pen = net.addNode("calligraphy");
    net.connect(src, pen);
    net.node(pen).setFloat("width", 10.0f);
    net.node(pen).setFloat("angle", 0.0f);
    net.node(pen).setFloat("roundness", 100.0f);
    net.node(pen).setFloat("trajectoryTolerance", 1.25f);
    net.node(pen).setFloat("outlineTolerance", 0.33f);
    auto out = net.cook(pen);
    CHECK(out->primitiveCount() == 1);
    const Primitive& prim = out->primitives()[0];
    CHECK(prim.closed && prim.kind == PrimKind::cubic && cubicSegments(prim) > 0);
    CHECK(out->pointCount() < 40);
    const auto box = curveBox(*out);
    CHECK(std::fabs(box[0] + 5.0f) < 0.4f);
    CHECK(std::fabs(box[2] - 105.0f) < 0.4f);
    CHECK(std::fabs(box[1] + 5.0f) < 0.4f);
    CHECK(std::fabs(box[3] - 5.0f) < 0.4f);
    CHECK(!out->hasPointAttrib("t"));
}

TEST(calligraphyKeepsALongPathWithATurnToFewCubics) {
    Network net(1);
    std::vector<std::array<float, 3>> points;
    for (int i = 0; i <= 40; ++i) {
        points.push_back({float(i) * 2.0f, 0.0f, float(i)});
    }
    for (int i = 1; i <= 40; ++i) {
        points.push_back({80.0f, float(i) * 2.0f, float(40 + i)});   // a right angle, 81 samples in all
    }
    NodeId src = strokeSource(net, strokeOf(points));
    NodeId pen = net.addNode("calligraphy");
    net.connect(src, pen);
    net.node(pen).setFloat("width", 6.0f);
    net.node(pen).setFloat("roundness", 100.0f);
    net.node(pen).setFloat("trajectoryTolerance", 0.75f);
    net.node(pen).setFloat("outlineTolerance", 0.2f);
    auto out = net.cook(pen);
    CHECK(out->primitiveCount() == 1);
    CHECK(out->pointCount() < 100);   // 63 measured; was 160 samples' worth of hulls
    const auto box = curveBox(*out);
    CHECK(std::fabs(box[0] + 3.0f) < 0.4f);
    CHECK(std::fabs(box[2] - 83.0f) < 0.4f);
    CHECK(std::fabs(box[3] - 83.0f) < 0.4f);
}

TEST(calligraphyTrajectoryToleranceSmoothsJitterButFollowsABend) {
    std::vector<std::array<float, 3>> line, arc;
    for (int i = 0; i <= 60; ++i) {
        line.push_back({float(i) * 3.0f, (i % 2 == 0 ? 0.1f : -0.1f), float(i)});   // a line with 0.1 jitter
        arc.push_back({float(i) * 3.0f, 40.0f * std::sin(float(i) * 0.1f), float(i)});   // a bend
    }
    Network lineNet(1), arcNet(1);
    NodeId linePen = lineNet.addNode("calligraphy");
    NodeId arcPen = arcNet.addNode("calligraphy");
    lineNet.connect(strokeSource(lineNet, strokeOf(line)), linePen);
    arcNet.connect(strokeSource(arcNet, strokeOf(arc)), arcPen);
    const uint32_t flatPoints = lineNet.cook(linePen)->pointCount();
    const uint32_t bentPoints = arcNet.cook(arcPen)->pointCount();
    CHECK(flatPoints < bentPoints);
    CHECK(flatPoints < 40);
    lineNet.node(linePen).setFloat("trajectoryTolerance", 0.01f);
    lineNet.node(linePen).setFloat("outlineTolerance", 0.01f);   // the outline's own tolerance (0.6 here) would smooth a 0.1 jitter again
    CHECK(lineNet.cook(linePen)->pointCount() > flatPoints);   // a tight fit follows the jitter
}

TEST(calligraphyFlatNibIsThinAcrossItsAngle) {
    Network net(1);
    NodeId src = strokeSource(net, strokeOf({{0, 0, 0}, {100, 0, 1}}));
    NodeId pen = net.addNode("calligraphy");
    net.connect(src, pen);
    net.node(pen).setFloat("width", 20.0f);
    net.node(pen).setFloat("angle", 0.0f);
    net.node(pen).setFloat("roundness", 10.0f);
    net.node(pen).setFloat("trajectoryTolerance", 2.5f);
    net.node(pen).setFloat("outlineTolerance", 0.25f);
    auto box = curveBox(*net.cook(pen));
    CHECK(box[3] - box[1] < 3.0f);   // a horizontal nib drawing a horizontal line is thin
    net.node(pen).setFloat("angle", 90.0f);
    box = curveBox(*net.cook(pen));
    CHECK(box[3] - box[1] > 19.0f);  // turned upright it is as tall as it is long
}

TEST(calligraphyNibFollowsThePressureAttribute) {
    Network net(1);
    auto geo = strokeOf({{0, 0, 0}, {100, 0, 1}});
    float* w = geo->attrib(AttribClass::point, "w").floats().data();
    w[0] = 0.25f;
    w[1] = 1.0f;
    NodeId src = strokeSource(net, geo);
    NodeId pen = net.addNode("calligraphy");
    net.connect(src, pen);
    net.node(pen).setFloat("width", 20.0f);
    net.node(pen).setFloat("roundness", 100.0f);
    auto box = curveBox(*net.cook(pen));
    CHECK(std::fabs(box[0] + 2.5f) < 0.4f);     // the thin start
    CHECK(std::fabs(box[2] - 110.0f) < 0.4f);   // the full end
    CHECK(std::fabs(box[3] - 10.0f) < 0.4f);
}

TEST(calligraphyKeepsShapeKeysAndSweepsCubics) {
    Network net(1);
    auto geo = std::make_shared<Geometry>();
    geo->resizePoints(4);
    const float px[4] = {0, 30, 60, 90}, py[4] = {0, 40, 40, 0};
    for (int i = 0; i < 4; ++i) {
        geo->x()[i] = px[i];
        geo->y()[i] = py[i];
    }
    Primitive prim;
    prim.count = 4;
    prim.kind = PrimKind::cubic;
    geo->addShapePrimitive(prim, "pathA");
    NodeId src = strokeSource(net, geo);
    NodeId pen = net.addNode("calligraphy");
    net.connect(src, pen);
    auto out = net.cook(pen);
    CHECK(out->primitiveCount() == 1);
    CHECK(out->shapes().size() == 1);
    CHECK(out->shapes()[0].key == "pathA");
    out->checkShapes();
    net.node(pen).setFloat("width", 0.0f);
    bool refused = false;
    try {
        net.cook(pen);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
}

TEST(calligraphyOfAClosedPathIsARingWithTheInsideLeftOpen) {
    Network net(1);
    auto geo = std::make_shared<Geometry>();
    geo->resizePoints(24);
    for (int i = 0; i < 24; ++i) {
        const double a = 2.0 * 3.14159265358979 * i / 24.0;
        geo->x()[i] = float(50.0 * std::cos(a));
        geo->y()[i] = float(50.0 * std::sin(a));
    }
    Primitive prim;
    prim.count = 24;
    prim.closed = true;
    geo->addShapePrimitive(prim, "ring");
    NodeId src = strokeSource(net, geo);
    NodeId pen = net.addNode("calligraphy");
    net.connect(src, pen);
    net.node(pen).setFloat("width", 10.0f);
    net.node(pen).setFloat("roundness", 100.0f);
    net.node(pen).setFloat("trajectoryTolerance", 1.25f);
    net.node(pen).setFloat("outlineTolerance", 0.33f);
    auto out = net.cook(pen);
    CHECK(out->primitiveCount() == 2);
    CHECK(out->shapes().size() == 1 && out->shapes()[0].key == "ring");
    const Geometry flat = flattenCurves(*out, 0.01f);
    CHECK(windingAt(flat, 0.0f, 0.0f) == 0);       // the middle is open
    CHECK(windingAt(flat, 50.0f, 0.0f) != 0);      // the stroke itself is filled
    CHECK(windingAt(flat, 0.0f, -50.0f) != 0);
    CHECK(windingAt(flat, 58.0f, 0.0f) == 0);      // and outside it is open too
    const auto box = pointBox(flat);
    CHECK(std::fabs(box[2] - 55.0f) < 0.6f);
}

// A square of side `side` centred on the origin, one closed polygon.
std::shared_ptr<Geometry> squareArt(float side) {
    auto geo = std::make_shared<Geometry>();
    geo->resizePoints(4);
    const float h = side * 0.5f;
    const float px[4] = {-h, h, h, -h}, py[4] = {-h, -h, h, h};
    for (int i = 0; i < 4; ++i) {
        geo->x()[i] = px[i];
        geo->y()[i] = py[i];
    }
    Primitive prim;
    prim.start = 0;
    prim.count = 4;
    prim.closed = true;
    geo->addPrimitive(prim);
    return geo;
}

struct ScatterRig {
    Network net{1};
    NodeId scatter;
    ScatterRig(std::shared_ptr<Geometry> path, std::shared_ptr<Geometry> art) {
        NodeId p = strokeSource(net, path);
        NodeId a = strokeSource(net, art);
        scatter = net.addNode("strokeScatter");
        net.connect(p, scatter, 0);
        net.connect(a, scatter, 1);
    }
};

TEST(strokeScatterPutsCopiesAtTheSpacingAlongThePath) {
    ScatterRig rig(strokeOf({{0, 0, 0}, {100, 0, 1}}), squareArt(10.0f));
    rig.net.node(rig.scatter).setFloat("spacing", 200.0f);   // 20 apart: copies at 0, 20, ... 100
    auto out = rig.net.cook(rig.scatter);
    CHECK(out->primitiveCount() == 6);
    const auto box = pointBox(*out);
    CHECK_NEAR(box[0], -5.0f);
    CHECK_NEAR(box[2], 105.0f);
    CHECK_NEAR(box[1], -5.0f);
    CHECK_NEAR(box[3], 5.0f);
}

TEST(strokeScatterSizeScalesTheCopiesAndTheSpacing) {
    ScatterRig rig(strokeOf({{0, 0, 0}, {100, 0, 1}}), squareArt(10.0f));
    rig.net.node(rig.scatter).setFloat("size", 2.0f);
    auto out = rig.net.cook(rig.scatter);
    CHECK(out->primitiveCount() == 6);   // 20 apart again: size 2 doubles the artwork and the step
    const auto box = pointBox(*out);
    CHECK_NEAR(box[3] - box[1], 20.0f);
}

TEST(strokeScatterTurnsCopiesWithThePathOnlyWhenAsked) {
    ScatterRig rig(strokeOf({{0, 0, 0}, {0, 100, 1}}), squareArt(10.0f));
    rig.net.node(rig.scatter).setFloat("spacing", 2000.0f);   // 200 apart on a 100 long path: one copy at the start
    auto out = rig.net.cook(rig.scatter);
    CHECK(out->primitiveCount() == 1);
    auto box = pointBox(*out);
    CHECK_NEAR(box[2] - box[0], 10.0f);   // a square turned a quarter is still a square
    rig.net.node(rig.scatter).setFloat("followPath", 0.0f);
    rig.net.node(rig.scatter).setFloat("rotation", 45.0f);
    out = rig.net.cook(rig.scatter);
    box = pointBox(*out);
    CHECK(box[2] - box[0] <= 14.15f);
}

TEST(strokeScatterIsRepeatableAndTheSeedChangesIt) {
    ScatterRig rig(strokeOf({{0, 0, 0}, {100, 0, 1}}), squareArt(10.0f));
    Node& node = rig.net.node(rig.scatter);
    node.setFloat("scatter", 100.0f);
    node.setFloat("sizeVariation", 50.0f);
    auto first = rig.net.cook(rig.scatter);
    node.setFloat("size", 1.0f);
    auto again = rig.net.cook(rig.scatter);
    CHECK(sameGeometry(*first, *again));
    node.setFloat("seed", 2.0f);
    CHECK(!sameGeometry(*first, *rig.net.cook(rig.scatter)));
}

TEST(strokeScatterCopiesFollowThePressureAttribute) {
    auto path = strokeOf({{0, 0, 0}, {100, 0, 1}});
    float* w = path->attrib(AttribClass::point, "w").floats().data();
    w[0] = 0.5f;
    w[1] = 0.5f;
    ScatterRig rig(path, squareArt(10.0f));
    rig.net.node(rig.scatter).setFloat("spacing", 1000.0f);
    const auto box = pointBox(*rig.net.cook(rig.scatter));
    CHECK_NEAR(box[3] - box[1], 5.0f);
}

TEST(strokeScatterCopiesTakeThePathsShapeKey) {
    auto path = strokeOf({{0, 0, 0}, {100, 0, 1}});
    path->setShapeKey(0, "pathA");
    ScatterRig rig(path, squareArt(10.0f));
    auto out = rig.net.cook(rig.scatter);
    CHECK(out->primitiveCount() > 1);
    CHECK(out->shapes().size() == 1);
    CHECK(out->shapes()[0].key == "pathA");
    out->checkShapes();
}

TEST(strokeScatterRefusesNoArtworkAndZeroSize) {
    auto refused = [](ScatterRig& rig) {
        try {
            rig.net.cook(rig.scatter);
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    ScatterRig empty(strokeOf({{0, 0, 0}, {100, 0, 1}}), std::make_shared<Geometry>());
    CHECK(refused(empty));
    ScatterRig flat(strokeOf({{0, 0, 0}, {100, 0, 1}}), squareArt(10.0f));
    flat.net.node(flat.scatter).setFloat("size", 0.0f);
    CHECK(refused(flat));
}

class ProbeRenderer : public RendererNode {
public:
    ProbeRenderer() : RendererNode("testProbeRenderer", 1) {}
    Value cookValue(CookContext&, const std::vector<Value>& inputs) override {
        const RenderView view = viewToRender(renderView());
        Image pixels;
        pixels.width = view.width;
        pixels.height = view.height;
        const uint32_t points = inputs[0].geometry ? static_cast<uint32_t>(inputs[0].geometry->pointCount()) : 0u;
        pixels.pixels.assign(static_cast<size_t>(view.width) * static_cast<size_t>(view.height), 0xFF000000u + points);
        auto result = std::make_shared<ImageResult>(view, std::move(pixels));
        setResult(result);
        Value value;
        value.image = result;
        return value;
    }
};

class ImageSinkNode : public ValueNode {
public:
    ImageSinkNode() : ValueNode("testImageSink", 1) {}
    PortType inputType(int) const override { return PortType::image; }
    PortType outputType() const override { return PortType::image; }
    Value cookValue(CookContext&, const std::vector<Value>& inputs) override { return inputs[0]; }
};

void registerImageTestNodes() {
    static const bool registered = (NodeRegistry::instance().registerType("testProbeRenderer", [] { return std::make_unique<ProbeRenderer>(); }),
                                    NodeRegistry::instance().registerType("testImageSink", [] { return std::make_unique<ImageSinkNode>(); }), true);
    (void)registered;
}

RenderView testWindow(double shiftX = 0.0) {
    RenderView view;
    view.transform = Matrix2D::translation(shiftX, 0.0);
    view.width = 10;
    view.height = 10;
    return view;
}

NodeId pointSource(Network& net, size_t points) {
    auto geo = std::make_shared<Geometry>();
    geo->resizePoints(points);
    return strokeSource(net, geo);
}

TEST(typedWiresRefuseAMismatchAndAcceptAMatch) {
    registerImageTestNodes();
    Network net(1);
    const NodeId src = pointSource(net, 3);
    const NodeId probe = net.addNode("testProbeRenderer");
    const NodeId sink = net.addNode("testImageSink");
    const NodeId merge = net.addNode("merge");

    net.connect(src, probe);
    net.connect(probe, sink);
    CHECK(net.inputOf(sink, 0) == probe);

    bool refused = false;
    try {
        net.connect(src, sink);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(net.inputOf(sink, 0) == probe);

    refused = false;
    try {
        net.connect(probe, merge);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(net.inputOf(merge, 0) == invalidNodeId);
}

TEST(imageOutputIsCookedThroughCookImageAndCached) {
    registerImageTestNodes();
    Network net(1);
    const NodeId src = pointSource(net, 3);
    const NodeId probe = net.addNode("testProbeRenderer");
    net.connect(src, probe);
    static_cast<RendererNode&>(net.node(probe)).setView(testWindow());

    ImageHandle first = net.cookImage(probe);
    CHECK(first != nullptr);
    CHECK(net.node(probe).cookCount() == 1);
    CHECK(first->drawnAt().width == 20);
    Image pixels;
    CHECK(first->readPixels(pixels));
    CHECK(pixels.pixels[0] == 0xFF000003u);

    CHECK(net.cookImage(probe) == first);
    CHECK(net.node(probe).cookCount() == 1);

    auto more = std::make_shared<Geometry>();
    more->resizePoints(5);
    net.node(src).setGeometryParam("geometry", more);
    ImageHandle second = net.cookImage(probe);
    CHECK(net.node(probe).cookCount() == 2);
    CHECK(second != first);
    CHECK(second->readPixels(pixels));
    CHECK(pixels.pixels[0] == 0xFF000005u);
}

TEST(cookAndCookImageRefuseTheOtherOutputType) {
    registerImageTestNodes();
    Network net(1);
    const NodeId src = pointSource(net, 1);
    const NodeId probe = net.addNode("testProbeRenderer");
    net.connect(src, probe);
    static_cast<RendererNode&>(net.node(probe)).setView(testWindow());

    bool refused = false;
    try {
        net.cook(probe);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);
    refused = false;
    try {
        net.cookImage(src);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);
    refused = false;
    try {
        net.freezeNode(probe);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);
}

TEST(aPanInsideTheDrawnRegionDoesNotCookAgain) {
    registerImageTestNodes();
    Network net(1);
    const NodeId src = pointSource(net, 2);
    const NodeId probe = net.addNode("testProbeRenderer");
    net.connect(src, probe);
    RendererNode& renderer = static_cast<RendererNode&>(net.node(probe));
    renderer.setView(testWindow());
    net.cookImage(probe);

    renderer.setView(testWindow(-2.0));
    CHECK(!net.node(probe).dirty());
    net.cookImage(probe);
    CHECK(net.node(probe).cookCount() == 1);

    renderer.setView(testWindow(-50.0));
    CHECK(net.node(probe).dirty());
    net.cookImage(probe);
    CHECK(net.node(probe).cookCount() == 2);
}

TEST(aBypassedImageNodePassesItsImageOn) {
    registerImageTestNodes();
    Network net(1);
    const NodeId src = pointSource(net, 2);
    const NodeId probe = net.addNode("testProbeRenderer");
    const NodeId sink = net.addNode("testImageSink");
    net.connect(src, probe);
    net.connect(probe, sink);
    static_cast<RendererNode&>(net.node(probe)).setView(testWindow());
    net.node(sink).setBypassed(true);
    CHECK(net.cookImage(sink) == net.cookImage(probe));
}

TEST(imageWiresSurviveSaveAndLoad) {
    registerImageTestNodes();
    Network net(1);
    const NodeId src = pointSource(net, 2);
    const NodeId probe = net.addNode("testProbeRenderer");
    const NodeId sink = net.addNode("testImageSink");
    net.connect(src, probe);
    net.connect(probe, sink);
    const std::string text = saveNetwork(net);

    Network loaded(1);
    loadNetwork(loaded, text);
    CHECK(loaded.nodeCount() == 3);
    CHECK(loaded.inputOf(sink, 0) == probe);
    CHECK(loaded.inputOf(probe, 0) == src);
    CHECK(loaded.cookImage(sink) != nullptr);
}

std::string exrTestPath(const char* name) { return (std::filesystem::temp_directory_path() / (std::string("jitnodesExr_") + name + ".exr")).string(); }

FloatImage hdrTestImage(int width, int height) {
    FloatImage image;
    image.width = width;
    image.height = height;
    image.linear = true;
    image.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    for (size_t i = 0; i < image.rgba.size() / 4; ++i) {
        image.rgba[i * 4 + 0] = 0.001f * static_cast<float>(i) - 0.25f;   // goes below zero
        image.rgba[i * 4 + 1] = static_cast<float>(i % 7) * 5.5f;          // above 1: HDR light
        image.rgba[i * 4 + 2] = 1e-6f * static_cast<float>(i + 1);         // very small
        image.rgba[i * 4 + 3] = static_cast<float>(i % 5) / 4.0f;          // alpha 0..1
    }
    return image;
}

TEST(exrRoundTripsFloatColourExactly) {
    const FloatImage image = hdrTestImage(40, 20);
    const std::string path = exrTestPath("float");
    ExrImage::fromFloatImage(image).write(path);
    const ExrImage back = ExrImage::read(path);
    std::filesystem::remove(path);

    CHECK(back.width == 40 && back.height == 20);
    CHECK(back.channels.size() == 4);
    CHECK(back.find("R") && back.find("G") && back.find("B") && back.find("A"));
    CHECK(back.find("R")->type == ExrSampleType::float32);
    const FloatImage again = back.toFloatImage();
    CHECK(again.linear);
    CHECK(again.rgba == image.rgba);
}

TEST(exrHalfChannelsKeepHalfPrecisionAndReadBackAsFloats) {
    const FloatImage image = hdrTestImage(40, 20);
    const std::string path = exrTestPath("half");
    ExrImage::fromFloatImage(image, true).write(path);
    const ExrImage back = ExrImage::read(path);
    std::filesystem::remove(path);

    CHECK(back.find("G")->type == ExrSampleType::float16);
    const FloatImage again = back.toFloatImage();
    CHECK(again.rgba.size() == image.rgba.size());
    for (size_t i = 0; i < image.rgba.size(); ++i) {
        const float tolerance = std::fabs(image.rgba[i]) * 1e-3f + 1e-7f;   // a half has 11 significant bits
        CHECK(std::fabs(again.rgba[i] - image.rgba[i]) <= tolerance);
    }
}

TEST(exrCarriesNamedChannelsOfEveryType) {
    const int w = 24, h = 18;
    const size_t count = static_cast<size_t>(w) * h;
    ExrImage image;
    image.width = w;
    image.height = h;
    ExrChannel depth;
    depth.name = "Z";
    depth.values.resize(count);
    ExrChannel id;
    id.name = "id";
    id.type = ExrSampleType::uint32;
    id.ids.resize(count);
    ExrChannel mask;
    mask.name = "mask";
    mask.type = ExrSampleType::float16;
    mask.values.resize(count);
    ExrChannel red;
    red.name = "R";
    red.values.resize(count);
    for (size_t i = 0; i < count; ++i) {
        depth.values[i] = 1000.0f + 0.5f * static_cast<float>(i);
        id.ids[i] = 16777217u + static_cast<uint32_t>(i) * 100000u;   // 2^24 + 1: a float cannot hold it
        mask.values[i] = (i % 2) ? 1.0f : 0.0f;
        red.values[i] = static_cast<float>(i);
    }
    image.channels = {mask, red, id, depth};   // not in alphabetical order on purpose

    for (bool compress : {true, false}) {
        const std::string path = exrTestPath(compress ? "named" : "namedRaw");
        image.write(path, compress);
        const ExrImage back = ExrImage::read(path);
        std::filesystem::remove(path);
        CHECK(back.channels.size() == 4);
        CHECK(back.find("Z") && back.find("id") && back.find("mask") && back.find("R"));
        CHECK(back.find("Z")->values == depth.values);
        CHECK(back.find("R")->values == red.values);
        CHECK(back.find("mask")->values == mask.values);
        CHECK(back.find("id")->type == ExrSampleType::uint32);
        CHECK(back.find("id")->ids == id.ids);
        CHECK(back.find("mask")->type == ExrSampleType::float16);
    }
}

TEST(exrSmallPicturesAndSingleChannelsRoundTrip) {
    ExrImage image;
    image.width = 3;
    image.height = 2;
    ExrChannel luma;
    luma.name = "Y";
    luma.values = {0.0f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
    image.channels.push_back(luma);
    const std::string path = exrTestPath("single");
    image.write(path);
    const ExrImage back = ExrImage::read(path);
    std::filesystem::remove(path);
    CHECK(back.channels.size() == 1);
    CHECK(back.find("Y")->values == luma.values);
    bool refused = false;
    try {
        back.toFloatImage();
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);
}

TEST(exrRefusesWhatIsNotAnExrFile) {
    bool refused = false;
    try {
        ExrImage::read(exrTestPath("doesNotExist"));
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);

    Image png;
    png.width = 2;
    png.height = 2;
    png.pixels.assign(4, 0xFF336699u);
    const std::string path = exrTestPath("notExr");
    writePng(path, png);
    refused = false;
    try {
        ExrImage::read(path);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    std::filesystem::remove(path);
    CHECK(refused);

    ExrImage empty;
    refused = false;
    try {
        empty.write(exrTestPath("empty"));
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);
}

TEST(everyRenderResultReadsAsFloatAndAFloatResultKeepsItsRange) {
    Image bytes;
    bytes.width = 1;
    bytes.height = 1;
    bytes.pixels = {0xFF336699u};
    const ImageResult eightBit(testWindow(), bytes);
    FloatImage widened;
    CHECK(eightBit.readFloatPixels(widened));
    CHECK(!widened.linear);
    CHECK_NEAR(widened.rgba[0], 0x33 / 255.0f);
    CHECK_NEAR(widened.rgba[1], 0x66 / 255.0f);
    CHECK_NEAR(widened.rgba[2], 0x99 / 255.0f);
    CHECK_NEAR(widened.rgba[3], 1.0f);

    const FloatImage hdr = hdrTestImage(20, 20);
    const FloatImageResult floatResult(testWindow(), hdr);
    FloatImage kept;
    CHECK(floatResult.readFloatPixels(kept));
    CHECK(kept.rgba == hdr.rgba);
    Image shown;
    CHECK(floatResult.readPixels(shown));
    CHECK(shown.pixels.size() == 400);

    const std::string path = exrTestPath("result");
    ExrImage::fromFloatImage(kept.asLinear()).write(path);
    const FloatImage fromFile = ExrImage::read(path).toFloatImage();
    std::filesystem::remove(path);
    CHECK(fromFile.rgba == hdr.rgba);
}

TEST(floatImageConvertsBetweenSrgbAndLinear) {
    CHECK_NEAR(FloatImage::srgbToLinear(0.5f), 0.21404f);
    CHECK_NEAR(FloatImage::linearToSrgb(FloatImage::srgbToLinear(0.73f)), 0.73f);

    Image bytes;
    bytes.width = 2;
    bytes.height = 1;
    bytes.pixels = {0xFF808080u, 0x80402010u};   // an opaque grey, and a half transparent premultiplied colour
    const FloatImage encoded = FloatImage::fromArgb8(bytes);
    const FloatImage linear = encoded.asLinear();
    CHECK(linear.linear);
    CHECK_NEAR(linear.rgba[0], FloatImage::srgbToLinear(128.0f / 255.0f));
    CHECK_NEAR(linear.rgba[3], 1.0f);
    CHECK_NEAR(linear.rgba[7], encoded.rgba[7]);   // alpha is never converted
    const FloatImage back = linear.asSrgb();
    for (size_t i = 0; i < back.rgba.size(); ++i) {
        CHECK(std::fabs(back.rgba[i] - encoded.rgba[i]) < 1e-5f);
    }
    CHECK(linear.toArgb8().pixels == bytes.pixels);
}

TEST(matrix2dComposesInDrawingOrderAndInverts) {
    const Matrix2D move = Matrix2D::translation(10.0, 20.0);
    const Matrix2D grow = Matrix2D::scaling(2.0);
    const auto moved = move.then(grow).apply(1.0, 1.0);   // move first, then scale
    CHECK_NEAR(static_cast<float>(moved.first), 22.0f);
    CHECK_NEAR(static_cast<float>(moved.second), 42.0f);
    const auto scaled = grow.then(move).apply(1.0, 1.0);
    CHECK_NEAR(static_cast<float>(scaled.first), 12.0f);
    CHECK_NEAR(static_cast<float>(scaled.second), 22.0f);

    const Matrix2D mixed = Matrix2D::rotation(0.7).then(Matrix2D::scaling(3.0, 2.0)).then(Matrix2D::translation(-4.0, 9.0));
    const auto there = mixed.apply(5.0, -3.0);
    const auto back = mixed.inverse().apply(there.first, there.second);
    CHECK_NEAR(static_cast<float>(back.first), 5.0f);
    CHECK_NEAR(static_cast<float>(back.second), -3.0f);
    CHECK(mixed.then(mixed.inverse()).isTranslateScale(1e-9));

    const auto vector = move.applyVector(1.0, 2.0);   // a vector ignores the move
    CHECK_NEAR(static_cast<float>(vector.first), 1.0f);
    CHECK(!Matrix2D::scaling(0.0, 1.0).invertible());
    CHECK(Matrix2D::scaling(0.0, 1.0).inverse() == Matrix2D());
    CHECK(!Matrix2D::rotation(0.3).isTranslateScale());
    CHECK(!Matrix2D::scaling(1.0, -1.0).isTranslateScale());   // a flip needs a new render
}

TEST(renderViewMarginPlacesTheWindowInTheMiddleOfTheDrawnPicture) {
    const RenderView window = testWindow();
    const RenderView drawn = RenderView::withMargin(window, 0.5);
    CHECK(drawn.width == 20 && drawn.height == 20);
    const auto origin = drawn.transform.apply(0.0, 0.0);   // the document origin, which the window shows at its corner
    CHECK_NEAR(static_cast<float>(origin.first), 5.0f);
    CHECK_NEAR(static_cast<float>(origin.second), 5.0f);
    const auto place = drawn.fromWindow(window).apply(0.0, 0.0);   // where the window's corner lands in the drawn picture
    CHECK_NEAR(static_cast<float>(place.first), 5.0f);
    CHECK_NEAR(static_cast<float>(place.second), 5.0f);
    CHECK(drawn.covers(window));
    CHECK(RenderView::withMargin(window, 0.0) == window);
}

TEST(renderViewCoversPansWithinTheMarginAndSmallZoomsOnly) {
    const RenderView window = testWindow();
    const RenderView drawn = RenderView::withMargin(window, 0.5);
    CHECK(drawn.covers(testWindow(-4.0)));    // panned 4 of the 5 pixels of margin
    CHECK(drawn.covers(testWindow(5.0)));     // exactly to the edge
    CHECK(!drawn.covers(testWindow(-6.0)));   // past the margin
    CHECK(!drawn.covers(testWindow(7.0)));

    RenderView zoomed = window;
    zoomed.transform = Matrix2D::scaling(1.1).then(Matrix2D::translation(0.0, 0.0));
    CHECK(drawn.covers(zoomed));              // 10% is within the 15% tolerance and the window still lies inside
    zoomed.transform = Matrix2D::scaling(1.4);
    CHECK(!drawn.covers(zoomed));
    CHECK(drawn.covers(zoomed, 0.5));         // a looser tolerance accepts the same zoom if it fits

    RenderView turned = window;
    turned.transform = Matrix2D::rotation(0.1);
    CHECK(!drawn.covers(turned));
    RenderView flipped = window;
    flipped.transform = Matrix2D::scaling(1.0, -1.0);
    CHECK(!drawn.covers(flipped));

    RenderView transparent = window;
    transparent.transparentPage = true;
    CHECK(!drawn.covers(transparent));        // another kind of page
    RenderView empty;
    CHECK(!empty.valid());
    CHECK(!drawn.covers(empty));
    CHECK(!empty.covers(window));
}

std::string imageOutTestPath(const char* name, const char* extension) {
    return (std::filesystem::temp_directory_path() / (std::string("jitnodesOut_") + name + extension)).string();
}

struct ImageOutRig {
    Network net{1};
    NodeId src = invalidNodeId, probe = invalidNodeId, out = invalidNodeId;
    explicit ImageOutRig(const std::string& path) {
        registerImageTestNodes();
        src = pointSource(net, 3);
        probe = net.addNode("testProbeRenderer");
        out = net.addNode("imageOut");
        net.connect(src, probe);
        net.connect(probe, out);
        static_cast<RendererNode&>(net.node(probe)).setView(testWindow());
        net.node(out).setString("path", path);
    }
    ImageOutNode& node() { return static_cast<ImageOutNode&>(net.node(out)); }
};

std::string readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

TEST(imageOutPassesTheImageOnAndCookingWritesNothing) {
    const std::string path = imageOutTestPath("cookOnly", ".png");
    std::filesystem::remove(path);
    ImageOutRig rig(path);
    CHECK(rig.net.cookImage(rig.out) == rig.net.cookImage(rig.probe));
    CHECK(!std::filesystem::exists(path));
}

TEST(imageOutRenderWritesAPngWithOrWithoutAlpha) {
    const std::string path = imageOutTestPath("png", ".png");
    ImageOutRig rig(path);
    CHECK(rig.node().render(rig.net).get() == path);
    std::string bytes = readFileBytes(path);
    CHECK(bytes.size() > 40 && bytes.compare(1, 3, "PNG") == 0);
    CHECK(static_cast<unsigned char>(bytes[25]) == 6);   // RGBA

    rig.net.node(rig.out).setFloat("alpha", 0.0f);
    rig.node().render(rig.net).get();
    bytes = readFileBytes(path);
    std::filesystem::remove(path);
    CHECK(static_cast<unsigned char>(bytes[25]) == 2);   // RGB
}

TEST(imageOutRenderWritesAnExrInLinearLightAndExpandsTheFrameToken) {
    const std::string base = imageOutTestPath("frame", "");
    ImageOutRig rig(base + "$F4.exr");
    rig.net.setTime(1.0 / rig.net.fps());
    const std::string written = rig.node().render(rig.net).get();
    CHECK(written == base + "0001.exr");

    FloatImage linear = ExrImage::read(written).toFloatImage();
    CHECK(linear.width == 20 && linear.height == 20);
    CHECK_NEAR(linear.rgba[2], FloatImage::srgbToLinear(3.0f / 255.0f));
    CHECK_NEAR(linear.rgba[3], 1.0f);

    rig.net.node(rig.out).setFloat("linear", 0.0f);
    rig.node().render(rig.net).get();
    FloatImage encoded = ExrImage::read(written).toFloatImage();
    std::filesystem::remove(written);
    CHECK_NEAR(encoded.rgba[2], 3.0f / 255.0f);
}

TEST(imageOutHalfAndTheFormatParameterOverrideTheExtension) {
    const std::string path = imageOutTestPath("forced", ".img");
    ImageOutRig rig(path);
    rig.net.node(rig.out).setString("format", "exr");
    rig.net.node(rig.out).setFloat("half", 1.0f);
    rig.node().render(rig.net).get();
    const ExrImage back = ExrImage::read(path);
    std::filesystem::remove(path);
    CHECK(back.find("B")->type == ExrSampleType::float16);
}

TEST(imageOutRefusesAnEmptyPathABadFormatAndNothingWiredIn) {
    ImageOutRig rig("");
    bool refused = false;
    try {
        rig.node().render(rig.net);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);

    rig.net.node(rig.out).setString("path", imageOutTestPath("bad", ".png"));
    rig.net.node(rig.out).setString("format", "bmp");
    refused = false;
    try {
        rig.node().render(rig.net);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);

    rig.net.node(rig.out).setString("format", "auto");
    rig.net.disconnect(rig.out, 0);
    refused = false;
    try {
        rig.node().render(rig.net);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);
}

}  // namespace

// With arguments only the tests whose name contains one of them run (case matters).
int main(int argc, char** argv) {
    int failed = 0;
    size_t ran = 0;
    for (const auto& t : tests()) {
        bool wanted = argc < 2;
        for (int i = 1; i < argc; ++i) {
            wanted = wanted || std::string(t.name).find(argv[i]) != std::string::npos;
        }
        if (!wanted) {
            continue;
        }
        ++ran;
        try {
            t.fn();
            std::printf("[ ok ] %s\n", t.name);
        } catch (const std::exception& e) {
            ++failed;
            std::printf("[FAIL] %s: %s\n", t.name, e.what());
        }
    }
    std::printf("%zu tests, %d failed\n", ran, failed);
    return failed ? 1 : 0;
}
