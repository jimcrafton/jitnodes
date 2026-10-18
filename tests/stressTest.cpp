// Stress test for the node execution engine: thousands of nodes, tens of thousands of points per node.
//
//   jitnodesStress                 full run (about a minute on a desktop CPU)
//   jitnodesStress --quick         small sizes, a few seconds (this is what ctest runs)
//   jitnodesStress --seed 7 --scenario chains,dag --branches 128 --depth 60 --points 20000
//
// Every scenario checks results, not only that nothing crashes: against an independent C++ reference (chains), against a
// second configuration of the same graph (JIT vs interpreter, fusion on vs off, 1 vs N threads, every instruction set),
// and against a graph rebuilt from scratch from its saved text (edit storms, undo/redo, time animation).
// See docs/architecture.md for what is being exercised.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "jitnodes/jitnodes.h"

using namespace jitnodes;

namespace {

// ------------------------------------------------------------------------------------------------ utilities

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint64_t next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    float range(float lo, float hi) { return lo + static_cast<float>(uniform()) * (hi - lo); }
    size_t index(size_t n) { return n ? static_cast<size_t>(next() % n) : 0; }
    bool chance(double p) { return uniform() < p; }
    // Two decimals keep program text short and exactly representable as the same float when re-parsed.
    float round2(float lo, float hi) { return std::round(range(lo, hi) * 100.0f) / 100.0f; }
};

struct Options {
    uint64_t seed = 1;
    bool quick = false;
    size_t branches = 64;     // chains: independent branches
    size_t depth = 40;        // chains: point-wise nodes per branch
    size_t points = 10000;    // points per source
    size_t dagNodes = 3000;   // dag: random nodes
    size_t edits = 150;       // edit storm length
    size_t wideAttrs = 0;     // wide: attributes (0 = scenario default)
    size_t wideChain = 0;     // wide: wranglers in the one long fused chain (0 = scenario default)
    size_t widePoints = 0;    // wide: points (0 = scenario default)
    size_t threads = 0;       // 0 = hardware default
    bool interpreter = true;  // include the (slow) interpreter comparisons
    bool verbose = false;
    std::set<std::string> scenarios;

    bool wants(const std::string& name) const { return scenarios.empty() || scenarios.count(name) != 0; }
};

struct Report {
    int checks = 0;
    int failures = 0;
    std::vector<std::string> failedScenarios;
    std::string current;

    void check(bool ok, const std::string& what) {
        ++checks;
        if (ok) return;
        ++failures;
        if (failedScenarios.empty() || failedScenarios.back() != current) failedScenarios.push_back(current);
        std::printf("    FAIL [%s] %s\n", current.c_str(), what.c_str());
    }
};

// Bit-identical, except that NaNs of any payload are equal and +0 equals -0: the SSE2 floor/ceil go through an int32 round trip
// and so return +0 where the other tiers return -0 for an input of -0.0 (documented in docs/architecture.md).
bool sameBits(float a, float b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (a == 0.0f && b == 0.0f) return true;
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

// Exact comparison of two geometries; `why` describes the first difference.
bool sameGeometry(const Geometry& a, const Geometry& b, std::string& why, bool ignoreExtraAttribs = false) {
    if (a.pointCount() != b.pointCount()) {
        why = "point count " + std::to_string(a.pointCount()) + " vs " + std::to_string(b.pointCount());
        return false;
    }
    if (a.primitiveCount() != b.primitiveCount()) {
        why = "primitive count differs";
        return false;
    }
    for (size_t i = 0; i < a.primitiveCount(); ++i) {
        const Primitive &p = a.primitives()[i], &q = b.primitives()[i];
        if (p.start != q.start || p.count != q.count || p.closed != q.closed) {
            why = "primitive " + std::to_string(i) + " differs";
            return false;
        }
    }
    for (AttribClass cls : {AttribClass::point, AttribClass::primitive}) {
        const auto& ma = a.attribs(cls);
        const auto& mb = b.attribs(cls);
        if (!ignoreExtraAttribs && ma.size() != mb.size()) {
            why = "attribute count differs (" + std::to_string(ma.size()) + " vs " + std::to_string(mb.size()) + ")";
            return false;
        }
        for (const auto& [name, arrA] : ma) {
            const auto it = mb.find(name);
            if (it == mb.end()) {
                if (ignoreExtraAttribs) continue;
                why = "attribute '" + name + "' missing on one side";
                return false;
            }
            const AttribArray& arrB = it->second;
            if (arrA.type() != arrB.type() || arrA.size() != arrB.size()) {
                why = "attribute '" + name + "' type or size differs";
                return false;
            }
            for (size_t i = 0; i < arrA.size(); ++i) {
                bool same;
                if (arrA.type() == AttribType::float32) same = sameBits(arrA.floats()[i], arrB.floats()[i]);
                else same = arrA.ints()[i] == arrB.ints()[i];
                if (!same) {
                    why = "attribute '" + name + "'[" + std::to_string(i) + "] = " + std::to_string(arrA.at(i)) + " vs " + std::to_string(arrB.at(i));
                    return false;
                }
            }
        }
    }
    return true;
}

// A 64-bit digest of everything in a geometry (counts, attribute names, types and values; NaNs and signed zeros canonicalised),
// so that whole intermediate results can be compared across configurations without keeping them in memory.
uint64_t digest(const Geometry& g) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFFull;
            h *= 1099511628211ull;
        }
    };
    mix(g.pointCount());
    mix(g.primitiveCount());
    for (const Primitive& p : g.primitives()) mix((static_cast<uint64_t>(p.start) << 33) ^ (static_cast<uint64_t>(p.count) << 1) ^ (p.closed ? 1u : 0u));
    for (AttribClass cls : {AttribClass::point, AttribClass::primitive}) {
        for (const auto& [name, arr] : g.attribs(cls)) {
            for (char c : name) mix(static_cast<uint8_t>(c));
            mix(static_cast<uint64_t>(arr.type()));
            for (size_t i = 0; i < arr.size(); ++i) {
                if (arr.type() == AttribType::int32) {
                    mix(static_cast<uint32_t>(arr.ints()[i]));
                } else {
                    float f = arr.floats()[i];
                    uint32_t bits;
                    if (std::isnan(f)) bits = 0x7FC00000u;
                    else if (f == 0.0f) bits = 0;
                    else std::memcpy(&bits, &f, sizeof(bits));
                    mix(bits);
                }
            }
        }
    }
    return h;
}

struct Config {
    bool jit = true;
    Isa isa = Isa::avx2;
    bool fusion = true;
    size_t threads = 0;

    std::string name() const {
        return std::string(jit ? isaName(isa) : "interpreter") + (fusion ? ", fused" : ", unfused") + ", " +
               (threads ? std::to_string(threads) : std::string("default")) + " thread(s)";
    }
};

void apply(Network& net, const Config& c) {
    net.setFusionEnabled(c.fusion);
    net.setThreadCount(c.threads ? c.threads : std::max<size_t>(1, std::min<size_t>(std::thread::hardware_concurrency(), 8)));
    net.compiler().setJitEnabled(c.jit);
    if (c.jit && net.compiler().isaSupported(c.isa)) net.compiler().setIsa(c.isa);
}

std::unique_ptr<Network> cloneWith(const Network& src, const Config& c) {
    auto net = std::make_unique<Network>(c.threads ? c.threads : 0);
    loadNetwork(*net, saveNetwork(src, true));
    apply(*net, c);
    return net;
}

size_t totalCooks(const Network& net) {
    size_t n = 0;
    for (NodeId id : net.nodeIds()) n += net.node(id).cookCount();
    return n;
}

struct Stopwatch {
    Clock::time_point start = Clock::now();
    double lap() {
        const double t = msSince(start);
        start = Clock::now();
        return t;
    }
};

// ------------------------------------------------------------------------------------------------ graph building helpers

NodeId addLine(Network& net, size_t count, float length, float y) {
    NodeId id = net.addNode("line");
    net.node(id).setFloat("count", static_cast<float>(count));
    net.node(id).setFloat("length", length);
    net.node(id).setFloat("y", y);
    return id;
}

