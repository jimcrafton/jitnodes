#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "editorModel.h"
#include "graphController.h"
#include "keyframeEditor.h"
#include "profileController.h"
#include "viewportController.h"

using namespace jitnodes;
using namespace jitnodes::editor;

namespace {

int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

struct Test {
    const char* name;
    std::function<void()> run;
};

std::vector<Test>& tests() {
    static std::vector<Test> all;
    return all;
}

struct Register {
    Register(const char* name, std::function<void()> run) { tests().push_back({name, std::move(run)}); }
};

#define TEST(name)                                  \
    void name();                                    \
    static Register register_##name(#name, name);   \
    void name()

TEST(addNodeWiresSelectedNodeIntoNewOne) {
    EditorModel m;
    const NodeId line = m.addNode("line");
    CHECK(m.selected() == line && m.network().displayNode() == line);
    const NodeId xf = m.addNode("transform");
    CHECK(m.network().inputOf(xf, 0) == line);
    CHECK(m.selected() == xf && m.network().displayNode() == xf);
    const NodeId merge = m.addNode("merge");
    CHECK(m.network().inputOf(merge, 0) == xf && m.network().inputOf(merge, 1) == invalidNodeId);
    CHECK(m.statusLine().find("3 nodes") != std::string::npos);
}

TEST(unknownTypeThrowsAndLeavesTheGraphAlone) {
    EditorModel m;
    m.addNode("line");
    bool threw = false;
    try {
        m.addNode("noSuchNode");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(m.network().nodeIds().size() == 1 && !m.canRedo());
}

TEST(undoRedoFollowEditsAndFixTheSelection) {
    EditorModel m;
    CHECK(!m.canUndo());
    const NodeId line = m.addNode("line");
    const NodeId xf = m.addNode("transform");
    CHECK(m.canUndo() && m.undoLabel() == "add transform");
    CHECK(m.undo());
    CHECK(m.network().nodeIds().size() == 1 && m.selected() == invalidNodeId);
    CHECK(m.canRedo() && m.redo());
    CHECK(m.network().inputOf(xf, 0) == line && m.network().displayNode() == xf);
    m.undo();
    m.undo();
    CHECK(!m.canUndo() && m.network().nodeIds().empty());
}

TEST(cookReportsPointsAndErrors) {
    EditorModel m;
    CHECK(m.cook().ok && m.cook().points == 0);
    m.addNode("line");
    m.network().node(m.selected()).setFloat("count", 250.0f);
    CookResult r = m.cook();
    CHECK(r.ok && r.points == 250 && r.ms >= 0.0);
    m.select(invalidNodeId);
    m.addNode("transform");
    r = m.cook();
    CHECK(!r.ok && r.error.find("not connected") != std::string::npos);
    CHECK(!m.lastCook().ok);
}

TEST(saveOpenRoundTripAndModifiedFlag) {
    const std::string path = (std::filesystem::temp_directory_path() / "jitnodes_editor_test.json").string();
    {
        EditorModel m;
        CHECK(!m.modified());
        m.addNode("line");
        m.addNode("transform");
        CHECK(m.modified());
        m.save(path);
        CHECK(!m.modified() && m.path() == path);
        m.network().node(m.selected()).setFloat("tx", 3.0f);
        CHECK(m.modified());
        m.undo();
        m.redo();
    }
    EditorModel other;
    other.addNode("null");
    other.open(path);
    CHECK(other.network().nodeIds().size() == 2 && !other.modified() && !other.canUndo());
    CHECK(other.network().displayNode() != invalidNodeId && other.cook().ok);
    other.newGraph();
    CHECK(other.network().nodeIds().empty() && other.path().empty() && !other.modified());
    bool threw = false;
    try {
        other.open(path + ".missing");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    std::filesystem::remove(path);
}

TEST(playheadMapsToTimeAndWraps) {
    EditorModel m;
    m.setDuration(4.0);
    m.setPlayheadFraction(0.5);
    CHECK(std::fabs(m.time() - 2.0) < 1e-9 && std::fabs(m.playheadFraction() - 0.5) < 1e-9);
    m.advance(1.5);
    CHECK(std::fabs(m.time() - 3.5) < 1e-9);
    m.advance(1.0);
    CHECK(std::fabs(m.time() - 0.5) < 1e-9);
    m.setPlayheadFraction(7.0);
    CHECK(std::fabs(m.time() - 4.0) < 1e-9);
}

TEST(timeDependentGraphRecooksWhenThePlayheadMoves) {
    EditorModel m;
    const NodeId line = m.addNode("line");
    const NodeId xf = m.addNode("transform");
    m.network().node(xf).setExpression("tx", "time * 10");
    m.setPlayheadFraction(0.0);
    m.cook();
    const size_t before = m.network().node(xf).cookCount();
    m.setPlayheadFraction(0.4);
    m.cook();
    CHECK(m.network().node(xf).cookCount() == before + 1);
    CHECK(m.network().node(line).cookCount() == 1);
}


Vec center(const GraphController& g, NodeId id) {
    const Node& n = g.model().network().node(id);
    return g.toScreen({n.posX() + nodeWidth / 2, n.posY() + nodeHeight / 2});
}

void drag(GraphController& g, Vec from, Vec to) {
    g.mouseDown(from, leftButton);
    g.mouseMove({(from.x + to.x) / 2, (from.y + to.y) / 2});
    g.mouseMove(to);
    g.mouseUp(to);
}

TEST(newNodesGetNonOverlappingPositionsBelowTheirInput) {
    EditorModel m;
    const NodeId line = m.addNode("line");
    const NodeId xf = m.addNode("transform");
    m.select(invalidNodeId);
    const NodeId other = m.addNode("line");
    const Network& net = m.network();
    CHECK(net.node(line).hasPosition() && net.node(xf).hasPosition() && net.node(other).hasPosition());
    CHECK(net.node(xf).posX() == net.node(line).posX() && net.node(xf).posY() > net.node(line).posY() + nodeHeight);
    CHECK(net.node(other).posX() >= net.node(line).posX() + nodeWidth);
    CHECK(m.undoLabel() == "add line" && m.modified());
}

TEST(draggingANodeMovesItAndUndoRestoresIt) {
    EditorModel m;
    const NodeId a = m.addNode("line");
    GraphController g(m);
    int changes = 0;
    g.onChanged = [&](bool graph) { changes += graph ? 1 : 0; };
    const float x0 = m.network().node(a).posX(), y0 = m.network().node(a).posY();
    const Vec c = center(g, a);
    drag(g, c, {c.x + 100, c.y + 60});
    CHECK(m.selected() == a);
    CHECK(std::fabs(m.network().node(a).posX() - (x0 + 100)) < 0.01f && std::fabs(m.network().node(a).posY() - (y0 + 60)) < 0.01f);
    CHECK(changes == 1 && m.undoLabel() == "move node" && m.modified());
    m.undo();
    CHECK(m.network().node(a).posX() == x0 && m.network().node(a).posY() == y0);

    const size_t undoable = m.canUndo();
    g.mouseDown(center(g, a), leftButton);
    g.mouseUp(center(g, a));
    CHECK(m.canUndo() == (undoable != 0) && changes == 1);
}

TEST(draggingFromAnOutputPortConnectsAnInputOrTheFirstFreeOne) {
    EditorModel m;
    const NodeId a = m.addNode("line");
    m.select(invalidNodeId);
    const NodeId b = m.addNode("merge");
    m.select(invalidNodeId);
    GraphController g(m);
    drag(g, g.toScreen(g.outputPort(a)), g.toScreen(g.inputPort(b, 1)));
    CHECK(m.network().inputOf(b, 1) == a && m.network().inputOf(b, 0) == invalidNodeId && m.undoLabel() == "connect");

    const NodeId c = m.addNode("line");
    m.select(invalidNodeId);
    drag(g, g.toScreen(g.outputPort(c)), center(g, b));
    CHECK(m.network().inputOf(b, 0) == c);

    const NodeId d = m.addNode("transform");
    m.select(invalidNodeId);
    m.network().connect(b, d);
    const size_t nodesBefore = m.network().nodeIds().size();
    drag(g, g.toScreen(g.outputPort(d)), center(g, c));
    CHECK(m.network().nodeIds().size() == nodesBefore && m.network().inputOf(b, 0) == c && m.undoLabel() == "add transform");
}

TEST(aCycleIsRejectedWithAMessage) {
    EditorModel m;
    const NodeId a = m.addNode("null");
    const NodeId b = m.addNode("null");
    GraphController g(m);
    CHECK(m.network().inputOf(b, 0) == a);
    const bool couldUndo = m.canUndo();
    const std::string label = m.undoLabel();
    drag(g, g.toScreen(g.outputPort(b)), g.toScreen(g.inputPort(a, 0)));
    CHECK(m.network().inputOf(a, 0) == invalidNodeId && !g.message().empty());
    CHECK(m.canUndo() == couldUndo && m.undoLabel() == label);
}

TEST(draggingAConnectedInputDetachesTheWire) {
    EditorModel m;
    const NodeId a = m.addNode("line");
    const NodeId b = m.addNode("null");
    GraphController g(m);
    CHECK(m.network().inputOf(b, 0) == a);
    const Vec in = g.toScreen(g.inputPort(b, 0));
    drag(g, in, {in.x + 300, in.y + 300});
    CHECK(m.network().inputOf(b, 0) == invalidNodeId && m.undoLabel() == "disconnect");
    m.undo();
    CHECK(m.network().inputOf(b, 0) == a);

    drag(g, in, g.toScreen(g.inputPort(b, 0)));
    CHECK(m.network().inputOf(b, 0) == a);

    m.select(invalidNodeId);
    const NodeId c = m.addNode("null");
    m.select(invalidNodeId);
    CHECK(m.network().inputOf(c, 0) == invalidNodeId);
    drag(g, g.toScreen(g.inputPort(b, 0)), center(g, c));
    CHECK(m.network().inputOf(c, 0) == a && m.network().inputOf(b, 0) == invalidNodeId);
}

TEST(panZoomAndFitKeepTheGraphVisible) {
    EditorModel m;
    const NodeId a = m.addNode("line");
    GraphController g(m);
    const Vec before = g.toScreen({100, 100});
    g.mouseDown({700, 500}, leftButton);
    g.mouseMove({750, 530});
    g.mouseUp({750, 530});
    CHECK(g.pan().x == 20.0f && g.pan().y == 20.0f && m.selected() == invalidNodeId);
    g.mouseDown({10, 10}, middleButton);
    g.mouseMove({20, 15});
    g.mouseUp({20, 15});
    CHECK(g.pan().x == 30.0f && g.pan().y == 25.0f);
    (void)before;

    const Vec cursor{300, 200};
    const Vec world = g.toWorld(cursor);
    CHECK(g.wheel(cursor, 1.0f) && g.zoom() > 1.0f);
    CHECK(std::fabs(g.toScreen(world).x - cursor.x) < 0.01f && std::fabs(g.toScreen(world).y - cursor.y) < 0.01f);
    for (int i = 0; i < 60; ++i) g.wheel(cursor, -1.0f);
    CHECK(g.zoom() == 0.25f);

    for (int i = 0; i < 5; ++i) {
        m.select(a);
        m.addNode("transform");
    }
    g.frameAll(600, 300);
    for (NodeId id : m.network().nodeIds()) {
        const Vec tl = g.toScreen({m.network().node(id).posX(), m.network().node(id).posY()});
        const Vec br = g.toScreen({m.network().node(id).posX() + nodeWidth, m.network().node(id).posY() + nodeHeight});
        CHECK(tl.x >= 0 && tl.y >= 0 && br.x <= 600 && br.y <= 300);
    }
}

TEST(deleteAndDoubleClickCommitAndUndo) {
    EditorModel m;
    const NodeId a = m.addNode("line");
    const NodeId b = m.addNode("transform");
    const NodeId c = m.addNode("null");
    GraphController g(m);
    int graphChanges = 0;
    g.onChanged = [&](bool graph) { graphChanges += graph ? 1 : 0; };

    CHECK(g.doubleClick(center(g, b)) && m.network().displayNode() == b && m.undoLabel() == "set display");
    CHECK(!g.doubleClick({5000, 5000}));
    g.mouseDown(center(g, c), leftButton);
    g.mouseUp(center(g, c));
    CHECK(g.deleteSelected());
    CHECK(m.network().nodeIds().size() == 2 && m.selected() == invalidNodeId && !g.deleteSelected());
    CHECK(graphChanges == 2);
    m.undo();
    CHECK(m.network().nodeIds().size() == 3 && m.network().inputOf(c, 0) == b && m.network().node(c).hasPosition());
    CHECK(m.network().inputOf(b, 0) == a);
}

TEST(openedGraphsWithoutPositionsGetLaidOut) {
    const std::string path = (std::filesystem::temp_directory_path() / "jitnodes_editor_layout.json").string();
    {
        Network net(1);
        const NodeId a = net.addNode("line");
        const NodeId b = net.addNode("transform");
        const NodeId c = net.addNode("transform");
        net.connect(a, b);
        net.connect(a, c);
        saveNetworkToFile(net, path);
    }
    EditorModel m;
    m.open(path);
    std::filesystem::remove(path);
    for (NodeId id : m.network().nodeIds()) CHECK(m.network().node(id).hasPosition());
    CHECK(!m.modified() && !m.canUndo());
    const Node& b = m.network().node(m.network().nodeIds()[1]);
    const Node& c = m.network().node(m.network().nodeIds()[2]);
    CHECK(b.posX() != c.posX() || b.posY() != c.posY());
}


TEST(parametersListFloatsThenStringsAndFlagAnimation) {
    EditorModel m;
    const NodeId wr = m.addNode("wrangle");
    std::vector<ParamInfo> p = m.parameters(wr);
    CHECK(p.size() == 6 && p[0].name == "a" && !p[0].isText && p[4].name == "class" && p[4].isText);
    CHECK(p[5].name == "code" && p[5].multiline && !p[4].multiline);
    m.network().node(wr).setExpression("b", "time * 2");
    m.network().node(wr).setKeyframes("c", {{0.0, 1.0f, KeyInterp::linear}, {4.0, 5.0f, KeyInterp::linear}});
    m.setDuration(4.0);
    m.setPlayheadFraction(0.5);
    p = m.parameters(wr);
    CHECK(p[1].expression && p[1].text == "time * 2" && !p[1].keyed);
    CHECK(p[2].keyed && !p[2].expression && p[2].text == "3");
    CHECK(!p[0].expression && p[0].text == "0");
}

TEST(setParameterAcceptsNumbersExpressionsAndText) {
    EditorModel m;
    const NodeId xf = m.addNode("transform");
    CHECK(m.setParameter(xf, "tx", "2.5").empty());
    CHECK(m.network().node(xf).baseFloat("tx") == 2.5f && m.undoLabel() == "edit transform1.tx");
    CHECK(m.setParameter(xf, "tx", "sin(time) * 3").empty() && m.network().node(xf).isAnimated("tx"));
    CHECK(m.setParameter(xf, "tx", " 7 ").empty() && !m.network().node(xf).isAnimated("tx") && m.network().node(xf).baseFloat("tx") == 7.0f);
    CHECK(m.setParameter(xf, "tx", "").empty() && !m.network().node(xf).isAnimated("tx"));

    const size_t before = m.network().node(xf).cookCount();
    const std::string label = m.undoLabel();
    CHECK(!m.setParameter(xf, "ty", "1 +").empty());
    CHECK(!m.network().node(xf).isAnimated("ty") && m.undoLabel() == label);
    CHECK(!m.setParameter(xf, "nope", "1").empty());
    (void)before;

    const NodeId wr = m.addNode("wrangle");
    CHECK(m.setParameter(wr, "code", "y = x * 2;").empty() && m.network().node(wr).getString("code") == "y = x * 2;");
    m.undo();
    CHECK(m.network().node(wr).getString("code").empty());
}

TEST(expressionToggleAndKeyframesFollowThePlayhead) {
    EditorModel m;
    const NodeId xf = m.addNode("transform");
    m.setDuration(4.0);
    m.network().node(xf).setFloat("sx", 2.0f);
    CHECK(m.toggleExpression(xf, "sx").empty() && m.network().node(xf).expression("sx") == "2");
    CHECK(m.toggleExpression(xf, "sx").empty() && m.network().node(xf).expression("sx").empty() && m.network().node(xf).baseFloat("sx") == 2.0f);
    CHECK(!m.toggleExpression(xf, "nope").empty());

    m.network().node(xf).setKeyframes("tx", {{0.0, 0.0f, KeyInterp::linear}, {4.0, 8.0f, KeyInterp::linear}});
    m.setPlayheadFraction(0.5);
    CHECK(m.keyframeNow(xf, "tx").empty());
    CHECK(m.network().node(xf).keyframes("tx").size() == 3 && m.undoLabel() == "key transform1.tx");
    CHECK(std::fabs(m.network().node(xf).keyframes("tx")[1].value - 4.0f) < 1e-4f);
    CHECK(m.keyframeNow(xf, "tx").empty() && m.network().node(xf).keyframes("tx").size() == 3);
    m.setPlayheadFraction(0.25);
    CHECK(m.keyframeNow(xf, "ty").empty() && m.network().node(xf).isAnimated("ty") && m.network().node(xf).keyframes("ty").size() == 1);
    m.undo();
    CHECK(!m.network().node(xf).isAnimated("ty"));
    CHECK(!m.keyframeNow(xf, "code").empty());
}


TEST(cookKeepsTheDisplayGeometryAndFailureClearsIt) {
    EditorModel m;
    CHECK(m.geometry() == nullptr);
    m.addNode("line");
    m.cook();
    CHECK(m.geometry() && m.geometry()->pointCount() == 100);
    m.select(invalidNodeId);
    m.addNode("transform");
    CHECK(!m.cook().ok && m.geometry() == nullptr);
}

TEST(viewportFitsTheGeometryAndKeepsYUp) {
    EditorModel m;
    const NodeId line = m.addNode("line");
    m.network().node(line).setFloat("length", 10.0f);
    m.network().node(line).setFloat("y", 2.0f);
    m.addNode("transform");
    m.network().node(m.selected()).setFloat("sy", 1.0f);
    m.cook();

    ViewportController v;
    v.setGeometry(m.geometry());
    CHECK(v.bounds().valid && v.bounds().minX == 0.0f && std::fabs(v.bounds().maxX - 10.0f) < 1e-4f && v.bounds().minY == 2.0f && v.bounds().maxY == 2.0f);
    CHECK(v.info() == "100 points, 1 primitive");
    v.setViewSize(400, 300);
    CHECK(std::fabs(v.zoom() - 352.0f / 10.0f) < 1e-3f);
    const Vec a = v.toScreen({0, 2}), b = v.toScreen({10, 2});
    CHECK(a.x >= 24 - 1e-3f && b.x <= 376 + 1e-3f && std::fabs(a.y - 150.0f) < 1e-3f);
    const Vec up = v.toScreen({0, 3}), down = v.toScreen({0, 1});
    CHECK(up.y < down.y);
    const Vec round = v.toWorld(v.toScreen({3.5f, -1.25f}));
    CHECK(std::fabs(round.x - 3.5f) < 1e-4f && std::fabs(round.y + 1.25f) < 1e-4f);
}

TEST(viewportPansZoomsAndRefits) {
    EditorModel m;
    m.addNode("line");
    m.cook();
    ViewportController v;
    v.setViewSize(400, 300);
    CHECK(v.info() == "no geometry");
    v.setGeometry(m.geometry());
    const float fitted = v.zoom();
    const Vec panBefore = v.pan();

    CHECK(!v.mouseDown({100, 100}, leftButton));
    CHECK(v.mouseMove({130, 90}) && v.pan().x == panBefore.x + 30 && v.pan().y == panBefore.y - 10);
    v.mouseUp({130, 90});
    CHECK(!v.mouseMove({200, 200}));

    const Vec cursor{250, 120};
    const Vec world = v.toWorld(cursor);
    CHECK(v.wheel(cursor, 1.0f) && v.zoom() > fitted);
    CHECK(std::fabs(v.toScreen(world).x - cursor.x) < 1e-2f && std::fabs(v.toScreen(world).y - cursor.y) < 1e-2f);

    m.addNode("transform");
    m.network().node(m.selected()).setFloat("sx", 2.0f);
    m.cook();
    v.setGeometry(m.geometry());
    CHECK(std::fabs(v.zoom() - fitted) > 1e-3f);
    CHECK(v.doubleClick());
    CHECK(std::fabs(v.zoom() - 352.0f / 20.0f) < 1e-3f);
    v.setGeometry(nullptr);
    CHECK(v.info() == "no geometry" && !v.bounds().valid);
}

TEST(drawStrideKeepsHugeGeometriesWithinTheBudget) {
    CHECK(ViewportController::drawStride(100, 40000) == 1 && ViewportController::drawStride(40000, 40000) == 1);
    CHECK(ViewportController::drawStride(1000000, 40000) == 25 && ViewportController::drawStride(40001, 40000) == 2);
    CHECK(1000000 / ViewportController::drawStride(1000000, 40000) <= 40000);
    CHECK(ViewportController::drawStride(10, 0) == 1);
}


TEST(profileRowsFollowCooksFusionAndReset) {
    EditorModel m;
    CHECK(m.profile().empty());
    m.addNode("line");
    m.addNode("transform");
    m.addNode("wrangle");
    m.addNode("null");
    CHECK(m.profile().size() == 4 && m.profile()[0].cooks == 0 && m.profile()[0].note.empty());
    CHECK(m.cook().ok);
    const std::vector<ProfileRow> rows = m.profile();
    CHECK(rows[0].name == "line1" && rows[0].type == "line" && rows[0].cooks == 1 && rows[0].lastMs >= 0.0 && rows[0].depth == 0);
    CHECK(rows[1].note.find("fused into wrangle3") == 0);
    CHECK(rows[2].note.find("fused chain of 2") == 0);
    CHECK(rows[3].type == "null" && rows[3].cooks == 1 && rows[3].note.empty());
    m.resetProfile();
    CHECK(m.profile()[0].cooks == 0 && m.profile()[0].lastMs == 0.0);
}

TEST(profileRowsIncludeSubnetContentsOneLevelDeeper) {
    EditorModel m;
    const NodeId line = m.addNode("line");
    const NodeId sub = m.addNode("subnet");
    Network& inner = dynamic_cast<SubnetNode&>(m.network().node(sub)).network();
    const NodeId in = inner.addNode("subnetInput", "in");
    const NodeId xf = inner.addNode("transform", "innerXf");
    inner.connect(in, xf);
    inner.setDisplayNode(xf);
    (void)line;
    CHECK(m.cook().ok);
    const std::vector<ProfileRow> rows = m.profile();
    CHECK(rows.size() == 4 && rows[1].name == "subnet2" && rows[1].depth == 0);
    CHECK(rows[2].depth == 1 && rows[2].name == "in" && rows[3].name == "innerXf" && rows[3].depth == 1);
}

TEST(profileListScrollsClampsAndMapsRowsToNodes) {
    std::vector<ProfileRow> rows;
    for (int i = 0; i < 30; ++i) {
        ProfileRow r;
        r.id = static_cast<NodeId>(i + 1);
        r.depth = i == 5 ? 1 : 0;
        r.name = "n" + std::to_string(i);
        rows.push_back(r);
    }
    ProfileController p;
    p.setViewHeight(100.0f);
    p.setRows(rows);
    CHECK(p.maxScroll() == 30 * 20 - 100.0f && p.scroll() == 0.0f);
    CHECK(!p.wheel(1.0f));
    CHECK(p.wheel(-1.0f) && p.scroll() == 60.0f);
    CHECK(p.wheel(1.0f) && p.scroll() == 0.0f);
    for (int i = 0; i < 50; ++i) p.wheel(-1.0f);
    CHECK(p.scroll() == p.maxScroll());
    CHECK(!p.rowAt(-1.0f));
    CHECK(p.rowAt(0.0f) == std::optional<size_t>(25));
    CHECK(p.selectableNode(5) == invalidNodeId && p.selectableNode(6) == 7 && p.selectableNode(500) == invalidNodeId);
    p.setRows({rows.begin(), rows.begin() + 3});
    CHECK(p.scroll() == 0.0f && !p.rowAt(70.0f) && p.rowAt(59.0f) == std::optional<size_t>(2));
}


TEST(keyframeEditorKeepsKeysSortedAndRejectsDuplicates) {
    KeyframeEditor e({{4.0, 8.0f, KeyInterp::linear}, {0.0, 0.0f, KeyInterp::linear}, {2.0, 5.0f, KeyInterp::smooth}});
    CHECK(e.size() == 3 && e.keys()[0].time == 0.0 && e.keys()[1].time == 2.0 && e.keys()[2].time == 4.0);

    size_t i = 1;
    CHECK(e.setTime(i, 3.0).empty() && i == 1 && e.keys()[1].time == 3.0 && e.keys()[1].value == 5.0f);
    i = 0;
    CHECK(e.setTime(i, 9.0).empty() && i == 2 && e.keys()[2].value == 0.0f && e.keys()[0].time == 3.0);
    i = 1;
    const std::vector<Keyframe> before = e.keys();
    CHECK(!e.setTime(i, 3.0).empty() && e.keys().size() == before.size() && e.keys()[0].time == before[0].time);
    CHECK(!e.setTime(i, std::nan("")).empty());
    i = 7;
    CHECK(!e.setTime(i, 1.0).empty());

    CHECK(e.setValue(0, -2.0f).empty() && e.keys()[0].value == -2.0f && !e.setValue(0, INFINITY).empty() && !e.setValue(9, 1.0f).empty());
    CHECK(e.setInterp(0, KeyInterp::constant).empty() && e.keys()[0].interp == KeyInterp::constant && !e.setInterp(9, KeyInterp::linear).empty());
    CHECK(e.remove(1).empty() && e.size() == 2 && !e.remove(5).empty());
}

TEST(keyframeEditorAddsKeysOnTheCurveAndReportsTheRange) {
    KeyframeEditor e({{0.0, 0.0f, KeyInterp::linear}, {4.0, 8.0f, KeyInterp::linear}});
    size_t i = 0;
    CHECK(e.addAt(1.0, i).empty() && i == 1 && std::fabs(e.keys()[1].value - 2.0f) < 1e-5f && std::fabs(e.valueAt(3.0) - 6.0f) < 1e-5f);
    CHECK(!e.addAt(1.0, i).empty() && e.size() == 3);
    CHECK(e.addAt(-1.0, i).empty() && i == 0 && e.keys()[0].value == 0.0f);
    const KeyframeEditor::Range r = e.range();
    CHECK(r.t0 == -1.0 && r.t1 == 4.0 && r.v0 == 0.0f && r.v1 == 8.0f);

    KeyframeEditor empty;
    CHECK(empty.valueAt(3.0) == 0.0f && empty.range().t1 > empty.range().t0 && empty.range().v1 > empty.range().v0);
    CHECK(empty.addAt(2.0, i).empty() && empty.keys()[0].value == 0.0f);
    KeyframeEditor flat({{1.0, 3.0f, KeyInterp::linear}});
    CHECK(flat.range().t1 > flat.range().t0 && flat.range().v1 > flat.range().v0);

    KeyInterp k = KeyInterp::linear;
    CHECK(parseInterp("smooth", k) && k == KeyInterp::smooth && !parseInterp("cubic", k) && std::string(interpName(KeyInterp::constant)) == "constant");
}

TEST(modelReplacesKeyframesAsOneUndoStep) {
    EditorModel m;
    const NodeId xf = m.addNode("transform");
    m.setDuration(4.0);
    CHECK(m.keyframes(xf, "tx").empty() && m.keyframes(xf, "code").empty());
    const std::vector<Keyframe> keys = {{0.0, 1.0f, KeyInterp::linear}, {2.0, 5.0f, KeyInterp::smooth}};
    CHECK(m.setKeyframes(xf, "tx", keys).empty() && m.keyframes(xf, "tx").size() == 2 && m.undoLabel() == "keys transform1.tx");
    m.setPlayheadFraction(0.0);
    CHECK(m.parameters(xf)[2].keyed);
    m.setPlayheadFraction(0.25);
    CHECK(m.setKeyframes(xf, "tx", {}).empty() && !m.network().node(xf).isAnimated("tx"));
    const float held = m.network().node(xf).baseFloat("tx");
    CHECK(held > 1.0f && held < 5.0f);
    m.undo();
    CHECK(m.keyframes(xf, "tx").size() == 2);
    CHECK(!m.setKeyframes(xf, "nope", keys).empty());
    CHECK(!m.setKeyframes(m.addNode("wrangle"), "code", keys).empty());
}


TEST(enteringASubnetRedirectsEditingCookingAndTheSelection) {
    EditorModel m;
    const NodeId line = m.addNode("line");
    const NodeId sub = m.addNode("subnet");
    CHECK(m.depth() == 0 && m.pathText() == "/" && m.canEnter(sub) && !m.canEnter(line) && !m.canEnter(999));
    CHECK(!m.enter(line).empty() && !m.enter(999).empty() && m.depth() == 0);
    CHECK(m.enter(sub).empty() && m.depth() == 1 && m.pathText() == "/subnet2" && m.selected() == invalidNodeId);
    CHECK(m.network().nodeIds().empty() && m.rootNetwork().nodeIds().size() == 2);
    CHECK(!m.cook().ok);

    const NodeId in = m.addNode("subnetInput");
    const NodeId xf = m.addNode("transform");
    CHECK(m.network().inputOf(xf, 0) == in && m.network().displayNode() == xf && m.network().nodeIds().size() == 2);
    CHECK(m.network().node(in).hasPosition() && m.network().node(xf).posY() > m.network().node(in).posY());
    CHECK(m.statusLine().find("2 nodes") != std::string::npos && m.statusLine().find("display: transform2") != std::string::npos);
    CHECK(m.setParameter(xf, "tx", "5").empty() && m.network().node(xf).baseFloat("tx") == 5.0f);
    const CookResult r = m.cook();
    CHECK(r.ok && r.points == 100 && m.geometry() && std::fabs(m.geometry()->x()[0] - 5.0f) < 1e-4f);
    CHECK(m.modified());

    CHECK(m.up() && m.depth() == 0 && m.selected() == sub && m.pathText() == "/" && !m.up());
    CHECK(m.network().nodeIds().size() == 2 && m.cook().ok && std::fabs(m.geometry()->x()[0] - 5.0f) < 1e-4f);
}

TEST(undoInsideASubnetKeepsOrLeavesThePathAsNeeded) {
    EditorModel m;
    m.addNode("line");
    const NodeId sub = m.addNode("subnet");
    m.enter(sub);
    const NodeId in = m.addNode("subnetInput");
    m.addNode("transform");
    CHECK(m.undo() && m.depth() == 1 && m.network().nodeIds().size() == 1 && m.network().nodeIds()[0] == in);
    CHECK(m.undo() && m.depth() == 1 && m.network().nodeIds().empty());
    CHECK(m.redo() && m.network().nodeIds().size() == 1);
    CHECK(m.undo() && m.undo() && m.depth() == 0 && m.selected() == invalidNodeId && m.network().nodeIds().size() == 1);
    CHECK(m.redo() && m.redo() && m.depth() == 0);
}

TEST(newAndOpenLeaveEverySubnetAndAssetInstancesCannotBeEntered) {
    const std::string path = (std::filesystem::temp_directory_path() / "jitnodes_editor_subnet.json").string();
    EditorModel m;
    const NodeId sub = m.addNode("subnet");
    m.enter(sub);
    m.addNode("subnetInput");
    m.addNode("transform");
    m.up();
    m.save(path);
    m.enter(sub);
    m.newGraph();
    CHECK(m.depth() == 0 && m.network().nodeIds().empty());
    m.open(path);
    CHECK(m.depth() == 0 && m.network().nodeIds().size() == 1);
    CHECK(m.enter(m.network().nodeIds()[0]).empty() && m.network().nodeIds().size() == 2);
    std::filesystem::remove(path);

    m.up();
    m.select(invalidNodeId);
    NodeRegistry::instance().registerType("editorTestAsset", [] { return std::make_unique<AssetNode>("editorTestAsset", 1); });
    const NodeId asset = m.addNode("editorTestAsset");
    CHECK(!m.canEnter(asset) && !m.enter(asset).empty() && m.depth() == 0);
}

TEST(promotedParametersAreAddedToSubnetsAndReadInside) {
    EditorModel m;
    const NodeId line = m.addNode("line");
    const NodeId sub = m.addNode("subnet");
    CHECK(m.addSpareParameter(sub, "amount").empty() && m.undoLabel() == "add parameter subnet2.amount");
    bool found = false;
    for (const ParamInfo& p : m.parameters(sub)) found = found || (p.name == "amount" && !p.isText);
    CHECK(found);
    CHECK(!m.addSpareParameter(sub, "").empty() && !m.addSpareParameter(sub, "1x").empty() && !m.addSpareParameter(sub, "a b").empty());
    CHECK(!m.addSpareParameter(sub, "amount").empty() && !m.addSpareParameter(sub, "time").empty() && !m.addSpareParameter(sub, "parent").empty());
    CHECK(!m.addSpareParameter(line, "x").empty());

    m.enter(sub);
    m.addNode("subnetInput");
    const NodeId xf = m.addNode("transform");
    CHECK(m.setParameter(xf, "tx", "parent.amount * 2").empty());
    m.up();
    CHECK(m.setParameter(sub, "amount", "3").empty());
    CHECK(m.cook().ok && std::fabs(m.geometry()->x()[0] - 6.0f) < 1e-4f);
    m.undo();
    m.undo();
    m.undo();
    m.undo();
    m.undo();
    CHECK(m.depth() == 0);
    bool still = false;
    for (const ParamInfo& p : m.parameters(sub)) still = still || p.name == "amount";
    CHECK(!still);
}

TEST(subnetContentsOfLoadedFilesGetPositions) {
    const std::string path = (std::filesystem::temp_directory_path() / "jitnodes_editor_subnet_layout.json").string();
    {
        Network net(1);
        const NodeId sub = net.addNode("subnet", "sub");
        Network& inner = dynamic_cast<SubnetNode&>(net.node(sub)).network();
        const NodeId in = inner.addNode("subnetInput", "in");
        const NodeId xf = inner.addNode("transform", "xf");
        inner.connect(in, xf);
        inner.setDisplayNode(xf);
        saveNetworkToFile(net, path);
    }
    EditorModel m;
    m.open(path);
    std::filesystem::remove(path);
    m.enter(m.network().nodeIds()[0]);
    for (NodeId id : m.network().nodeIds()) CHECK(m.network().node(id).hasPosition());
    CHECK(!m.modified());
}

TEST(doubleClickEntersSubnetsAndSetsTheDisplayOnOtherNodes) {
    EditorModel m;
    const NodeId line = m.addNode("line");
    const NodeId sub = m.addNode("subnet");
    m.select(invalidNodeId);
    const NodeId other = m.addNode("null");
    GraphController g(m);
    int navigated = 0;
    g.onNavigated = [&] { ++navigated; };

    CHECK(g.doubleClick(center(g, other)) && m.network().displayNode() == other && m.depth() == 0 && navigated == 0);
    CHECK(g.doubleClick(center(g, sub)) && m.depth() == 1 && navigated == 1 && m.selected() == invalidNodeId);
    CHECK(!g.enterSelected());
    const NodeId inner = m.addNode("subnetInput");
    CHECK(!g.enterSelected() && m.selected() == inner);
    CHECK(g.up() && m.depth() == 0 && navigated == 2 && m.selected() == sub && !g.up());
    CHECK(g.enterSelected() && m.depth() == 1 && navigated == 3);
    CHECK(m.network().nodeIds().size() == 1 && m.rootNetwork().node(line).hasPosition());
}


TEST(subnetsBecomeAssetsGetUpdatedAndCanBeUnlocked) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "jitnodes_editor_assets";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string pathA = (dir / "a.asset.json").string(), pathB = (dir / "b.asset.json").string();

    EditorModel m;
    const NodeId line = m.addNode("line");
    const NodeId sub = m.addNode("subnet");
    CHECK(m.addSpareParameter(sub, "amount").empty());
    m.enter(sub);
    m.addNode("subnetInput");
    const NodeId xf = m.addNode("transform");
    CHECK(m.setParameter(xf, "tx", "parent.amount * 2").empty());
    m.up();
    CHECK(m.setParameter(sub, "amount", "3").empty());

    CHECK(!m.createAsset(line, "editorMadeAsset", pathA, true).empty());
    CHECK(!m.createAsset(sub, "1bad", pathA, true).empty() && !m.createAsset(sub, "line", pathA, true).empty());
    CHECK(!std::filesystem::exists(pathA) && m.canEnter(sub));

    size_t updated = 99;
    CHECK(m.createAsset(sub, "editorMadeAsset", pathA, true, &updated).empty() && updated == 0 && std::filesystem::exists(pathA));
    const NodeRegistry& registry = NodeRegistry::instance();
    CHECK(m.isAssetInstance(sub) && m.network().node(sub).typeName() == "editorMadeAsset" && m.network().node(sub).name() == "subnet2");
    CHECK(m.network().inputOf(sub, 0) == line && m.network().node(sub).baseFloat("amount") == 3.0f && !m.canEnter(sub));
    CHECK(m.undoLabel() == "create asset editorMadeAsset" && registry.assetVersion("editorMadeAsset") == 1);
    const std::vector<std::string> names = m.assetNames();
    CHECK(std::find(names.begin(), names.end(), "editorMadeAsset") != names.end());
    CHECK(m.cook().ok && std::fabs(m.geometry()->x()[0] - 6.0f) < 1e-4f);

    CHECK(m.undo() && m.network().node(sub).typeName() == "subnet" && m.canEnter(sub));
    CHECK(m.redo() && m.isAssetInstance(sub) && m.network().node(sub).baseFloat("amount") == 3.0f);

    m.select(invalidNodeId);
    const NodeId sub2 = m.addNode("subnet");
    m.addSpareParameter(sub2, "amount");
    m.enter(sub2);
    m.addNode("subnetInput");
    const NodeId xf2 = m.addNode("transform");
    m.setParameter(xf2, "tx", "parent.amount * 10");
    m.up();
    CHECK(m.createAsset(sub2, "editorMadeAsset", pathB, false, &updated).empty() && updated == 1);
    CHECK(registry.assetVersion("editorMadeAsset") == 2 && !m.canUndo() && m.isAssetInstance(sub) && m.canEnter(sub2));
    m.setDisplay(sub);
    CHECK(m.cook().ok && std::fabs(m.geometry()->x()[0] - 30.0f) < 1e-4f);

    CHECK(m.unlockAsset(sub).empty() && !m.isAssetInstance(sub) && m.canEnter(sub) && m.undoLabel() == "unlock subnet2");
    m.enter(sub);
    CHECK(m.network().nodeIds().size() == 2);
    m.up();
    CHECK(m.network().node(sub).baseFloat("amount") == 3.0f && m.cook().ok && std::fabs(m.geometry()->x()[0] - 30.0f) < 1e-4f);
    CHECK(!m.unlockAsset(sub).empty() && !m.unlockAsset(line).empty());

    CHECK(!m.loadAssetFile((dir / "missing.asset.json").string()).empty());
    size_t loaded = 0;
    CHECK(m.loadAssetsFrom(dir.string(), &loaded).empty() && loaded == 2);
    CHECK(m.loadAssetsFrom((dir / "nope").string(), &loaded).empty() && loaded == 0);
    CHECK(registry.assetVersion("editorMadeAsset") == 2);
    std::filesystem::remove_all(dir);
}


GeometryHandle attributeGeometry() {
    EditorModel m;
    m.addNode("line");
    const NodeId wr = m.addNode("wrangle");
    m.setParameter(wr, "code", "temp = x * 0.1; N.x = x; N.y = 1;");
    const NodeId prim = m.addNode("wrangle");
    m.setParameter(prim, "class", "primitive");
    m.setParameter(prim, "code", "w = 3;");
    if (!m.cook().ok) return nullptr;
    return m.geometry();
}

std::optional<size_t> choiceIndex(const ViewportController& v, const std::string& label) {
    for (size_t i = 0; i < v.choices().size(); ++i)
        if (v.choices()[i].label == label) return i;
    return std::nullopt;
}

TEST(viewportListsAttributesAndComputesTheirValuesAndRange) {
    const GeometryHandle geo = attributeGeometry();
    CHECK(geo != nullptr);
    ViewportController v;
    v.setViewSize(400, 300);
    CHECK(v.choices().empty() && !v.hasValues());
    v.setGeometry(geo);
    for (const char* label : {"x", "y", "temp", "N.x", "N.y", "N (length)", "prim: w"}) CHECK(choiceIndex(v, label).has_value());
    CHECK(!choiceIndex(v, "group.none") && !v.selectedChoice() && !v.hasValues());

    v.selectChoice(choiceIndex(v, "temp"));
    CHECK(v.hasValues() && v.valueClass() == AttribClass::point && v.values().size() == 100 && v.valueLabel() == "temp");
    CHECK(std::fabs(v.valueMin() - 0.0f) < 1e-5f && std::fabs(v.valueMax() - 1.0f) < 1e-4f);
    CHECK(std::fabs(v.values()[99] - 1.0f) < 1e-4f && v.colorOf(v.valueMin()) == ViewportController::colormap(0.0f) &&
          v.colorOf(v.valueMax()) == ViewportController::colormap(1.0f));

    v.selectChoice(choiceIndex(v, "N (length)"));
    CHECK(v.values().size() == 100 && std::fabs(v.valueMin() - 1.0f) < 1e-5f && std::fabs(v.valueMax() - std::sqrt(101.0f)) < 1e-3f);

    v.selectChoice(choiceIndex(v, "prim: w"));
    CHECK(v.valueClass() == AttribClass::primitive && v.values().size() == 1 && v.values()[0] == 3.0f);
    CHECK(v.valueMin() == 2.5f && v.valueMax() == 3.5f);

    v.selectChoice(choiceIndex(v, "temp"));
    EditorModel other;
    other.addNode("line");
    other.cook();
    v.setGeometry(other.geometry());
    CHECK(!v.selectedChoice() && !v.hasValues() && v.valueLabel().empty());
    v.setGeometry(geo);
    CHECK(v.selectedChoice() == choiceIndex(v, "temp") && v.hasValues());
    v.setGeometry(attributeGeometry());
    CHECK(v.selectedChoice() && v.valueLabel() == "temp" && v.values().size() == 100);
    v.selectChoice(std::nullopt);
    CHECK(!v.hasValues());
    v.selectChoice(9999);
    CHECK(!v.hasValues());
}

TEST(colormapRunsFromDarkPurpleToYellowAndHandlesBadValues) {
    CHECK(ViewportController::colormap(0.0f) == 0xFF440154u && ViewportController::colormap(1.0f) == 0xFFFDE725u);
    CHECK(ViewportController::colormap(-3.0f) == 0xFF440154u && ViewportController::colormap(7.0f) == 0xFFFDE725u);
    CHECK(ViewportController::colormap(0.5f) == 0xFF21918Cu && ViewportController::colormap(0.25f) == 0xFF3B528Bu);
    CHECK(ViewportController::colormap(std::nanf("")) == 0xFF808080u);
    float lastGreen = -1.0f;
    for (int i = 0; i <= 20; ++i) {
        const uint32_t c = ViewportController::colormap(static_cast<float>(i) / 20.0f);
        CHECK((c >> 24) == 0xFFu);
        const float green = static_cast<float>((c >> 8) & 255u);
        CHECK(green >= lastGreen - 1.0f);
        lastGreen = green;
    }
}

TEST(hoveringFindsTheNearestPointAndDescribesIt) {
    const GeometryHandle geo = attributeGeometry();
    ViewportController v;
    v.setViewSize(400, 300);
    v.setGeometry(geo);
    v.selectChoice(choiceIndex(v, "temp"));
    const Vec onPoint = v.toScreen({geo->x()[40], geo->y()[40]});
    CHECK(v.nearestPoint({onPoint.x + 1.0f, onPoint.y - 1.0f}, 5.0f) == std::optional<size_t>(40));
    CHECK(!v.nearestPoint({onPoint.x, onPoint.y + 60.0f}, 5.0f));

    CHECK(v.mouseMove({onPoint.x + 1.0f, onPoint.y}) && v.hover() == std::optional<size_t>(40));
    CHECK(!v.mouseMove({onPoint.x + 1.2f, onPoint.y}));
    const std::string text = v.describePoint(40);
    CHECK(text.rfind("#40", 0) == 0 && text.find("temp=") != std::string::npos && text.find("x=") != std::string::npos);
    v.selectChoice(choiceIndex(v, "prim: w"));
    CHECK(v.describePoint(40).find("prim: w=3") != std::string::npos);
    v.selectChoice(std::nullopt);
    CHECK(v.describePoint(40).find("=") != std::string::npos && v.describePoint(40).find("temp") == std::string::npos && v.describePoint(5000).empty());

    CHECK(v.mouseMove({onPoint.x, onPoint.y + 90.0f}) && !v.hover());
    v.mouseMove({onPoint.x, onPoint.y});
    CHECK(v.hover());
    v.clearHover();
    CHECK(!v.hover());
    v.mouseDown({10, 10}, leftButton);
    CHECK(v.mouseMove({20, 15}) && !v.hover());
    v.mouseUp({20, 15});
}

TEST(renamingANodeIsOneUndoStepAndRejectsBadNames) {
    EditorModel m;
    const NodeId a = m.addNode("line");
    const NodeId b = m.addNode("transform");
    m.network().node(b).setExpression("tx", "line1.length");
    m.commit("expression");
    CHECK(m.renameNode(a, "source").empty() && m.network().node(a).name() == "source" && m.undoLabel() == "rename line1");
    CHECK(m.network().node(b).expression("tx") == "source.length");
    CHECK(!m.renameNode(a, "transform2").empty() && !m.renameNode(a, "9lives").empty() && m.network().node(a).name() == "source");
    CHECK(m.renameNode(a, "source").empty() && m.undoLabel() == "rename line1");
    m.undo();
    CHECK(m.network().node(a).name() == "line1" && m.network().node(b).expression("tx") == "line1.length");
    m.redo();
    CHECK(m.network().node(a).name() == "source" && m.cook().ok);
}

TEST(copyCutAndPasteWorkOnTheSelectedNode) {
    EditorModel m;
    CHECK(!m.copySelected() && !m.cutSelected() && !m.canPaste() && !m.paste().empty());
    const NodeId a = m.addNode("line");
    const NodeId b = m.addNode("wrangle");
    m.network().node(b).setString("code", "y = x * 2;");
    m.commit("code");
    m.moveNode(b, 100.0f, 50.0f);
    CHECK(m.copySelected() && m.canPaste());

    CHECK(m.paste().empty() && m.network().nodeIds().size() == 3);
    const NodeId first = m.selected();
    CHECK(first != b && m.network().node(first).name() == "wrangle1" && m.network().node(first).getString("code") == "y = x * 2;");
    CHECK(m.network().inputOf(first, 0) == a && m.network().displayNode() == b);
    CHECK(m.network().node(first).posX() == 130.0f && m.network().node(first).posY() == 80.0f);
    CHECK(m.undoLabel() == "paste wrangle1");
    CHECK(m.paste().empty() && m.network().node(m.selected()).posX() == 160.0f);

    m.undo();
    m.undo();
    CHECK(m.network().nodeIds().size() == 2);

    m.select(b);
    CHECK(m.cutSelected() && m.network().nodeIds().size() == 1 && m.selected() == invalidNodeId && m.undoLabel() == "cut wrangle2");
    CHECK(m.paste().empty() && m.network().node(m.selected()).name() == "wrangle2" && m.network().inputOf(m.selected(), 0) == a);
    m.undo();
    m.undo();
    CHECK(m.network().nodeIds().size() == 2 && m.network().node(b).getString("code") == "y = x * 2;");
}

TEST(controllerClipboardRepaintsAndReportsErrors) {
    EditorModel m;
    m.addNode("line");
    GraphController g(m);
    int graphChanges = 0, otherChanges = 0;
    g.onChanged = [&](bool graph) { (graph ? graphChanges : otherChanges)++; };
    CHECK(g.paste() && !g.message().empty() && otherChanges == 1);
    CHECK(g.copySelected() && g.message().empty());
    CHECK(g.paste() && graphChanges == 1 && m.network().nodeIds().size() == 2);
    CHECK(g.cutSelected() && graphChanges == 2 && m.network().nodeIds().size() == 1);
    CHECK(!g.cutSelected() && !g.copySelected());
}

TEST(modelKeepsASetOfSelectedNodes) {
    EditorModel m;
    const NodeId a = m.addNode("line");
    const NodeId b = m.addNode("transform");
    const NodeId c = m.addNode("null");
    CHECK(m.selected() == c && m.selection().size() == 1);
    m.setSelection({a, b});
    CHECK(m.selected() == invalidNodeId && m.selection().size() == 2 && m.isSelected(a) && !m.isSelected(c));
    m.toggleSelected(a);
    CHECK(m.selected() == b);
    m.toggleSelected(c);
    m.toggleSelected(a);
    CHECK(m.selection().size() == 3);
    m.selectAll();
    CHECK(m.selection().size() == 3);
    m.select(invalidNodeId);
    CHECK(m.selection().empty());

    m.setSelection({a, b});
    CHECK(m.removeSelected() && m.network().nodeIds().size() == 1 && m.selection().empty() && m.undoLabel() == "delete 2 nodes");
    CHECK(!m.removeSelected());
    m.undo();
    CHECK(m.network().nodeIds().size() == 3 && m.network().inputOf(c, 0) == b);
    m.removeNode(a);
    m.select(b);
    CHECK(m.removeSelected() && m.undoLabel() == "delete transform2");
}

TEST(clickingWithShiftTogglesAndDraggingMovesTheWholeSelection) {
    EditorModel m;
    const NodeId a = m.addNode("line");
    const NodeId b = m.addNode("transform");
    const NodeId c = m.addNode("null");
    GraphController g(m);
    int graphChanges = 0, otherChanges = 0;
    g.onChanged = [&](bool graph) { (graph ? graphChanges : otherChanges)++; };

    g.mouseDown(center(g, a), leftButton);
    g.mouseUp(center(g, a));
    g.mouseDown(center(g, b), leftButton, shiftKey);
    g.mouseUp(center(g, b));
    CHECK(m.selection().size() == 2 && m.isSelected(a) && m.isSelected(b) && !m.isSelected(c));
    g.mouseDown(center(g, c), leftButton, ctrlKey);
    g.mouseUp(center(g, c));
    CHECK(m.selection().size() == 3);
    g.mouseDown(center(g, c), leftButton, shiftKey);
    g.mouseUp(center(g, c));
    CHECK(m.selection().size() == 2 && !m.isSelected(c) && m.network().node(c).hasPosition());

    const float ax = m.network().node(a).posX(), bx = m.network().node(b).posX(), cx = m.network().node(c).posX();
    const Vec start = center(g, a);
    g.mouseDown(start, leftButton);
    g.mouseMove({start.x + 40, start.y + 20});
    g.mouseUp({start.x + 40, start.y + 20});
    CHECK(m.network().node(a).posX() == ax + 40 && m.network().node(b).posX() == bx + 40 && m.network().node(c).posX() == cx);
    CHECK(m.selection().size() == 2 && m.undoLabel() == "move nodes");
    m.undo();
    CHECK(m.network().node(a).posX() == ax && m.network().node(b).posX() == bx);

    g.mouseDown(center(g, a), leftButton);
    g.mouseUp(center(g, a));
    CHECK(m.selected() == a);
    CHECK(g.selectAll() && m.selection().size() == 3);
    CHECK(g.deleteSelected() && m.network().nodeIds().empty());
}

TEST(draggingABoxSelectsTheNodesItTouches) {
    EditorModel m;
    const NodeId a = m.addNode("line");
    const NodeId b = m.addNode("transform");
    const NodeId c = m.addNode("null");
    m.moveNode(a, 0, 0);
    m.moveNode(b, 300, 0);
    m.moveNode(c, 600, 300);
    GraphController g(m);
    g.onChanged = [](bool) {};

    const Vec origin = g.toScreen({-10, -10});
    CHECK(g.mouseDown(origin, leftButton) && g.marquee().has_value());
    g.mouseMove(g.toScreen({20, 20}));
    CHECK(m.selection() == std::vector<NodeId>{a});
    g.mouseMove(g.toScreen({320, 30}));
    CHECK(m.selection() == (std::vector<NodeId>{a, b}));
    g.mouseMove(g.toScreen({-5, -5}));
    CHECK(m.selection().empty());
    g.mouseMove(g.toScreen({320, 30}));
    g.mouseUp(g.toScreen({320, 30}));
    CHECK(!g.marquee().has_value() && m.selection().size() == 2);

    g.mouseDown(g.toScreen({590, 290}), leftButton, shiftKey);
    g.mouseMove(g.toScreen({620, 320}));
    g.mouseUp(g.toScreen({620, 320}));
    CHECK(m.selection().size() == 3);

    g.mouseDown(g.toScreen({900, 900}), leftButton);
    g.mouseUp(g.toScreen({900, 900}));
    CHECK(m.selection().empty());
}

TEST(copyAndPasteOfSeveralNodesKeepsTheWiresBetweenThem) {
    EditorModel m;
    const NodeId a = m.addNode("line");
    const NodeId b = m.addNode("transform");
    const NodeId c = m.addNode("wrangle");
    m.network().node(c).setExpression("a", "transform2.tx + 1");
    m.commit("expression");
    m.setSelection({b, c});
    CHECK(m.copySelected());
    CHECK(m.paste().empty() && m.network().nodeIds().size() == 5 && m.selection().size() == 2 && m.undoLabel() == "paste 2 nodes");
    const NodeId b2 = m.network().findNode("transform1"), c2 = m.network().findNode("wrangle2");
    CHECK(b2 != invalidNodeId && c2 == invalidNodeId);
    const NodeId copyOfC = m.network().findNode("wrangle1");
    CHECK(copyOfC != invalidNodeId && m.network().inputOf(b2, 0) == a);
    CHECK(m.network().inputOf(copyOfC, 0) == b2);
    CHECK(m.network().node(copyOfC).expression("a") == "transform1.tx + 1");
    CHECK(m.network().inputOf(c, 0) == b);

    m.undo();
    CHECK(m.network().nodeIds().size() == 3);
    m.select(a);
    CHECK(m.cutSelected() && m.undoLabel() == "cut line1" && m.paste().empty() && m.network().findNode("line1") != invalidNodeId);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (const Test& t : tests()) {
        const int before = failures;
        try {
            t.run();
        } catch (const std::exception& e) {
            std::printf("  EXCEPTION %s\n", e.what());
            ++failures;
        }
        std::printf("[%s] %s\n", failures == before ? " ok " : "FAIL", t.name);
    }
    std::printf("%zu tests, %d failed\n", tests().size(), failures);
    return failures ? 1 : 0;
}
