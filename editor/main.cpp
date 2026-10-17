#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>

#include "editorWindow.h"
#include "newui/bundle.h"
#include "keyframeDialog.h"
#include "newui/mouse_constants.h"

extern void registerReflectionData();

using namespace jitnodes;
using namespace jitnodes::editor;

namespace {

bool click(EditorWindow& window, const char* name) {
    newui::Button& b = window.button(name);
    b.onClick.syncCall(b);
    return true;
}

// Drives the wired buttons without a mouse, so --check proves the whole path from view to model.
bool selfCheck(EditorWindow& window, EditorModel& model) {
    int failed = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "check failed: %s\n", what);
            ++failed;
        }
    };
    click(window, "addLine");
    click(window, "addTransform");
    expect(model.network().nodeIds().size() == 2, "palette buttons add nodes");
    expect(model.lastCook().ok && model.lastCook().points == 100, "adding a node cooks the display node");
    click(window, "undoButton");
    expect(model.network().nodeIds().size() == 1, "undo removes the last node");
    click(window, "redoButton");
    expect(model.network().nodeIds().size() == 2, "redo restores it");
    click(window, "addMerge");
    expect(model.network().nodeIds().size() == 3, "merge added");

    GraphCanvas& canvas = window.canvas();
    expect(canvas.isVisible(), "the canvas is visible (a plain SubView is invisible by default)");
    GraphController& graph = window.controller();
    const NodeId first = model.network().nodeIds().front();
    const Vec start = graph.toScreen({model.network().node(first).posX() + nodeWidth / 2, model.network().node(first).posY() + nodeHeight / 2});
    const float x0 = model.network().node(first).posX();
    canvas.onMouseDown.syncCall(canvas, newui::Point(start.x, start.y), newui::mbmLeftButton, 0u);
    canvas.onMouseMove.syncCall(canvas, newui::Point(start.x + 40, start.y + 10), newui::mbmLeftButton, 0u);
    canvas.onMouseUp.syncCall(canvas, newui::Point(start.x + 40, start.y + 10), newui::mbmLeftButton, 0u);
    expect(model.selected() == first && model.network().node(first).posX() == x0 + 40, "dragging a node through the canvas events moves it");
    expect(model.undoLabel() == "move node", "the move is one undo step");

    ParameterPanel& panel = window.parameters();
    model.select(model.network().nodeIds().front());
    window.refresh();
    expect(panel.rowCount() == 3 && panel.fieldOf("count") && panel.fieldOf("length"), "the panel lists the selected line's parameters");
    model.select(model.network().nodeIds()[1]);
    window.refresh();
    expect(panel.rowCount() == 4 && panel.fieldOf("tx"), "selecting another node rebuilds the rows");
    auto* tx = dynamic_cast<newui::TextField*>(panel.fieldOf("tx"));
    expect(tx != nullptr, "float parameters use text fields");
    if (tx) {
        tx->setText(L"5");
        panel.applyField("tx");
        expect(model.network().node(model.selected()).baseFloat("tx") == 5.0f, "applying a field sets the parameter");
        expect(model.undoLabel() == "edit transform2.tx", "the edit is an undo step");
        tx->setText(L"sin(time) * 2");
        panel.applyField("tx");
        expect(model.network().node(model.selected()).isAnimated("tx"), "text that is not a number becomes an expression");
        tx->setText(L"1 +");
        panel.applyField("tx");
        expect(model.network().node(model.selected()).expression("tx") == "sin(time) * 2", "a bad expression is rejected and the field reverts");
    }
    model.undo();
    window.refresh();

    ViewportController& viewport = window.viewport();
    model.setDisplay(model.network().nodeIds()[1]);
    model.cook();
    window.refresh();
    expect(viewport.geometry() == model.geometry() && viewport.bounds().valid, "the viewport follows the display node's geometry");
    expect(window.viewportCanvas().isVisible(), "the viewport canvas is visible");
    {
        BLImage vimage(500, 300, BL_FORMAT_PRGB32);
        BLContext vctx(vimage);
        window.viewportCanvas().paint(vctx);
        vctx.end();
        BLImageData vdata;
        vimage.get_data(&vdata);
        bool drawn = false;
        for (int y = 0; y < 300 && !drawn; ++y) {
            const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(vdata.pixel_data) + static_cast<size_t>(y) * static_cast<size_t>(vdata.stride));
            for (int x = 0; x < 500; ++x)
            {
                const std::uint32_t px = row[x], red = (px >> 16) & 255u, blue = px & 255u;
                if (blue > 0xB0u && red < 0x90u) {
                    drawn = true;
                    break;
                }
            }
        }
        expect(drawn, "the viewport draws the geometry in its accent colour");
    }

    ProfileController& profile = window.profile();
    window.refresh();
    expect(profile.rows().size() == model.network().nodeIds().size() && profile.rows()[0].cooks >= 1, "the profile list has a row per node");
    expect(window.profileCanvas().isVisible(), "the profile canvas is visible");
    {
        BLImage pimage(600, 100, BL_FORMAT_PRGB32);
        BLContext pctx(pimage);
        window.profileCanvas().paint(pctx);
        pctx.end();
        BLImageData pdata;
        pimage.get_data(&pdata);
        bool text = false;
        for (int y = 0; y < 20 && !text; ++y) {
            const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(pdata.pixel_data) + static_cast<size_t>(y) * static_cast<size_t>(pdata.stride));
            for (int x = 0; x < 600; ++x)
                if ((row[x] & 0xFFu) > 0xA0u) {
                    text = true;
                    break;
                }
        }
        expect(text, "the profile canvas draws the first row's text");
    }
    window.profileCanvas().onMouseDown.syncCall(window.profileCanvas(), newui::Point(20, 30), newui::mbmLeftButton, 0u);
    expect(model.selected() == profile.rows()[1].id, "clicking a profile row selects its node");

    {
        KeyframeDialog dialog("transform2.tx", {{0.0, 1.0f, KeyInterp::linear}, {2.0, 5.0f, KeyInterp::smooth}}, 1.0);
        expect(dialog.rowCount() == 2, "the keyframe dialog has a row per key");
        dialog.timeField(1).setText(L"-1");
        expect(dialog.applyRow(1).empty() && dialog.editor().keys()[0].time == -1.0, "editing a time re-sorts the keys");
        dialog.rebuildNow();
        expect(dialog.rowCount() == 2 && dialog.editor().keys()[0].value == 5.0f, "the rows follow the new order");
        dialog.valueField(0).setText(L"abc");
        expect(!dialog.applyRow(0).empty() && dialog.editor().keys()[0].value == 5.0f, "a bad value is rejected");
        dialog.valueField(0).setText(L"7.5");
        expect(dialog.applyRow(0).empty() && dialog.editor().keys()[0].value == 7.5f, "a good value is applied");
        dialog.addKeyAtPlayhead();
        expect(dialog.editor().size() == 3 && dialog.rowCount() == 3, "a key can be added at the playhead");
        expect(dialog.setInterpolation(0, KeyInterp::constant).empty() && dialog.editor().keys()[0].interp == KeyInterp::constant, "the interpolation can change");
        dialog.removeKey(1);
        expect(dialog.editor().size() == 2 && dialog.rowCount() == 2, "a key can be deleted");

        BLImage kimage(400, 150, BL_FORMAT_PRGB32);
        BLContext kctx(kimage);
        dialog.preview().paint(kctx);
        kctx.end();
        BLImageData kdata;
        kimage.get_data(&kdata);
        bool curve = false;
        for (int y = 0; y < 150 && !curve; ++y) {
            const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(kdata.pixel_data) + static_cast<size_t>(y) * static_cast<size_t>(kdata.stride));
            for (int x = 0; x < 400; ++x) {
                const std::uint32_t px = row[x], red = (px >> 16) & 255u, blue = px & 255u;
                if (blue > 0xB0u && red < 0x90u) {
                    curve = true;
                    break;
                }
            }
        }
        expect(curve, "the curve preview draws the curve");
    }

    {
        model.newGraph();
        window.refresh();
        click(window, "addLine");
        click(window, "addWrangle");
        model.setParameter(model.selected(), "code", "temp = x * 0.1;");
        model.cook();
        window.refresh();
        const auto& labels = window.attributeLabels();
        const auto at = std::find(labels.begin(), labels.end(), "temp");
        expect(labels.size() > 2 && labels[0] == "(none)" && at != labels.end(), "the Color by list shows the geometry's attributes");
        if (at != labels.end()) {
            window.viewport().selectChoice(static_cast<size_t>(at - labels.begin()) - 1);
            expect(window.viewport().hasValues() && window.viewport().values().size() == 100, "choosing an attribute gives the viewport its values");
            BLImage aimage(500, 300, BL_FORMAT_PRGB32);
            BLContext actx(aimage);
            window.viewportCanvas().paint(actx);
            actx.end();
            BLImageData adata;
            aimage.get_data(&adata);
            bool low = false, high = false;
            for (int y = 0; y < 300; ++y) {
                const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(adata.pixel_data) + static_cast<size_t>(y) * static_cast<size_t>(adata.stride));
                for (int x = 0; x < 500; ++x) {
                    low = low || row[x] == 0xFF440154u;
                    high = high || row[x] == 0xFFFDE725u;
                }
            }
            expect(low && high, "the legend shows both ends of the colour map");
        }
    }

    const int width = 600, height = 400;
    BLImage image(width, height, BL_FORMAT_PRGB32);
    {
        BLContext ctx(image);
        canvas.paint(ctx);
        ctx.end();
    }
    BLImageData data;
    image.get_data(&data);
    auto pixelAt = [&](Vec p) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(data.pixel_data) + static_cast<size_t>(p.y) * static_cast<size_t>(data.stride));
        return row[static_cast<size_t>(p.x)];
    };
    const Vec onNode = graph.toScreen({model.network().node(first).posX() + 10, model.network().node(first).posY() + 6});
    expect(pixelAt({3, 3}) == 0xFF1A1B1Eu, "the canvas paints its background");
    expect(pixelAt(onNode) != 0xFF1A1B1Eu, "the canvas paints the nodes");
    {
        model.newGraph();
        window.refresh();
        click(window, "addLine");
        click(window, "addSubnet");
        const NodeId sub = model.selected();
        const Vec c = graph.toScreen({model.network().node(sub).posX() + nodeWidth / 2, model.network().node(sub).posY() + nodeHeight / 2});
        canvas.onMouseDblClick.syncCall(canvas, newui::Point(c.x, c.y), newui::mbmLeftButton, 0u);
        expect(model.depth() == 1, "double-clicking a subnet enters it");
        click(window, "addSubnetInput");
        click(window, "addTransform");
        expect(model.network().nodeIds().size() == 2 && model.rootNetwork().nodeIds().size() == 2, "palette buttons add nodes inside the subnet");
        expect(model.lastCook().ok && model.lastCook().points == 100 && model.geometry(), "the viewport shows the subnet's output while inside");
        click(window, "upButton");
        expect(model.depth() == 0 && model.selected() == sub, "Up leaves the subnet and selects it");
        window.textField("spareName").setText(L"amount");
        click(window, "addSpareButton");
        bool promoted = false;
        for (const ParamInfo& p : model.parameters(sub)) promoted = promoted || p.name == "amount";
        expect(promoted, "Add parameter promotes a parameter on the selected subnet");
        expect(window.parameters().fieldOf("amount") != nullptr, "the parameter panel lists the promoted parameter");
    }
    {
        model.newGraph();
        window.refresh();
        expect(window.assetCount() >= 1, "the assets of the Assets directory are listed");
        click(window, "addLine");
        click(window, "addAssetButton");
        expect(model.network().nodeIds().size() == 2 && model.isAssetInstance(model.selected()), "Add asset adds an instance of the chosen asset");
        expect(model.lastCook().ok && model.lastCook().points == 100, "the asset instance cooks");
        click(window, "unlockAssetButton");
        expect(!model.isAssetInstance(model.selected()) && model.canEnter(model.selected()), "Unlock turns the instance into an editable subnet");
    }
    return failed == 0;
}

}  // namespace