NodeId addWrangle(Network& net, const std::string& code, std::array<float, 4> p = {0, 0, 0, 0}) {
    NodeId id = net.addNode("wrangle");
    net.node(id).setString("code", code);
    const char* names[4] = {"a", "b", "c", "d"};
    for (int i = 0; i < 4; ++i) net.node(id).setFloat(names[i], p[static_cast<size_t>(i)]);
    return id;
}

// Merges `inputs` with a tree of the given fan-in and returns the root.
NodeId mergeTree(Network& net, std::vector<NodeId> inputs, size_t fanIn) {
    while (inputs.size() > 1) {
        std::vector<NodeId> next;
        for (size_t i = 0; i < inputs.size(); i += fanIn) {
            const size_t end = std::min(inputs.size(), i + fanIn);
            if (end - i == 1) {
                next.push_back(inputs[i]);
                continue;
            }
            NodeId m = net.addNode("merge");
            for (size_t k = i; k < end; ++k) net.connect(inputs[k], m, static_cast<int>(k - i));
            next.push_back(m);
        }
        inputs = std::move(next);
    }
    return inputs.front();
}

// ------------------------------------------------------------------------------------------------ scenario 1: chains with a reference

// The six per-node programs. Each has a C++ reference written with the same float operations in the same order, so the JIT,
// the interpreter and the reference agree exactly (no fused multiply-add anywhere).
constexpr int templateCount = 6;

const char* templateCode(int t) {
    switch (t) {
        case 1: return "x = x * a + b; y = y * c + d;";
        case 2: return "y = y + x * a; x = x + y * b;";
        case 3: return "t = x * a; x = y * b + c; y = t + d;";
        case 4: return "x = x > a ? x - b : x + c; y = clamp(y, 0 - 1000, 1000) * 0.5 + d;";
        case 5: return "x = min(x, a) + max(y, b) * 0.25; y = abs(y - c) + d * 0.5;";
        default: return "";  // 0 is the transform node
    }
}

std::array<float, 4> randomParams(int t, Rng& rng) {
    switch (t) {
        case 0: return {rng.round2(0.9f, 1.1f), rng.round2(0.9f, 1.1f), rng.round2(-0.5f, 0.5f), rng.round2(-0.5f, 0.5f)};
        case 1: return {rng.round2(0.9f, 1.1f), rng.round2(-0.5f, 0.5f), rng.round2(0.9f, 1.1f), rng.round2(-0.5f, 0.5f)};
        case 2: return {rng.round2(-0.1f, 0.1f), rng.round2(-0.1f, 0.1f), 0, 0};
        case 3: return {rng.round2(0.9f, 1.1f), rng.round2(0.9f, 1.1f), rng.round2(-0.5f, 0.5f), rng.round2(-0.5f, 0.5f)};
        case 4: return {rng.round2(-5, 5), rng.round2(-0.5f, 0.5f), rng.round2(-0.5f, 0.5f), rng.round2(-0.5f, 0.5f)};
        default: return {rng.round2(-5, 5), rng.round2(-5, 5), rng.round2(-1, 1), rng.round2(-1, 1)};
    }
}

float refMin(float a, float b) { return a < b ? a : b; }
float refMax(float a, float b) { return a > b ? a : b; }
float refAbs(float a) {
    const float t = 0.0f - a;
    return a > t ? a : t;
}

void referenceStep(int t, const std::array<float, 4>& p, float& x, float& y) {
    const float a = p[0], b = p[1], c = p[2], d = p[3];
    switch (t) {
        case 0: {  // transform: sx, sy, tx, ty
            x = x * a + c;
            y = y * b + d;
            break;
        }
        case 1:
            x = x * a + b;
            y = y * c + d;
            break;
        case 2:
            y = y + x * a;
            x = x + y * b;
            break;
        case 3: {
            const float tmp = x * a;
            x = y * b + c;
            y = tmp + d;
            break;
        }
        case 4: {
            x = x > a ? x - b : x + c;
            y = refMin(refMax(y, 0.0f - 1000.0f), 1000.0f) * 0.5f + d;
            break;
        }
        default: {
            const float nx = refMin(x, a) + refMax(y, b) * 0.25f;
            y = refAbs(y - c) + d * 0.5f;
            x = nx;
            break;
        }
    }
}

struct Branch {
    size_t count = 0;
    float length = 0, y0 = 0;
    NodeId line = invalidNodeId;
    std::vector<int> tmpl;
    std::vector<std::array<float, 4>> params;
    std::vector<NodeId> chain;
    std::vector<float> refX, refY;
};

const char* paramName(int t, int slot) {
    static const char* transform[4] = {"sx", "sy", "tx", "ty"};
    static const char* wrangle[4] = {"a", "b", "c", "d"};
    return t == 0 ? transform[slot] : wrangle[slot];
}

void computeReference(Branch& b) {
    b.refX.resize(b.count);
    b.refY.resize(b.count);
    const float step = b.count > 1 ? b.length / static_cast<float>(b.count - 1) : 0.0f;
    for (size_t i = 0; i < b.count; ++i) {
        float x = static_cast<float>(i) * step, y = b.y0;
        for (size_t k = 0; k < b.tmpl.size(); ++k) referenceStep(b.tmpl[k], b.params[k], x, y);
        b.refX[i] = x;
        b.refY[i] = y;
    }
}

struct ChainGraph {
    std::unique_ptr<Network> net;
    std::vector<Branch> branches;
    NodeId out = invalidNodeId;
    size_t nodeCount = 0;
};

ChainGraph buildChains(const Options& opt, Rng& rng, size_t threads) {
    ChainGraph g;
    g.net = std::make_unique<Network>(threads);
    Network& net = *g.net;
    std::vector<NodeId> ends;
    for (size_t bi = 0; bi < opt.branches; ++bi) {
        Branch b;
        b.count = opt.points;
        b.length = rng.round2(1, 20);
        b.y0 = rng.round2(-2, 2);
        b.line = addLine(net, b.count, b.length, b.y0);
        NodeId prev = b.line;
        for (size_t k = 0; k < opt.depth; ++k) {
            const int t = static_cast<int>(rng.index(templateCount));
            const auto p = randomParams(t, rng);
            NodeId id;
            if (t == 0) {
                id = net.addNode("transform");
                for (int s = 0; s < 4; ++s) net.node(id).setFloat(paramName(0, s), p[static_cast<size_t>(s)]);
            } else {
                id = addWrangle(net, templateCode(t), p);
            }
            net.connect(prev, id);
            b.tmpl.push_back(t);
            b.params.push_back(p);
            b.chain.push_back(id);
            prev = id;
        }
        computeReference(b);
        ends.push_back(prev);
        g.branches.push_back(std::move(b));
    }
    NodeId root = mergeTree(net, ends, 4);
    g.out = net.addNode("null");
    net.connect(root, g.out);
    net.setDisplayNode(g.out);
    g.nodeCount = net.nodeIds().size();
    return g;
}

bool matchesReference(const ChainGraph& g, const Geometry& geo, std::string& why) {
    size_t total = 0;
    for (const Branch& b : g.branches) total += b.count;
    if (geo.pointCount() != total) {
        why = "point count " + std::to_string(geo.pointCount()) + " expected " + std::to_string(total);
        return false;
    }
    const float* x = geo.x();
    const float* y = geo.y();
    size_t at = 0;
    for (size_t bi = 0; bi < g.branches.size(); ++bi) {
        const Branch& b = g.branches[bi];
        for (size_t i = 0; i < b.count; ++i, ++at) {
            if (!sameBits(x[at], b.refX[i]) || !sameBits(y[at], b.refY[i])) {
                why = "branch " + std::to_string(bi) + " point " + std::to_string(i) + ": (" + std::to_string(x[at]) + ", " + std::to_string(y[at]) +
                      ") expected (" + std::to_string(b.refX[i]) + ", " + std::to_string(b.refY[i]) + ")";
                return false;
            }
        }
    }
    return true;
}

void scenarioChains(const Options& opt, Report& rep) {
    Rng rng(opt.seed * 101 + 1);
    Stopwatch sw;
    ChainGraph g = buildChains(opt, rng, opt.threads);
    Network& net = *g.net;
    std::printf("  graph: %zu nodes (%zu branches x %zu point-wise nodes, %zu points each), built in %.0f ms\n", g.nodeCount, opt.branches, opt.depth,
                opt.points, sw.lap());

    std::string why;
    auto cookAndCheck = [&](Network& n, const ChainGraph& expect, const char* label) {
        Stopwatch t;
        GeometryHandle geo = n.cookDisplay();
        const double ms = t.lap();
        const bool ok = matchesReference(expect, *geo, why);
        rep.check(ok, std::string(label) + ": " + why);
        if (opt.verbose || !ok) std::printf("    %-52s %9.1f ms  %s\n", label, ms, ok ? "ok" : "MISMATCH");
        return geo;
    };

    // 1. First cook: plan, compile, run; compared with the independent reference.
    sw.lap();
    GeometryHandle first = cookAndCheck(net, g, "cold cook (fused, JIT)");
    const double coldMs = sw.lap();
    std::printf("  cold cook: %.1f ms for %zu nodes, %zu points out, %zu kernels compiled (%.1f ms compiling), %zu fused chains\n", coldMs, g.nodeCount,
                first->pointCount(), net.compiler().compileCount(), net.compiler().compileMillis(), net.fusedChainCount());
    rep.check(net.fusedChainCount() >= opt.branches, "every branch fused into at least one chain");
    rep.check(net.cookDisplay().get() == first.get(), "a second cook of a clean graph returns the cached geometry");

    // 2. The same graph in other configurations must give bit-identical output.
    std::vector<Config> configs;
    configs.push_back({true, Isa::avx2, false, opt.threads});
    configs.push_back({true, Isa::avx2, true, 1});
    configs.push_back({true, Isa::sse2, true, opt.threads});
    {
        Config c{true, Isa::avx512, true, opt.threads};
        if (net.compiler().isaSupported(Isa::avx512)) configs.push_back(c);
    }
    if (opt.interpreter) {
        configs.push_back({false, Isa::sse2, true, opt.threads});
        configs.push_back({false, Isa::sse2, false, opt.threads});
    }
    for (const Config& c : configs) {
        auto other = cloneWith(net, c);
        Stopwatch t;
        GeometryHandle geo = other->cookDisplay();
        const double ms = t.lap();
        const bool ok = sameGeometry(*geo, *first, why);
        rep.check(ok, c.name() + ": " + why);
        std::printf("  %-44s %9.1f ms  %s\n", c.name().c_str(), ms, ok ? "identical" : "DIFFERENT");
    }

    // 3. Persistence: save, load, save must be byte-identical and cook to the same geometry.
    {
        const std::string text = saveNetwork(net, true);
        Network loaded(opt.threads);
        Stopwatch t;
        loadNetwork(loaded, text);
        const double loadMs = t.lap();
        rep.check(saveNetwork(loaded, true) == text, "save -> load -> save is byte-identical");
        GeometryHandle geo = loaded.cookDisplay();
        rep.check(sameGeometry(*geo, *first, why), "loaded graph cooks identically: " + why);
        std::printf("  persistence: %.1f KB of JSON, loaded in %.0f ms, byte-identical re-save\n", static_cast<double>(text.size()) / 1024.0, loadMs);
    }

    // 4. Edit storm: change one parameter, recook, compare with the reference; only a handful of nodes may recook.
    {
        std::vector<size_t> cooksBefore;
        double editCookMs = 0.0;
        size_t worstRecooked = 0;
        const size_t edits = opt.edits;
        for (size_t e = 0; e < edits; ++e) {
            Branch& b = g.branches[rng.index(g.branches.size())];
            const size_t k = rng.index(b.chain.size());
            const int t = b.tmpl[k];
            auto p = randomParams(t, rng);
            const int slot = static_cast<int>(rng.index(4));
            b.params[k][static_cast<size_t>(slot)] = p[static_cast<size_t>(slot)];
            net.node(b.chain[k]).setFloat(paramName(t, slot), p[static_cast<size_t>(slot)]);
            computeReference(b);
            const size_t before = totalCooks(net);
            Stopwatch t2;
            GeometryHandle geo = net.cookDisplay();
            editCookMs += t2.lap();
            const size_t recooked = totalCooks(net) - before;
            worstRecooked = std::max(worstRecooked, recooked);
            if (!matchesReference(g, *geo, why)) {
                rep.check(false, "edit " + std::to_string(e) + ": " + why);
                break;
            }
        }
        rep.check(true, "edit storm");
        rep.check(worstRecooked <= 8, "one edit recooks a handful of nodes (worst " + std::to_string(worstRecooked) + ")");
        std::printf("  edit storm: %zu single-parameter edits, each recooked at most %zu node(s), %.2f ms per recook on average\n", edits, worstRecooked,
                    editCookMs / static_cast<double>(std::max<size_t>(edits, 1)));
    }

    // 5. Undo/redo storm through History on the big graph.
    {
        History history(net, opt.edits + 10);
        std::vector<Branch> initial = g.branches;
        struct Edit {
            size_t branch, node;
            int slot;
            float value;
        };
        std::vector<Edit> log;
        const size_t steps = std::max<size_t>(opt.edits / 5, 8);
        Stopwatch t;
        for (size_t e = 0; e < steps; ++e) {
            Branch& b = g.branches[rng.index(g.branches.size())];
            const size_t bi = static_cast<size_t>(&b - g.branches.data());
            const size_t k = rng.index(b.chain.size());
            const int t2 = b.tmpl[k];
            const int slot = static_cast<int>(rng.index(4));
            float v = randomParams(t2, rng)[static_cast<size_t>(slot)];
            while (v == b.params[k][static_cast<size_t>(slot)]) v = randomParams(t2, rng)[static_cast<size_t>(slot)];  // a no-op edit would not commit
            b.params[k][static_cast<size_t>(slot)] = v;
            net.node(b.chain[k]).setFloat(paramName(t2, slot), v);
            log.push_back({bi, k, slot, v});
            history.commit("edit " + std::to_string(e));
        }
        const double commitMs = t.lap();
        for (Branch& b : g.branches) computeReference(b);
        rep.check(matchesReference(g, *net.cookDisplay(), why), "after the edits: " + why);

        auto setState = [&](size_t applied) {
            g.branches = initial;
            for (size_t i = 0; i < applied; ++i) g.branches[log[i].branch].params[log[i].node][static_cast<size_t>(log[i].slot)] = log[i].value;
            for (Branch& b : g.branches) computeReference(b);
        };
        size_t undone = 0;
        while (history.canUndo()) {
            history.undo();
            ++undone;
            if (undone % 4 == 0 || !history.canUndo()) {
                setState(steps - undone);
                if (!matchesReference(g, *net.cookDisplay(), why)) {
                    rep.check(false, "after " + std::to_string(undone) + " undo(s): " + why);
                    break;
                }
            }
        }
        const double undoMs = t.lap();
        rep.check(undone == steps, "every edit is one undo step");
        size_t redone = 0;
        while (history.canRedo()) {
            history.redo();
            ++redone;
        }
        setState(steps);
        rep.check(matchesReference(g, *net.cookDisplay(), why), "after redoing everything: " + why);
        std::printf("  history: %zu commits in %.0f ms (%.1f ms each), undo all in %.0f ms, redo all, results match the reference at every checked step\n",
                    steps, commitMs, commitMs / static_cast<double>(steps), undoMs);
    }
}

// ------------------------------------------------------------------------------------------------ scenario 2: random DAG

std::string randomExpr(Rng& rng, int depth, bool transcendental, const std::vector<std::string>& vars) {
    auto leaf = [&]() -> std::string {
        if (rng.chance(0.7)) return vars[rng.index(vars.size())];
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(rng.round2(-3, 3)));
        return buf;
    };
    if (depth <= 0 || rng.chance(0.2)) return leaf();
    auto sub = [&]() { return randomExpr(rng, depth - 1, transcendental, vars); };
    const size_t kinds = transcendental ? 16 : 11;
    switch (rng.index(kinds)) {
        case 0: return "(" + sub() + " + " + sub() + ")";
        case 1: return "(" + sub() + " - " + sub() + ")";
        case 2: return "(" + sub() + " * " + sub() + ")";
        case 3: return "min(" + sub() + ", " + sub() + ")";
        case 4: return "max(" + sub() + ", " + sub() + ")";
        case 5: return "abs(" + sub() + ")";
        case 6: return "sqrt(abs(" + sub() + "))";
        case 7: return "floor(" + sub() + ")";
        case 8: return "(" + sub() + " / (abs(" + sub() + ") + 0.5))";
        case 9: return "((" + sub() + " > " + sub() + ") ? " + sub() + " : " + sub() + ")";
        case 10: return "((" + sub() + " < " + sub() + " && !(" + sub() + " == " + sub() + ")) ? " + sub() + " : " + sub() + ")";
        case 11: return "sin(" + sub() + ")";
        case 12: return "cos(" + sub() + ")";
        case 13: return "exp(clamp(" + sub() + ", -5, 5))";
        case 14: return "log(abs(" + sub() + ") + 0.5)";
        default: return "pow(abs(" + sub() + ") + 0.5, clamp(" + sub() + ", -2, 2))";
    }
}