int main(int argc, char** argv) {
    const bool checkOnly = argc > 1 && std::strcmp(argv[1], "--check") == 0;
    std::string startupFile, startupAttribute;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--attribute" && i + 1 < argc) startupAttribute = argv[++i];
        else if (arg != "--check" && startupFile.empty()) startupFile = arg;
    }
    registerReflectionData();

    newui::Frame frame;
    newui::Application& app = newui::Application::instance();
    app.setName("jitnodesEditor");
    app.setFrame(&frame);
    frame.setName("mainWindow");

    if (!newui::Bundle::instance().loadFrame(frame)) {
        std::fprintf(stderr, "could not load Resources/mainWindow.newui\n");
        return 1;
    }

    EditorModel model;
    const std::string assetError = model.loadAssetsFrom((std::filesystem::path(newui::Bundle::instance().executableDir()) / "Assets").string());
    if (!assetError.empty()) std::fprintf(stderr, "assets: %s\n", assetError.c_str());
    try {
        EditorWindow window(frame, model);
        std::printf("jitnodesEditor: %zu views wired\n", window.viewCount());
        if (checkOnly) return selfCheck(window, model) ? 0 : 1;
        if (!startupFile.empty()) window.openFile(startupFile);
        if (!startupAttribute.empty() && !window.selectAttribute(startupAttribute)) std::fprintf(stderr, "no attribute named '%s'\n", startupAttribute.c_str());
        app.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "jitnodesEditor: %s\n", e.what());
        return 1;
    }
    return 0;
}