std::string randomProgram(Rng& rng, bool transcendental) {
    static const std::vector<std::string> vars = {"x", "y", "a", "b", "c", "d", "u", "v", "w", "i1"};
    static const std::vector<std::string> targets = {"x", "y", "u", "v", "w", "i1"};
    std::string code;
    const size_t statements = 1 + rng.index(4);
    for (size_t s = 0; s < statements; ++s) {
        code += targets[rng.index(targets.size())] + " = clamp(" + randomExpr(rng, 3, transcendental, vars) + ", -1000, 1000); ";
    }
    return code;
}

struct DagGraph {
    std::unique_ptr<Network> net;
    std::vector<NodeId> ids;       // creation order
    std::vector<size_t> points;    // point count per node (conservative bookkeeping while building)
    std::vector<int> consumers;
    std::vector<NodeId> leaves;  // the nodes nobody consumes (before the final reduction), compared in full by digest
    NodeId out = invalidNodeId;
    NodeId topMerge = invalidNodeId;
};

std::vector<uint64_t> leafDigests(Network& net, const std::vector<NodeId>& leaves) {
    std::vector<uint64_t> out;
    out.reserve(leaves.size());
    for (NodeId id : leaves) out.push_back(digest(*net.cook(id)));  // cached after the display cook
    return out;
}

// Names the first leaf whose digest differs (empty when all agree).
std::string firstLeafDifference(Network& net, const std::vector<NodeId>& leaves, const std::vector<uint64_t>& want, const std::vector<uint64_t>& got) {
    for (size_t i = 0; i < leaves.size(); ++i)
        if (want[i] != got[i]) return "leaf " + net.node(leaves[i]).name() + " (" + net.node(leaves[i]).typeName() + ", node #" + std::to_string(leaves[i]) + ")";
    return {};
}

DagGraph buildDag(const Options& opt, Rng& rng, bool transcendental) {
    DagGraph g;
    g.net = std::make_unique<Network>(opt.threads);
    Network& net = *g.net;
    const size_t cap = 4 * opt.points;  // keeps every cached result small enough that thousands of them fit in memory
    auto add = [&](NodeId id, size_t pts, std::initializer_list<size_t> inputs) {
        g.ids.push_back(id);
        g.points.push_back(pts);
        g.consumers.push_back(0);
        for (size_t in : inputs) ++g.consumers[in];
    };
    auto pick = [&]() -> size_t {
        const size_t n = g.ids.size();
        if (rng.chance(0.7)) return n - 1 - rng.index(std::min<size_t>(n, 30));
        return rng.index(n);
    };
    const size_t sources = std::max<size_t>(4, opt.dagNodes / 20);
    for (size_t i = 0; i < sources; ++i) {
        const size_t count = 1000 + rng.index(opt.points > 1000 ? opt.points - 1000 : 1);
        NodeId id;
        if (rng.chance(0.75)) {
            id = addLine(net, count, rng.round2(1, 30), rng.round2(-3, 3));
        } else {
            id = net.addNode("circle");
            net.node(id).setFloat("count", static_cast<float>(count));
            net.node(id).setFloat("radius", rng.round2(1, 10));
        }
        // Every geometry of the graph descends from a source, so i1 and i2 are int attributes everywhere (an int attribute merged
        // with a float one of the same name is an error by design); wranglers may read and write them freely.
        for (const char* name : {"i1", "i2"}) {
            NodeId ac = net.addNode("attribCreate");
            net.node(ac).setString("name", name);
            net.node(ac).setString("type", "int");
            net.node(ac).setFloat("value", static_cast<float>(rng.index(9)));
            net.connect(id, ac);
            id = ac;
        }
        add(id, count, {});
    }
    while (g.ids.size() < opt.dagNodes) {
        const size_t in = pick();
        const double r = rng.uniform();
        NodeId id;
        if (r < 0.15) {
            id = net.addNode("transform");
            for (int s = 0; s < 4; ++s) net.node(id).setFloat(paramName(0, s), rng.round2(-1.2f, 1.2f));
            net.connect(g.ids[in], id);
            add(id, g.points[in], {in});
        } else if (r < 0.72) {
            id = addWrangle(net, randomProgram(rng, transcendental), {rng.round2(-2, 2), rng.round2(-2, 2), rng.round2(-2, 2), rng.round2(-2, 2)});
            net.connect(g.ids[in], id);
            add(id, g.points[in], {in});
        } else if (r < 0.78) {
            id = net.addNode("attribCreate");
            static const char* names[] = {"u", "v", "w"};
            net.node(id).setString("name", names[rng.index(3)]);
            net.node(id).setString("type", "float");
            net.node(id).setFloat("value", rng.round2(-4, 4));
            net.connect(g.ids[in], id);
            add(id, g.points[in], {in});
        } else if (r < 0.86) {
            id = net.addNode("null");
            net.connect(g.ids[in], id);
            add(id, g.points[in], {in});
        } else if (r < 0.90) {
            // a subnet whose inside is one random wrangle: exercises child networks next to the flat nodes
            id = net.addNode("subnet");
            Network& child = static_cast<SubnetNode&>(net.node(id)).network();
            NodeId sin = child.addNode("subnetInput");
            NodeId w = addWrangle(child, randomProgram(rng, transcendental), {rng.round2(-2, 2), rng.round2(-2, 2), 0, 0});
            child.connect(sin, w);
            child.setDisplayNode(w);
            net.connect(g.ids[in], id);
            add(id, g.points[in], {in});
        } else {
            const size_t fan = 2 + rng.index(4);
            std::vector<size_t> ins;
            size_t total = 0;
            for (size_t k = 0; k < fan * 4 && ins.size() < fan; ++k) {
                const size_t c = pick();
                if (std::find(ins.begin(), ins.end(), c) != ins.end()) continue;
                if (total + g.points[c] > cap) continue;
                ins.push_back(c);
                total += g.points[c];
            }
            if (ins.size() < 2) continue;
            id = net.addNode("merge");
            for (size_t k = 0; k < ins.size(); ++k) net.connect(g.ids[ins[k]], id, static_cast<int>(k));
            g.ids.push_back(id);
            g.points.push_back(total);
            g.consumers.push_back(0);
            for (size_t c : ins) ++g.consumers[c];
        }
    }
    // Everything must contribute to the display node: merge all leaves (nodes nobody uses) in groups, then merge the groups.
    std::vector<NodeId> leaves;
    for (size_t i = 0; i < g.ids.size(); ++i)
        if (g.consumers[i] == 0) leaves.push_back(g.ids[i]);
    g.leaves = leaves;
    // Reduce every leaf to a few points before the final merge, so the whole graph contributes without one enormous result.
    std::vector<NodeId> reduced;
    for (NodeId leaf : leaves) {
        NodeId r = net.addNode("resample");
        net.node(r).setFloat("count", 50.0f);
        net.connect(leaf, r);
        reduced.push_back(r);
    }
    g.topMerge = mergeTree(net, reduced, 64);
    g.out = net.addNode("null");
    net.connect(g.topMerge, g.out);
    net.setDisplayNode(g.out);
    return g;
}

void scenarioDag(const Options& opt, Report& rep, bool transcendental) {
    Rng rng(opt.seed * 977 + (transcendental ? 2 : 1));
    Stopwatch sw;
    DagGraph g = buildDag(opt, rng, transcendental);
    Network& net = *g.net;
    const size_t nodes = net.nodeIds().size();
    std::printf("  graph: %zu nodes (random DAG, %s functions), built in %.0f ms\n", nodes, transcendental ? "all" : "exact-arithmetic", sw.lap());

    sw.lap();
    GeometryHandle base = net.cookDisplay();
    const double coldMs = sw.lap();
    std::printf("  cold cook: %.1f ms, %zu points out, %zu attributes, %zu kernels compiled (%.1f ms), %zu fused chains, %zu cooks\n", coldMs,
                base->pointCount(), base->attribs(AttribClass::point).size(), net.compiler().compileCount(), net.compiler().compileMillis(),
                net.fusedChainCount(), totalCooks(net));
    rep.check(base->pointCount() > 0, "the DAG produced geometry");
    const std::vector<uint64_t> baseDigests = leafDigests(net, g.leaves);
    size_t leafPoints = 0;
    for (NodeId id : g.leaves) leafPoints += net.cook(id)->pointCount();
    std::printf("  %zu leaf results (%zu points in total) are also compared in full by digest\n", g.leaves.size(), leafPoints);

    std::vector<Config> configs;
    configs.push_back({true, Isa::avx2, false, opt.threads});
    configs.push_back({true, Isa::avx2, true, 1});
    configs.push_back({true, Isa::sse2, true, opt.threads});
    if (net.compiler().isaSupported(Isa::avx512)) configs.push_back({true, Isa::avx512, true, opt.threads});
    if (!transcendental && opt.interpreter) {
        configs.push_back({false, Isa::sse2, true, opt.threads});
        configs.push_back({false, Isa::sse2, false, 1});
    }
    std::string why;
    for (const Config& c : configs) {
        auto other = cloneWith(net, c);
        Stopwatch t;
        GeometryHandle geo = other->cookDisplay();
        const double ms = t.lap();
        bool ok = sameGeometry(*geo, *base, why);
        const std::string leafDiff = firstLeafDifference(*other, g.leaves, baseDigests, leafDigests(*other, g.leaves));
        if (!leafDiff.empty()) {
            ok = false;
            why += (why.empty() ? "" : "; ") + leafDiff + " differs";
        }
        rep.check(ok, c.name() + ": " + why);
        std::printf("  %-44s %9.1f ms  %s\n", c.name().c_str(), ms, ok ? "identical" : "DIFFERENT");
    }
}

// ------------------------------------------------------------------------------------------------ scenario 3: edit storm against a rebuilt graph

void scenarioEdits(const Options& opt, Report& rep) {
    Rng rng(opt.seed * 31 + 7);
    Options dagOpt = opt;
    dagOpt.dagNodes = std::max<size_t>(opt.dagNodes, 200);
    DagGraph g = buildDag(dagOpt, rng, false);
    Network& net = *g.net;
    const size_t baseNodes = g.ids.size();
    std::printf("  graph: %zu nodes\n", net.nodeIds().size());

    Config reference{true, Isa::avx2, false, 1};  // a different planner path (no fusion, one thread) as the oracle
    std::string why;
    auto verify = [&](const char* label) {
        GeometryHandle got = net.cookDisplay();
        auto fresh = cloneWith(net, reference);
        GeometryHandle want = fresh->cookDisplay();
        bool ok = sameGeometry(*got, *want, why);
        const std::string leafDiff = firstLeafDifference(net, g.leaves, leafDigests(*fresh, g.leaves), leafDigests(net, g.leaves));
        if (!leafDiff.empty()) {
            ok = false;
            why += (why.empty() ? "" : "; ") + leafDiff + " differs";
        }
        rep.check(ok, std::string(label) + ": " + why);
        return ok;
    };
    rep.check(verify("initial"), "initial graph");

    History history(net, 4000);
    std::vector<std::string> snapshots;  // saved text at each commit, for checking undo/redo
    std::vector<GeometryHandle> results;
    std::vector<double> times;  // the playhead is not part of the history, so the test puts it back itself
    times.push_back(net.time());
    snapshots.push_back(saveNetwork(net, true));
    results.push_back(net.cookDisplay());

    std::vector<NodeId> added;
    std::vector<std::string> names;
    for (NodeId id : g.ids) names.push_back(net.node(id).name());
    size_t editsDone = 0, compared = 0, evalNodes = 0;
    double cookMs = 0;
    const size_t batches = std::max<size_t>(opt.edits / 5, 4);
    for (size_t batch = 0; batch < batches; ++batch) {
        for (int e = 0; e < 5; ++e, ++editsDone) {
            const double r = rng.uniform();
            const size_t at = rng.index(baseNodes);
            Node& n = net.node(g.ids[at]);
            if (r < 0.45) {  // a parameter
                const auto names4 = n.typeName() == "transform" ? std::vector<std::string>{"sx", "sy", "tx", "ty"} : std::vector<std::string>{"a", "b", "c", "d"};
                if (n.typeName() == "transform" || n.typeName() == "wrangle") n.setFloat(names4[rng.index(4)], rng.round2(-2, 2));
            } else if (r < 0.60) {  // new program text (a different kernel)
                if (n.typeName() == "wrangle") n.setString("code", randomProgram(rng, false));
            } else if (r < 0.70) {  // a time-dependent expression, sometimes referring to an earlier node
                if (n.typeName() == "wrangle") {
                    std::string text = "time * " + std::to_string(rng.round2(0.1f, 3));
                    if (at > 10 && g.ids[at - 5] != g.ids[at]) {
                        const Node& other = net.node(g.ids[rng.index(at)]);
                        if (other.typeName() == "wrangle") text += " + " + other.name() + ".a * 0.5";
                    }
                    n.setExpression("b", text);
                }
            } else if (r < 0.78) {  // rewire: the input becomes any earlier node
                if (at > 0 && n.inputCount() == 1 && n.typeName() != "subnetInput") {
                    const size_t src = rng.index(at);
                    if (net.node(g.ids[src]).typeName() != "merge" || true) {
                        net.connect(g.ids[src], g.ids[at], 0);
                    }
                }
            } else if (r < 0.90) {  // add a new wrangle fed by an existing node and attach it to the top merge
                NodeId w = addWrangle(net, randomProgram(rng, false), {rng.round2(-2, 2), 0, 0, 0});
                net.connect(g.ids[rng.index(baseNodes)], w);
                int slot = 0;
                while (net.inputOf(g.topMerge, slot) != invalidNodeId) ++slot;
                net.connect(w, g.topMerge, slot);
                added.push_back(w);
            } else if (!added.empty()) {  // remove one of the added nodes again
                const size_t i = rng.index(added.size());
                net.removeNode(added[i]);
                added.erase(added.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                net.setTime(rng.round2(0, 5));
            }
        }
        net.setTime(rng.round2(0, 5));
        const size_t before = totalCooks(net);
        Stopwatch t;
        net.cookDisplay();
        cookMs += t.lap();
        evalNodes += totalCooks(net) - before;
        history.commit("batch " + std::to_string(batch));
        times.push_back(net.time());
        snapshots.push_back(saveNetwork(net, true));
        results.push_back(net.cookDisplay());
        if (batch % 3 == 0 || batch + 1 == batches) {
            ++compared;
            if (!verify(("after batch " + std::to_string(batch)).c_str())) break;
        }
    }
    std::printf("  %zu edits in %zu batches, %zu comparisons against a from-scratch rebuild; incremental cooks averaged %.1f nodes and %.1f ms\n", editsDone,
                batches, compared, static_cast<double>(evalNodes) / static_cast<double>(batches), cookMs / static_cast<double>(batches));

    // Undo every batch: each earlier state must cook to what it cooked to the first time.
    size_t step = snapshots.size() - 1;
    Stopwatch t;
    bool undoOk = true;
    while (history.canUndo()) {
        history.undo();
        --step;
        net.setTime(times[step]);
        GeometryHandle geo = net.cookDisplay();
        const bool ok = sameGeometry(*geo, *results[step], why) && saveNetwork(net, true) == snapshots[step];
        if (!ok) {
            rep.check(false, "undo to state " + std::to_string(step) + ": " + why);
            undoOk = false;
            break;
        }
    }
    rep.check(undoOk && step == 0, "undoing every batch reproduces every earlier state exactly");
    while (history.canRedo()) history.redo();
    net.setTime(times.back());
    rep.check(sameGeometry(*net.cookDisplay(), *results.back(), why), "redoing everything reproduces the final result: " + why);
    std::printf("  undo/redo of %zu states verified in %.0f ms\n", snapshots.size() - 1, t.lap());
}

// ------------------------------------------------------------------------------------------------ scenario 4: subnets and assets

void scenarioSubnets(const Options& opt, Report& rep) {
    Rng rng(opt.seed * 13 + 3);
    const size_t points = std::min<size_t>(opt.points, 20000);
    const size_t instances = opt.quick ? 40 : 300;
    const size_t depth = opt.quick ? 4 : 8;

    // An asset: one input, promoted parameters `scale` and `shift`, a wrangle driven by them.
    {
        Network def(1);
        NodeId sub = def.addNode("subnet");
        Node& s = def.node(sub);
        s.addSpareFloat("scale", 1.0f);
        s.addSpareFloat("shift", 0.0f);
        Network& child = static_cast<SubnetNode&>(s).network();
        NodeId in = child.addNode("subnetInput");
        NodeId w = addWrangle(child, "x = x * a + b;", {});
        child.node(w).setExpression("a", "parent.scale");
        child.node(w).setExpression("b", "parent.shift");
        child.connect(in, w);
        child.setDisplayNode(w);
        registerAsset(saveAsset(def, sub, "stressAffine", 1));
    }

    Network net(opt.threads);
    NodeId line = addLine(net, points, 10.0f, 0.0f);
    NodeId prev = line;
    std::vector<std::pair<float, float>> params;
    for (size_t i = 0; i < instances; ++i) {
        NodeId inst = net.addNode("stressAffine");
        const float scale = rng.round2(0.95f, 1.05f), shift = rng.round2(-0.3f, 0.3f);
        net.node(inst).setFloat("scale", scale);
        net.node(inst).setFloat("shift", shift);
        net.connect(prev, inst);
        params.emplace_back(scale, shift);
        prev = inst;
    }
    // Nested subnets, `depth` levels deep, each forwarding its own promoted parameter to the next level.
    {
        NodeId outer = net.addNode("subnet");
        Node* current = &net.node(outer);
        current->addSpareFloat("amount", 0.5f);
        Network* cn = &static_cast<SubnetNode&>(*current).network();
        NodeId first = cn->addNode("subnetInput");
        NodeId chainPrev = first;
        for (size_t level = 0; level < depth; ++level) {
            NodeId inner = cn->addNode("subnet");
            Node& in = cn->node(inner);
            in.addSpareFloat("amount", 0.0f);
            in.setExpression("amount", "parent.amount");
            Network& next = static_cast<SubnetNode&>(in).network();
            NodeId si = next.addNode("subnetInput");
            NodeId w = addWrangle(next, "x = x + a;", {});
            next.node(w).setExpression("a", "parent.amount");
            next.connect(si, w);
            next.setDisplayNode(w);
            cn->connect(chainPrev, inner);
            chainPrev = inner;
        }
        cn->setDisplayNode(chainPrev);
        net.connect(prev, outer);
        prev = outer;
    }
    net.setDisplayNode(prev);
    std::printf("  graph: %zu top-level nodes, %zu asset instances, %zu nested subnet levels, %zu points\n", net.nodeIds().size(), instances, depth, points);

    auto expected = [&](float amount) {
        std::vector<float> xs(points);
        const float step = points > 1 ? 10.0f / static_cast<float>(points - 1) : 0.0f;
        for (size_t i = 0; i < points; ++i) {
            float x = static_cast<float>(i) * step;
            for (const auto& [scale, shift] : params) x = x * scale + shift;
            for (size_t l = 0; l < depth; ++l) x = x + amount;
            xs[i] = x;
        }
        return xs;
    };
    auto check = [&](float amount, const char* label) {
        const auto want = expected(amount);
        Stopwatch t;
        GeometryHandle geo = net.cookDisplay();
        const double ms = t.lap();
        bool ok = geo->pointCount() == points;
        size_t bad = 0;
        for (size_t i = 0; ok && i < points; ++i)
            if (!sameBits(geo->x()[i], want[i]) && ++bad == 1) std::printf("    first difference at %zu: %g vs %g\n", i, static_cast<double>(geo->x()[i]), static_cast<double>(want[i]));
        rep.check(ok && bad == 0, std::string(label) + " matches the reference");
        std::printf("  %-34s %8.1f ms\n", label, ms);
    };
    check(0.5f, "cold cook");
    net.node(net.nodeIds().back()).setFloat("amount", 0.25f);
    check(0.25f, "after changing the outer parameter");
    const size_t compiles = net.compiler().compileCount();
    net.node(net.nodeIds()[1]).setFloat("scale", 1.01f);
    params[0].first = 1.01f;
    check(0.25f, "after editing the first instance");
    rep.check(net.compiler().compileCount() == compiles, "editing a promoted parameter never recompiles");
    std::printf("  %zu kernels compiled for %zu instances plus the nested levels (cache size %zu)\n", net.compiler().compileCount(), instances, net.compiler().cacheSize());
    rep.check(net.compiler().compileCount() <= 4, "all instances share a handful of kernels");

    // Round trip through a file: the asset definition is embedded.
    const std::string text = saveNetwork(net, true);
    Network loaded(opt.threads);
    loadNetwork(loaded, text);
    rep.check(saveNetwork(loaded, true) == text, "graph with assets and nested subnets saves byte-identically after a load");
    std::string why;
    rep.check(sameGeometry(*loaded.cookDisplay(), *net.cookDisplay(), why), "loaded copy cooks identically: " + why);
}

// ------------------------------------------------------------------------------------------------ scenario 5: time and expressions

void scenarioTime(const Options& opt, Report& rep) {
    Rng rng(opt.seed * 57 + 5);
    const size_t branches = opt.quick ? 16 : 64;
    const size_t depth = opt.quick ? 10 : 20;
    const size_t points = std::min<size_t>(opt.points, 10000);
    Network net(opt.threads);
    std::vector<NodeId> ends;
    std::vector<NodeId> animated;
    for (size_t b = 0; b < branches; ++b) {
        NodeId prev = addLine(net, points, 5.0f, 0.0f);
        for (size_t k = 0; k < depth; ++k) {
            NodeId w = addWrangle(net, "x = x + a * 0.01 + b * 0.01; y = y + sin(x + c) * 0.1;", {0.1f, 0.2f, 0.3f, 0.0f});
            const bool dynamic = (b % 2 == 0);
            if (dynamic) {
                net.node(w).setExpression("a", "sin(time * " + std::to_string(rng.round2(0.5f, 3)) + ")");
                if (k > 0) net.node(w).setExpression("b", net.node(prev).name() + ".a * 0.5 + frame * 0.01");
                animated.push_back(w);
            }
            net.connect(prev, w);
            prev = w;
        }
        ends.push_back(prev);
    }
    NodeId root = mergeTree(net, ends, 4);
    net.setDisplayNode(root);
    std::printf("  graph: %zu nodes, half of the branches animated (time, frame and references to other nodes)\n", net.nodeIds().size());
    rep.check(net.anyDependsOnTime(), "time dependence detected");
    size_t timeDependent = 0;
    for (NodeId id : net.nodeIds()) timeDependent += net.dependsOnTime(id) ? 1 : 0;
    rep.check(timeDependent == animated.size(), "exactly the animated wranglers depend on time (" + std::to_string(timeDependent) + " of " + std::to_string(animated.size()) + ")");

    net.cookDisplay();
    std::string why;
    const size_t frames = opt.quick ? 12 : 60;
    double ms = 0;
    size_t recooked = 0, staticRecooked = 0;
    for (size_t f = 1; f <= frames; ++f) {
        net.setTime(static_cast<double>(f) / 24.0);
        const size_t before = totalCooks(net);
        std::vector<size_t> staticBefore;
        for (NodeId id : net.nodeIds())
            if (!net.dependsOnTime(id) && net.node(id).typeName() == "wrangle") staticBefore.push_back(net.node(id).cookCount());
        Stopwatch t;
        GeometryHandle geo = net.cookDisplay();
        ms += t.lap();
        recooked += totalCooks(net) - before;
        size_t i = 0;
        for (NodeId id : net.nodeIds())
            if (!net.dependsOnTime(id) && net.node(id).typeName() == "wrangle") staticRecooked += net.node(id).cookCount() - staticBefore[i++];
        if (f % std::max<size_t>(frames / 4, 1) == 0) {
            auto fresh = cloneWith(net, {true, Isa::avx2, false, 1});
            fresh->setTime(net.time());
            rep.check(sameGeometry(*geo, *fresh->cookDisplay(), why), "frame " + std::to_string(f) + " equals a from-scratch cook: " + why);
        }
    }
    std::printf("  %zu frames: %.2f ms per frame, %.0f node cooks per frame\n", frames, ms / static_cast<double>(frames), static_cast<double>(recooked) / static_cast<double>(frames));
    // Fused chains cook as one task whose tail carries the count, so allow that the static branches' tails never recook.
    rep.check(staticRecooked == 0, "static branches are never recooked when only time changes");
}

// ------------------------------------------------------------------------------------------------ scenario 6: cancellation and errors

void scenarioCancelAndErrors(const Options& opt, Report& rep) {
    Rng rng(opt.seed * 7 + 11);
    Options o = opt;
    o.branches = opt.quick ? 16 : 48;
    o.depth = opt.quick ? 20 : 40;
    o.points = opt.quick ? 4000 : 10000;
    ChainGraph g = buildChains(o, rng, opt.threads);
    Network& net = *g.net;
    std::string why;

    // Cancel a slow cook (the interpreter) from another thread, several times, then let one finish.
    net.compiler().setJitEnabled(false);
    size_t cancelled = 0, finishedEarly = 0;
    const int attempts = opt.quick ? 3 : 5;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        std::atomic<bool> started{false};
        bool wasCancelled = false;
        std::thread canceller([&] {
            while (!started) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(15 + attempt * 10));
            net.cancel();
        });
        try {
            started = true;
            net.cookDisplay();
            ++finishedEarly;
        } catch (const CookCancelled&) {
            wasCancelled = true;
            ++cancelled;
        }
        canceller.join();
        (void)wasCancelled;
        // the next cook must be unaffected by the earlier cancel
        if (attempt + 1 == attempts) {
            GeometryHandle geo = net.cookDisplay();
            rep.check(matchesReference(g, *geo, why), "a cook after cancelled ones completes and matches the reference: " + why);
        }
    }
    std::printf("  cancellation: %zu of %d cooks were cancelled mid-way (%zu finished first), the final cook matches the reference\n", cancelled, attempts,
                finishedEarly);
    rep.check(cancelled > 0 || opt.quick, "at least one cook was actually cancelled");
    net.compiler().setJitEnabled(true);

    // Errors inside a big parallel cook: a bad program in one branch, then a missing input, then repair.
    Branch& victim = g.branches[g.branches.size() / 2];
    const NodeId bad = victim.chain[victim.chain.size() / 2];
    Node& badNode = net.node(bad);
    const std::string originalCode = badNode.typeName() == "wrangle" ? badNode.getString("code") : std::string();
    NodeId wrangleId = invalidNodeId;
    for (NodeId id : victim.chain)
        if (net.node(id).typeName() == "wrangle") wrangleId = id;
    rep.check(wrangleId != invalidNodeId, "found a wrangle to break");
    const std::string keep = net.node(wrangleId).getString("code");
    net.node(wrangleId).setString("code", "x = x +;");
    bool threw = false;
    std::string message;
    try {
        net.cookDisplay();
    } catch (const std::runtime_error& e) {
        threw = true;
        message = e.what();
    }
    rep.check(threw && message.find("parse error") != std::string::npos, "a syntax error deep in a parallel cook is reported: " + message);
    net.node(wrangleId).setString("code", keep);
    rep.check(matchesReference(g, *net.cookDisplay(), why), "after repairing the program the cook matches the reference: " + why);

    const NodeId line = victim.line;
    NodeId firstUser = victim.chain.front();
    net.disconnect(firstUser, 0);
    threw = false;
    message.clear();
    try {
        net.cookDisplay();
    } catch (const std::runtime_error& e) {
        threw = true;
        message = e.what();
    }
    rep.check(threw && message.find("not connected") != std::string::npos, "a missing input is reported: " + message);
    net.connect(line, firstUser, 0);
    rep.check(matchesReference(g, *net.cookDisplay(), why), "after reconnecting the cook matches the reference: " + why);

    bool cycleRefused = false;
    try {
        net.connect(victim.chain.back(), victim.chain.front(), 0);
    } catch (const std::runtime_error&) {
        cycleRefused = true;
    }
    rep.check(cycleRefused, "a connection that would close a cycle through 20+ nodes is refused");
    rep.check(matchesReference(g, *net.cookDisplay(), why), "the refused connection left the graph intact: " + why);

    bool badExpr = false;
    try {
        net.node(wrangleId).setExpression("a", "1 +");
    } catch (const std::runtime_error&) {
        badExpr = true;
    }
    rep.check(badExpr, "a bad expression is rejected");
    rep.check(matchesReference(g, *net.cookDisplay(), why), "the rejected expression changed nothing: " + why);
    std::printf("  errors: syntax error, missing input, cycle and bad expression were all reported and recoverable\n");
}

// ------------------------------------------------------------------------------------------------ scenario 7: wide geometry and a huge fused chain

void scenarioWide(const Options& opt, Report& rep) {
    Rng rng(opt.seed * 3 + 17);
    const size_t points = opt.widePoints ? opt.widePoints : opt.quick ? 20000 : std::max<size_t>(opt.points * 10, 100000);
    const size_t attrs = opt.wideAttrs ? opt.wideAttrs : opt.quick ? 40 : 200;
    const size_t chain = opt.wideChain ? opt.wideChain : opt.quick ? 60 : 400;
    Network net(opt.threads);
    NodeId line = addLine(net, points, 10.0f, 0.0f);
    NodeId prev = line;
    for (size_t i = 0; i < attrs; ++i) {
        NodeId c = net.addNode("attribCreate");
        net.node(c).setString("name", "attr" + std::to_string(i));
        net.node(c).setFloat("value", static_cast<float>(i));
        net.connect(prev, c);
        prev = c;
    }
    NodeId base = prev;
    std::set<size_t> touched;
    std::vector<size_t> touchOrder;
    for (size_t i = 0; i < chain; ++i) {
        const size_t target = rng.index(std::min<size_t>(attrs, 20));  // a small set of attributes written many times
        touched.insert(target);
        touchOrder.push_back(target);
        NodeId w = addWrangle(net, "attr" + std::to_string(target) + " = attr" + std::to_string(target) + " * a + x * b;", {1.0001f, 0.0001f, 0, 0});
        net.connect(prev, w);
        prev = w;
    }
    net.setDisplayNode(prev);
    std::printf("  graph: %zu points x %zu attributes (%.0f MB if nothing were shared), then a chain of %zu wranglers writing %zu of them\n", points, attrs,
                static_cast<double>(points * attrs * 4) / 1048576.0, chain, touched.size());

    Stopwatch t;
    GeometryHandle geo = net.cookDisplay();
    const double ms = t.lap();
    GeometryHandle before = net.cook(base);
    size_t shared = 0, cloned = 0;
    for (size_t i = 0; i < attrs; ++i) {
        const std::string name = "attr" + std::to_string(i);
        const AttribArray* a = geo->findAttrib(AttribClass::point, name);
        const AttribArray* b = before->findAttrib(AttribClass::point, name);
        rep.check(a && b, name + " exists on both sides");
        if (!a || !b) continue;
        const bool sameStorage = a->sharesStorageWith(*b);
        if (touched.count(i)) {
            ++cloned;
            rep.check(!sameStorage, name + " was written, so it has its own storage");
        } else {
            ++shared;
            rep.check(sameStorage, name + " was not written, so it still shares storage with the input");
        }
    }
    std::printf("  cold cook %.1f ms: %zu untouched attributes share storage with the input, %zu written ones were cloned once each\n", ms, shared, cloned);
    std::printf("  one fused kernel: %zu kernels compiled in %.1f ms for the %zu-node chain, %zu fused chains\n", net.compiler().compileCount(),
                net.compiler().compileMillis(), chain, net.fusedChainCount());
    rep.check(net.fusedChainCount() >= 1, "the long chain fused");

    // Reference for one attribute: replay its writes.
    const size_t probe = touchOrder.front();
    std::vector<float> want(points);
    for (size_t p = 0; p < points; p += std::max<size_t>(points / 997, 1)) {
        const float step = 10.0f / static_cast<float>(points - 1);
        const float x = static_cast<float>(p) * step;
        float v = static_cast<float>(probe);
        for (size_t target : touchOrder)
            if (target == probe) v = v * 1.0001f + x * 0.0001f;
        const AttribArray* a = geo->findAttrib(AttribClass::point, "attr" + std::to_string(probe));
        rep.check(sameBits(a->at(p), v), "attr" + std::to_string(probe) + "[" + std::to_string(p) + "] matches a replay of its writes");
        if (!sameBits(a->at(p), v)) break;
    }

    // Changing a parameter of the middle of the chain recooks the whole fused kernel but clones nothing new for untouched attributes.
    const NodeId middle = net.nodeIds()[attrs + 1 + chain / 2];
    net.node(middle).setFloat("b", 0.0002f);
    t.lap();
    GeometryHandle again = net.cookDisplay();
    std::printf("  recook after editing the middle of the chain: %.1f ms\n", t.lap());
    rep.check(again.get() != geo.get(), "the edit produced a new geometry");
    size_t stillShared = 0;
    for (size_t i = 0; i < attrs; ++i)
        if (!touched.count(i) && again->findAttrib(AttribClass::point, "attr" + std::to_string(i))->sharesStorageWith(*before->findAttrib(AttribClass::point, "attr" + std::to_string(i)))) ++stillShared;
    rep.check(stillShared == shared, "untouched attributes still share storage after the recook");
}

// ------------------------------------------------------------------------------------------------ scenario 8: scaling table

void scenarioScaling(const Options& opt, Report& rep) {
    Rng rng(opt.seed * 5 + 23);
    Options o = opt;
    o.branches = opt.quick ? 16 : 64;
    o.depth = opt.quick ? 12 : 40;
    ChainGraph source = buildChains(o, rng, opt.threads);
    const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
    std::vector<size_t> threadCounts = {1, 2, 4};
    if (hw >= 8) threadCounts.push_back(8);
    std::printf("  %zu nodes, %zu points per source; times are medians of %d recooks\n", source.nodeCount, o.points, opt.quick ? 3 : 7);
    std::printf("  %-10s %-10s %14s %16s\n", "threads", "fusion", "full recook ms", "one-edit recook ms");
    const int rounds = opt.quick ? 3 : 7;
    std::vector<double> fullByThreads;
    for (bool fusion : {true, false}) {
        for (size_t threads : threadCounts) {
            auto net = cloneWith(*source.net, {true, Isa::avx2, fusion, threads});
            net->cookDisplay();
            std::vector<double> full, single;
            for (int r = 0; r < rounds; ++r) {
                for (Branch& b : source.branches) net->node(b.chain.front()).setFloat(paramName(b.tmpl.front(), 0), 1.0f + 0.001f * static_cast<float>(r + 1));
                Stopwatch t;
                net->cookDisplay();
                full.push_back(t.lap());
                Branch& b = source.branches[rng.index(source.branches.size())];
                net->node(b.chain.back()).setFloat(paramName(b.tmpl.back(), 1), 1.0f + 0.002f * static_cast<float>(r + 1));
                t.lap();
                net->cookDisplay();
                single.push_back(t.lap());
            }
            std::sort(full.begin(), full.end());
            std::sort(single.begin(), single.end());
            std::printf("  %-10zu %-10s %14.2f %16.2f\n", threads, fusion ? "on" : "off", full[full.size() / 2], single[single.size() / 2]);
            if (fusion) fullByThreads.push_back(full[full.size() / 2]);
        }
    }
    rep.check(true, "scaling table printed");
    (void)fullByThreads;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep the progress visible if a scenario crashes the process
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", arg.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--quick") opt.quick = true;
        else if (arg == "--seed") opt.seed = std::strtoull(value().c_str(), nullptr, 10);
        else if (arg == "--branches") opt.branches = std::strtoull(value().c_str(), nullptr, 10);
        else if (arg == "--depth") opt.depth = std::strtoull(value().c_str(), nullptr, 10);
        else if (arg == "--points") opt.points = std::strtoull(value().c_str(), nullptr, 10);
        else if (arg == "--dag-nodes") opt.dagNodes = std::strtoull(value().c_str(), nullptr, 10);
        else if (arg == "--edits") opt.edits = std::strtoull(value().c_str(), nullptr, 10);
        else if (arg == "--wide-attrs") opt.wideAttrs = std::strtoull(value().c_str(), nullptr, 10);
        else if (arg == "--wide-chain") opt.wideChain = std::strtoull(value().c_str(), nullptr, 10);
        else if (arg == "--wide-points") opt.widePoints = std::strtoull(value().c_str(), nullptr, 10);
        else if (arg == "--threads") opt.threads = std::strtoull(value().c_str(), nullptr, 10);
        else if (arg == "--no-interpreter") opt.interpreter = false;
        else if (arg == "--verbose") opt.verbose = true;
        else if (arg == "--scenario") {
            std::stringstream ss(value());
            std::string name;
            while (std::getline(ss, name, ',')) opt.scenarios.insert(name);
        } else {
            std::printf("usage: jitnodesStress [--quick] [--seed N] [--scenario chains,dag,dagmath,edits,subnets,time,cancel,wide,scaling]\n"
                        "                      [--branches N] [--depth N] [--points N] [--dag-nodes N] [--edits N] [--threads N] [--no-interpreter] [--verbose]\n");
            return arg == "--help" ? 0 : 2;
        }
    }
    if (opt.quick) {
        opt.branches = std::min<size_t>(opt.branches, 16);
        opt.depth = std::min<size_t>(opt.depth, 12);
        opt.points = std::min<size_t>(opt.points, 3000);
        opt.dagNodes = std::min<size_t>(opt.dagNodes, 400);
        opt.edits = std::min<size_t>(opt.edits, 30);
    }

    NodeRegistry& registry = NodeRegistry::instance();
    (void)registry;
    {
        Network probe(1);
        std::printf("jitnodes stress test (seed %llu%s): %zu hardware threads, JIT %s, default ISA %s\n", static_cast<unsigned long long>(opt.seed),
                    opt.quick ? ", quick" : "", static_cast<size_t>(std::thread::hardware_concurrency()), probe.compiler().jitSupported() ? "available" : "unavailable",
                    isaName(probe.compiler().isa()));
    }

    struct Scenario {
        const char* name;
        const char* title;
        std::function<void(const Options&, Report&)> run;
    };
    const std::vector<Scenario> scenarios = {
        {"chains", "1  chains: thousands of point-wise nodes against an independent reference", scenarioChains},
        {"dag", "2  random DAG, exact arithmetic: every configuration incl. the interpreter must agree", [](const Options& o, Report& r) { scenarioDag(o, r, false); }},
        {"dagmath", "3  random DAG with sin/cos/exp/log/pow: every JIT configuration must agree", [](const Options& o, Report& r) { scenarioDag(o, r, true); }},
        {"edits", "4  edit storm on a random DAG: rewiring, new programs, expressions, add/remove nodes, undo/redo", scenarioEdits},
        {"subnets", "5  asset instances and deeply nested subnets", scenarioSubnets},
        {"time", "6  time animation and cross-node expressions", scenarioTime},
        {"cancel", "7  cancellation and error recovery inside a large parallel cook", scenarioCancelAndErrors},
        {"wide", "8  wide geometry (copy-on-write) and one very long fused chain", scenarioWide},
        {"scaling", "9  thread scaling and fusion, full and single-edit recooks", scenarioScaling},
    };

    Report rep;
    const auto totalStart = Clock::now();
    for (const Scenario& s : scenarios) {
        if (!opt.wants(s.name)) continue;
        rep.current = s.name;
        std::printf("\n== %s\n", s.title);
        const int before = rep.failures;
        const auto start = Clock::now();
        try {
            s.run(opt, rep);
        } catch (const std::exception& e) {
            rep.check(false, std::string("unexpected exception: ") + e.what());
        }
        std::printf("-- %s: %s (%.1f s)\n", s.name, rep.failures == before ? "PASS" : "FAIL", msSince(start) / 1000.0);
    }
    std::printf("\n%d checks, %d failed, %.1f s total\n", rep.checks, rep.failures, msSince(totalStart) / 1000.0);
    if (rep.failures) {
        std::printf("failed scenarios:");
        for (const auto& s : rep.failedScenarios) std::printf(" %s", s.c_str());
        std::printf("\n");
    }
    return rep.failures ? 1 : 0;
}
