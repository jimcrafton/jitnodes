#include "editorWindow.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

#include "imageExport.h"
#include "keyframeDialog.h"
#include "newui/clipboardmgr.h"
#include "newui/menus.h"
#include "newui/dialogs.h"
#include "newui/utils.h"

namespace jitnodes::editor {

namespace {

struct PaletteEntry {
    const char* view;
    const char* type;
};

const std::vector<PaletteEntry> palette = {
    {"addLine", "line"}, {"addTransform", "transform"}, {"addWrangle", "wrangle"}, {"addMerge", "merge"},
    {"addAttribCreate", "attribCreate"}, {"addResample", "resample"}, {"addSubdivide", "subdivide"},
    {"addCopyToPoints", "copyToPoints"}, {"addBlast", "blast"}, {"addSubnet", "subnet"}, {"addSubnetInput", "subnetInput"}, {"addNull", "null"},
    {"addGenerate", "generate"}, {"addScatter", "scatter"}, {"addNoise", "noise"}, {"addWarp", "warp"}, {"addColorRamp", "colorRamp"},
    {"addRelax", "relax"}, {"addAttribBlur", "attribBlur"}, {"addWave", "wave"}, {"addAttractor", "attractor"}, {"addFlow", "flow"},
    {"addVoronoi", "voronoi"}, {"addLloyd", "lloyd"}, {"addParticles", "particles"}, {"addBoids", "boids"},
    {"addSprings", "springs"}, {"addReaction", "reactionDiffusion"},
    {"addLsystem", "lsystem"}, {"addCurveSample", "curveSample"}, {"addRender", "render"},
};

const std::vector<const char*> displayOnlyViews = {};

std::string format(const char* pattern, double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), pattern, value);
    return buf;
}

std::string baseName(const std::string& path) {
    return path.empty() ? std::string("untitled") : std::filesystem::path(path).filename().string();
}

newui::FileDialogOptions graphDialog(const std::string& title) {
    newui::FileDialogOptions options;
    options.title = title;
    options.defaultExtension = "json";
    options.filters = {{"jitnodes graphs", "*.json;*.json5"}, {"All files", "*.*"}};
    return options;
}

}  // namespace

template <class T>
T& EditorWindow::view(const char* name) {
    auto* v = dynamic_cast<T*>(root_.findView(name));
    if (!v) throw std::runtime_error(std::string("mainWindow.newui: no ") + typeid(T).name() + " named '" + name + "'");
    ++viewCount_;
    return *v;
}

template <class F>
void EditorWindow::onClick(const char* name, F action) {
    newui::Button& b = view<newui::Button>(name);
    b.onClick.add(std::function<newui::SyncReturn(newui::Control&)>([this, action](newui::Control&) {
        guarded([&] { action(); });
        return newui::SyncReturn::Handled;
    }));
}

EditorWindow::EditorWindow(newui::Frame& frame, EditorModel& model) : frame_(frame), model_(model), root_(frame.rootView()) {
    for (const char* name : displayOnlyViews) view<newui::View>(name);
    for (const char* name : {"graphStatus", "nodeTitle", "nodeType", "statusText", "cookInfo", "timeLabel", "profileSummary"}) view<newui::Label>(name);

    newui::SubView* host = dynamic_cast<newui::SubView*>(&view<newui::View>("graphCanvas"));
    if (!host) throw std::runtime_error("mainWindow.newui: graphCanvas must be a SubView");
    controller_ = std::make_unique<GraphController>(model_);
    controller_->onNavigated = [this] { afterNavigation(); };
    controller_->onChanged = [this](bool graphChanged) {
        if (graphChanged) guarded([this] { edited(); });
        else refresh();
    };
    host->setLayout(std::make_unique<newui::FlexLayout>(newui::Orientation::Vertical));
    canvas_ = new GraphCanvas(*controller_);
    canvas_->setBounds(newui::Rect(0, 0, host->bounds().width(), host->bounds().height()));
    canvas_->setLayoutParams(std::make_unique<newui::FlexLayoutParams>(1.0f));
    host->addChild(canvas_);
    newui::SubView* paramHost = dynamic_cast<newui::SubView*>(&view<newui::View>("paramList"));
    if (!paramHost) throw std::runtime_error("mainWindow.newui: paramList must be a SubView");
    params_ = std::make_unique<ParameterPanel>(*paramHost, model_, [this] { guarded([this] { edited(); }); },
                                               [this](const std::string& message) { message_ = message; },
                                               [this](const std::string& name) { guarded([&] { editKeys(name); }); });
    view<newui::Label>("paramError");
    view<newui::Toggle>("displayToggle").onCheckedChanged.add(std::function<newui::SyncReturn(newui::Toggle&)>([this](newui::Toggle& t) {
        const NodeId selected = model_.displayTarget();
        if (!updatingUi_ && selected != invalidNodeId && t.isChecked() && model_.network().displayNode() != selected) {
            model_.setDisplay(selected);
            guarded([this] { edited(); });
        }
        return newui::SyncReturn::Handled;
    }));
    for (const char* name : {"graphPath"}) view<newui::Label>(name);
    view<newui::TextField>("spareName");
    onClick("upButton", [this] { controller_->up(); });
    // Something copied in another program shows up in the Paste button when the canvas is entered or focused.
    auto refreshPaste = [this](newui::View&) {
        if (auto* b = dynamic_cast<newui::Button*>(root_.findView("pasteButton"))) b->setEnabled(model_.canPaste() || systemClipboardHasNodes());
        return newui::SyncReturn::Ignored;
    };
    canvas_->onGotFocus.add(std::function<newui::SyncReturn(newui::View&)>(refreshPaste));
    canvas_->onMouseDown.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>(
        [refreshPaste](newui::View& v, const newui::Point&, std::uint32_t, std::uint32_t) { return refreshPaste(v); }));
    controller_->writeClipboard = [](const std::string& text) { newui::ClipboardManager::setText(newui::utf8ToWide(text)); };
    controller_->readClipboard = [] {
        std::wstring text;
        return newui::ClipboardManager::getText(text) ? newui::wideToUtf8(text) : std::string();
    };
    onClick("copyButton", [this] { controller_->copySelected(); });
    onClick("cutButton", [this] { controller_->cutSelected(); });
    onClick("pasteButton", [this] { controller_->paste(); });
    newui::TextField& nameField = view<newui::TextField>("nodeName");
    nameField.onReturnPressed.add(std::function<newui::SyncReturn(newui::TextField&)>([this](newui::TextField&) {
        guarded([this] { renameSelected(); });
        return newui::SyncReturn::Handled;
    }));
    onClick("renameButton", [this] { renameSelected(); });
    canvas_->onRenameRequested = [this] {
        newui::TextField& field = view<newui::TextField>("nodeName");
        root_.setFocusedSubView(&field);
        field.controller().selectAll();
    };
    onClick("addSpareButton", [this] {
        newui::TextField& field = view<newui::TextField>("spareName");
        const NodeId id = model_.selected();
        if (id == invalidNodeId) {
            message_ = "select a subnet first";
            return;
        }
        message_ = model_.addSpareParameter(id, newui::wideToUtf8(field.text()));
        if (message_.empty()) field.setText(L"");
        edited();
    });
    newui::SubView* assetHost = dynamic_cast<newui::SubView*>(&view<newui::View>("assetListHost"));
    if (!assetHost) throw std::runtime_error("mainWindow.newui: assetListHost must be a SubView");
    assetHost->setLayout(std::make_unique<newui::FlexLayout>(newui::Orientation::Vertical));
    assetTree_ = new newui::TreeView();
    assetTree_->setName("assetList");
    assetTree_->setBounds(newui::Rect(0, 0, assetHost->bounds().width(), assetHost->bounds().height()));
    assetTree_->setLayoutParams(std::make_unique<newui::FlexLayoutParams>(1.0f));
    assetTree_->setModel(std::make_unique<newui::StringTreeModel>());
    assetTree_->onSelectionChanged.add(std::function<newui::SyncReturn(newui::TreeView&)>([this](newui::TreeView&) {
        if (!updatingUi_) updateAssetDetails();
        return newui::SyncReturn::Handled;
    }));
    assetHost->addChild(assetTree_);
    view<newui::Label>("assetDetails");
    onClick("addAssetButton", [this] {
        const std::string name = selectedAsset();
        if (name.empty()) return;
        model_.addNode(name);
        edited();
    });
    onClick("reloadAssetButton", [this] {
        const std::string name = selectedAsset();
        if (name.empty()) return;
        size_t updated = 0;
        message_ = model_.reloadAsset(name, &updated);
        if (message_.empty() && updated > 0) message_ = "reloaded " + name + ", " + std::to_string(updated) + " instance(s) rebuilt";
        edited();
    });
    onClick("loadAssetButton", [this] { doLoadAsset(); });
    onClick("createAssetButton", [this] { doCreateAsset(); });
    onClick("unlockAssetButton", [this] {
        message_ = model_.unlockAsset(model_.selected());
        edited();
    });
    onClick("fitGraphButton", [this] {
        canvas_->fit();
        canvas_->redraw();
    });

    newui::SubView* viewportHost = dynamic_cast<newui::SubView*>(&view<newui::View>("viewportCanvas"));
    if (!viewportHost) throw std::runtime_error("mainWindow.newui: viewportCanvas must be a SubView");
    viewport_ = std::make_unique<ViewportController>();
    viewportHost->setLayout(std::make_unique<newui::FlexLayout>(newui::Orientation::Vertical));
    viewportCanvas_ = new ViewportCanvas(*viewport_);
    viewportCanvas_->setBounds(newui::Rect(0, 0, viewportHost->bounds().width(), viewportHost->bounds().height()));
    viewportCanvas_->setLayoutParams(std::make_unique<newui::FlexLayoutParams>(1.0f));
    viewportHost->addChild(viewportCanvas_);
    palettePanel_ = &view<newui::SubView>("palettePanel");
    graphPanel_ = &view<newui::SubView>("graphPanel");
    sidePanel_ = &view<newui::SubView>("sidePanel");
    paramsPanel_ = &view<newui::SubView>("paramsPanel");
    view<newui::SubView>("viewportPanel");
    viewportCanvas_->onToggleMaximize = [this] { newui::Application::instance().runLoop().post([this] { toggleViewportMaximized(); }); };
    viewportCanvas_->onContextMenu = [this] { newui::Application::instance().runLoop().post([this] { showViewportMenu(); }); };
    view<newui::Label>("viewportInfo");
    view<newui::DropDownList>("attrSelect").onSelectionChanged.add(std::function<newui::SyncReturn(newui::DropDownList&)>([this](newui::DropDownList& d) {
        if (!updatingUi_ && d.selectedIndex()) {
            viewport_->selectChoice(*d.selectedIndex() == 0 ? std::nullopt : std::optional<size_t>(*d.selectedIndex() - 1));
            updatingUi_ = true;
            refreshAttributes();
            updatingUi_ = false;
            viewportCanvas_->redraw();
        }
        return newui::SyncReturn::Handled;
    }));

    {
        auto& maps = view<newui::DropDownList>("mapSelect");
        auto items = std::make_unique<newui::StringListModel>();
        for (size_t i = 0; i < ViewportController::colorMapCount; ++i) items->addItem(ViewportController::colorMapName(static_cast<ViewportController::ColorMap>(i)));
        maps.setModel(std::move(items));
        maps.setSelectedIndex(static_cast<size_t>(viewport_->colorMap()));
        maps.onSelectionChanged.add(std::function<newui::SyncReturn(newui::DropDownList&)>([this](newui::DropDownList& d) {
            if (!updatingUi_ && d.selectedIndex()) {
                viewport_->setColorMap(static_cast<ViewportController::ColorMap>(*d.selectedIndex()));
                viewportCanvas_->redraw();
            }
            return newui::SyncReturn::Handled;
        }));
        for (const char* name : {"rangeMin", "rangeMax"}) {
            newui::TextField& field = view<newui::TextField>(name);
            field.setPlaceholder("auto");
            field.onReturnPressed.add(std::function<newui::SyncReturn(newui::TextField&)>([this](newui::TextField&) {
                guarded([this] { applyRange(); });
                return newui::SyncReturn::Handled;
            }));
            field.onLostFocus.add(std::function<newui::SyncReturn(newui::View&)>([this](newui::View&) {
                guarded([this] { applyRange(); });
                return newui::SyncReturn::Handled;
            }));
        }
        onClick("autoRangeButton", [this] {
            viewport_->setRange(std::nullopt, std::nullopt);
            viewportCanvas_->redraw();
            refreshAttributes();
        });
    }

    newui::SubView* profileHost = dynamic_cast<newui::SubView*>(&view<newui::View>("profileRows"));
    if (!profileHost) throw std::runtime_error("mainWindow.newui: profileRows must be a SubView");
    profile_ = std::make_unique<ProfileController>();
    profileHost->setLayout(std::make_unique<newui::FlexLayout>(newui::Orientation::Vertical));
    profileCanvas_ = new ProfileCanvas(*profile_);
    profileCanvas_->setBounds(newui::Rect(0, 0, profileHost->bounds().width(), profileHost->bounds().height()));
    profileCanvas_->setLayoutParams(std::make_unique<newui::FlexLayoutParams>(1.0f));
    profileHost->addChild(profileCanvas_);
    profileCanvas_->onRowClicked = [this](NodeId id) {
        model_.select(id);
        refresh();
    };
    onClick("resetProfileButton", [this] {
        model_.resetProfile();
        refresh();
    });
    onClick("fitButton", [this] {
        viewport_->fit();
        viewportCanvas_->redraw();
    });

    onClick("newButton", [this] { doNew(); });
    onClick("openButton", [this] { doOpen(); });
    onClick("saveButton", [this] { doSave(false); });
    onClick("undoButton", [this] {
        model_.undo();
        edited();
    });
    onClick("redoButton", [this] {
        model_.redo();
        edited();
    });
    for (const PaletteEntry& entry : palette) {
        onClick(entry.view, [this, type = std::string(entry.type)] {
            model_.addNode(type);
            edited();
        });
    }

    view<newui::Button>("playButton").onClick.add(std::function<newui::SyncReturn(newui::Control&)>([this](newui::Control&) {
        model_.setPlaying(!model_.playing());
        if (model_.playing()) startPlayback();
        refresh();
        return newui::SyncReturn::Handled;
    }));

    view<newui::Slider>("timeSlider").onValueChanged.add(std::function<newui::SyncReturn(newui::Slider&)>([this](newui::Slider& s) {
        if (!updatingUi_) {
            model_.setPlayheadFraction(s.value() / 100.0);
            guarded([this] { edited(); });
        }
        return newui::SyncReturn::Handled;
    }));

    view<newui::Toggle>("fusionToggle").onCheckedChanged.add(std::function<newui::SyncReturn(newui::Toggle&)>([this](newui::Toggle& t) {
        model_.rootNetwork().setFusionEnabled(t.isChecked());
        return newui::SyncReturn::Handled;
    }));
    view<newui::Toggle>("fusionToggle").setChecked(model_.rootNetwork().fusionEnabled());

    newui::DropDownList& threads = view<newui::DropDownList>("threadsSelect");
    auto threadItems = std::make_unique<newui::StringListModel>();
    const std::vector<size_t> threadCounts = {1, 2, 4, 8};
    std::optional<size_t> currentThreads;
    for (size_t i = 0; i < threadCounts.size(); ++i) {
        threadItems->addItem(std::to_string(threadCounts[i]));
        if (threadCounts[i] == model_.rootNetwork().threadCount()) currentThreads = i;
    }
    threads.setModel(std::move(threadItems));
    threads.setSelectedIndex(currentThreads);
    threads.onSelectionChanged.add(std::function<newui::SyncReturn(newui::DropDownList&)>([this, threadCounts](newui::DropDownList& d) {
        if (d.selectedIndex() && *d.selectedIndex() < threadCounts.size()) model_.rootNetwork().setThreadCount(threadCounts[*d.selectedIndex()]);
        return newui::SyncReturn::Handled;
    }));

    newui::DropDownList& isa = view<newui::DropDownList>("isaSelect");
    auto isaItems = std::make_unique<newui::StringListModel>();
    const std::vector<Isa> isas = model_.network().compiler().supportedIsas();
    std::optional<size_t> currentIsa;
    for (size_t i = 0; i < isas.size(); ++i) {
        isaItems->addItem(isaName(isas[i]));
        if (isas[i] == model_.network().compiler().isa()) currentIsa = i;
    }
    isa.setModel(std::move(isaItems));
    isa.setSelectedIndex(currentIsa);
    isa.onSelectionChanged.add(std::function<newui::SyncReturn(newui::DropDownList&)>([this, isas](newui::DropDownList& d) {
        if (d.selectedIndex() && *d.selectedIndex() < isas.size()) model_.network().compiler().setIsa(isas[*d.selectedIndex()]);
        return newui::SyncReturn::Handled;
    }));

    refresh();
}

EditorWindow::~EditorWindow() {
    if (timer_ != newui::RunLoop::kInvalidTimerHandle) newui::Application::instance().runLoop().cancelDelayed(timer_);
}

void EditorWindow::setText(const char* name, const std::string& text) {
    auto* label = dynamic_cast<newui::Label*>(root_.findView(name));
    if (label && label->text() != text) label->setText(text);
}

void EditorWindow::guarded(const std::function<void()>& action) {
    try {
        message_.clear();
        action();
    } catch (const std::exception& e) {
        message_ = e.what();
        refresh();
    }
}

void EditorWindow::edited() {
    model_.cook();
    refresh();
}

void EditorWindow::refresh() {
    updatingUi_ = true;
    dynamic_cast<newui::Button*>(root_.findView("undoButton"))->setEnabled(model_.canUndo());
    dynamic_cast<newui::Button*>(root_.findView("redoButton"))->setEnabled(model_.canRedo());
    dynamic_cast<newui::Button*>(root_.findView("playButton"))->setText(model_.playing() ? "Pause" : "Play");

    auto* slider = dynamic_cast<newui::Slider*>(root_.findView("timeSlider"));
    const float wanted = static_cast<float>(model_.playheadFraction() * 100.0);
    if (slider->value() != wanted) slider->setValue(wanted);
    setText("timeLabel", format("%.2f s", model_.time()));

    refreshAssets();
    setText("graphStatus", model_.statusLine());
    setText("graphPath", model_.pathText());
    if (auto* upButton = dynamic_cast<newui::Button*>(root_.findView("upButton"))) upButton->setEnabled(model_.depth() > 0);
    {
        const NodeId sel = model_.selected();
        if (auto* b = dynamic_cast<newui::Button*>(root_.findView("createAssetButton"))) b->setEnabled(sel != invalidNodeId && model_.canEnter(sel));
        if (auto* b = dynamic_cast<newui::Button*>(root_.findView("unlockAssetButton"))) b->setEnabled(sel != invalidNodeId && model_.isAssetInstance(sel));
    }
    if (auto* spare = dynamic_cast<newui::Button*>(root_.findView("addSpareButton"))) {
        const NodeId sel = model_.selected();
        spare->setEnabled(sel != invalidNodeId && model_.canEnter(sel));
    }
    const NodeId selected = model_.selected();
    const size_t selectedCount = model_.selection().size();
    if (selectedCount > 1) {
        setText("nodeTitle", std::to_string(selectedCount) + " nodes selected");
        setText("nodeType", "");
    } else if (selected != invalidNodeId) {
        const Node& n = model_.network().node(selected);
        setText("nodeTitle", n.name());
        if (auto* field = dynamic_cast<newui::TextField*>(root_.findView("nodeName")); field && !field->isFocused() && field->text() != newui::utf8ToWide(n.name()))
            field->setText(newui::utf8ToWide(n.name()));
        setText("nodeType", n.typeName() + ", cooked " + std::to_string(n.cookCount()) + "x");
    } else {
        setText("nodeTitle", "no selection");
        if (auto* field = dynamic_cast<newui::TextField*>(root_.findView("nodeName")); field && !field->isFocused() && !field->text().empty()) field->setText(L"");
        setText("nodeType", "");
    }

    params_->sync();
    auto* display = dynamic_cast<newui::Toggle*>(root_.findView("displayToggle"));
    const NodeId target = model_.displayTarget();
    display->setEnabled(target != invalidNodeId);
    display->setChecked(target != invalidNodeId && model_.network().displayNode() == target);
    if (controller_ && message_.empty()) message_ = controller_->message();
    if (auto* b = dynamic_cast<newui::Button*>(root_.findView("copyButton"))) b->setEnabled(selectedCount > 0);
    if (auto* b = dynamic_cast<newui::Button*>(root_.findView("cutButton"))) b->setEnabled(selectedCount > 0);
    if (auto* b = dynamic_cast<newui::Button*>(root_.findView("pasteButton"))) b->setEnabled(model_.canPaste() || systemClipboardHasNodes());
    if (auto* b = dynamic_cast<newui::Button*>(root_.findView("renameButton"))) b->setEnabled(selectedCount > 0);
    setText("paramError", message_.empty() ? std::string("\u00b7") : message_);

    const CookResult& cook = model_.lastCook();
    setText("cookInfo", cook.ok ? "last cook " + format("%.2f", cook.ms) + " ms, " + std::to_string(cook.points) + " points" : "cook failed");
    setText("statusText", !message_.empty() ? message_ : (!cook.ok ? cook.error : "Ready"));
    setText("profileSummary", "kernels compiled: " + std::to_string(model_.network().compiler().compileCount()) + ", threads: " +
                                  std::to_string(model_.network().threadCount()));
    if (canvas_) canvas_->redraw();
    if (profile_) {
        profile_->setRows(model_.profile());
        profile_->setSelected(model_.depth() == 0 ? model_.selected() : invalidNodeId);  // ids repeat between networks
        profileCanvas_->redraw();
    }
    if (viewport_) {
        viewport_->setGeometry(model_.geometry());
        refreshAttributes();
        setText("viewportInfo", viewport_->info());
        viewportCanvas_->redraw();
    }
    frame_.setTitle("jitnodes Editor - " + baseName(model_.path()) + (model_.modified() ? "*" : ""));
    updatingUi_ = false;
}

bool EditorWindow::systemClipboardHasNodes() {
    // Reading the clipboard can fail while another program holds it, so look again only when its content changed.
    const DWORD sequence = ::GetClipboardSequenceNumber();
    if (sequence != clipboardSequence_) {
        clipboardSequence_ = sequence;
        std::wstring text;
        clipboardHasNodes_ = false;
        if (newui::ClipboardManager::getText(text)) {
            try {
                clipboardHasNodes_ = !clipsFromText(newui::wideToUtf8(text)).empty();
            } catch (const std::runtime_error&) {
            }
        }
    }
    return clipboardHasNodes_;
}

void EditorWindow::renameSelected() {
    message_ = model_.renameSelection(newui::wideToUtf8(view<newui::TextField>("nodeName").text()));
    edited();
}

void EditorWindow::editKeys(const std::string& name) {
    const NodeId id = model_.selected();
    if (id == invalidNodeId) return;
    KeyframeDialog dialog(model_.network().node(id).name() + "." + name, model_.keyframes(id, name), model_.time());
    std::vector<Keyframe> keys;
    if (!dialog.run(frame_, keys)) return;
    message_ = model_.setKeyframes(id, name, std::move(keys));
    edited();
}

bool EditorWindow::selectAttribute(const std::string& label) {
    const auto& choices = viewport_->choices();
    for (size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].label != label) continue;
        viewport_->selectChoice(i);
        updatingUi_ = true;
        refreshAttributes();
        updatingUi_ = false;
        viewportCanvas_->redraw();
        return true;
    }
    return false;
}

namespace {

// Empty text is "auto" (nullopt); false when the text is neither empty nor a finite number.
bool parseRangeEnd(const std::wstring& text, std::optional<float>& out) {
    out.reset();
    const std::string s = newui::wideToUtf8(text);
    if (s.find_first_not_of(" \t") == std::string::npos) return true;
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    while (end && (*end == ' ' || *end == '\t')) ++end;
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

std::wstring rangeEndText(std::optional<float> v) {
    if (!v) return {};
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(*v));
    return newui::utf8ToWide(buf);
}

}  // namespace

void EditorWindow::applyRange() {
    if (updatingUi_ || !viewport_->hasValues()) return;
    std::optional<float> low, high;
    if (!parseRangeEnd(view<newui::TextField>("rangeMin").text(), low) || !parseRangeEnd(view<newui::TextField>("rangeMax").text(), high)) {
        message_ = "the range needs numbers (or nothing for auto)";
        view<newui::TextField>("rangeMin").setText(rangeEndText(viewport_->manualLow()));
        view<newui::TextField>("rangeMax").setText(rangeEndText(viewport_->manualHigh()));
        setText("statusText", message_);
        return;
    }
    if (low == viewport_->manualLow() && high == viewport_->manualHigh()) return;
    viewport_->setRange(low, high);
    viewportCanvas_->redraw();
}

void EditorWindow::refreshAttributes() {
    std::vector<std::string> labels{"(none)"};
    for (const auto& c : viewport_->choices()) labels.push_back(c.label);
    auto* select = dynamic_cast<newui::DropDownList*>(root_.findView("attrSelect"));
    if (!select) return;
    const std::optional<size_t> chosen = viewport_->selectedChoice();
    const size_t index = chosen ? *chosen + 1 : 0;
    if (labels != attributeLabels_) {
        attributeLabels_ = labels;
        auto items = std::make_unique<newui::StringListModel>();
        for (const std::string& l : labels) items->addItem(l);
        select->setModel(std::move(items));
    }
    if (select->selectedIndex() != std::optional<size_t>(index)) select->setSelectedIndex(index);
    for (auto [name, value] : {std::pair{"rangeMin", viewport_->manualLow()}, std::pair{"rangeMax", viewport_->manualHigh()}}) {
        auto* field = dynamic_cast<newui::TextField*>(root_.findView(name));
        if (field && !field->isFocused() && field->text() != rangeEndText(value)) field->setText(rangeEndText(value));
        if (field) field->setEnabled(viewport_->hasValues());
    }
    if (auto* b = dynamic_cast<newui::Button*>(root_.findView("autoRangeButton"))) b->setEnabled(viewport_->hasValues());
}

bool EditorWindow::selectAsset(const std::string& name) {
    const auto at = std::find(assetNames_.begin(), assetNames_.end(), name);
    if (at == assetNames_.end()) return false;
    assetTree_->setSelectedPath(std::vector<size_t>{static_cast<size_t>(at - assetNames_.begin())});
    updateAssetDetails();
    return true;
}

std::string EditorWindow::selectedAsset() const {
    const auto path = assetTree_->selectedPath();
    return path && path->size() == 1 && path->front() < assetNames_.size() ? assetNames_[path->front()] : std::string();
}

void EditorWindow::refreshAssets() {
    const std::vector<std::string> names = model_.assetNames();
    if (names != assetNames_) {
        const std::string keep = selectedAsset();
        assetNames_ = names;
        auto items = std::make_unique<newui::StringTreeModel>();
        std::optional<size_t> index;
        for (size_t i = 0; i < names.size(); ++i) {
            items->addItem(names[i]);
            if (names[i] == keep) index = i;
        }
        assetTree_->setModel(std::move(items));
        if (!index && !names.empty()) index = 0;
        assetTree_->setSelectedPath(index ? std::optional<std::vector<size_t>>(std::vector<size_t>{*index}) : std::nullopt);
    }
    updateAssetDetails();
}

void EditorWindow::updateAssetDetails() {
    const std::string name = selectedAsset();
    const EditorModel::AssetInfo info = name.empty() ? EditorModel::AssetInfo() : model_.assetInfo(name);
    std::string text = "no asset selected";
    if (!info.name.empty()) {
        text = info.name + " v" + std::to_string(info.version) + ": " + std::to_string(info.inputs) + (info.inputs == 1 ? " input, " : " inputs, ");
        text += std::to_string(info.parameters.size()) + (info.parameters.size() == 1 ? " parameter" : " parameters");
        for (size_t i = 0; i < info.parameters.size(); ++i) text += (i ? ", " : " (") + info.parameters[i] + (i + 1 == info.parameters.size() ? ")" : "");
        text += "; " + std::to_string(info.instances) + " in the graph";
        if (!info.source.empty()) text += "; " + std::filesystem::path(info.source).filename().string();
    }
    setText("assetDetails", text);
    if (auto* b = dynamic_cast<newui::Button*>(root_.findView("addAssetButton"))) b->setEnabled(!info.name.empty());
    if (auto* b = dynamic_cast<newui::Button*>(root_.findView("reloadAssetButton"))) b->setEnabled(!info.source.empty());
}

void EditorWindow::doLoadAsset() {
    newui::FileDialogOptions options;
    options.title = "Load asset";
    options.filters = {{"asset files", "*.asset.json;*.json"}, {"All files", "*.*"}};
    std::string path;
    if (!newui::Dialog::showOpenFile(frame_.frameHandle(), options, path)) return;
    message_ = model_.loadAssetFile(path);
    edited();
}

void EditorWindow::doCreateAsset() {
    const NodeId id = model_.selected();
    if (id == invalidNodeId || !model_.canEnter(id)) {
        message_ = "select a subnet first";
        refresh();
        return;
    }
    newui::FileDialogOptions options;
    options.title = "Create asset from " + model_.network().node(id).name();
    options.defaultFileName = model_.network().node(id).name() + ".asset.json";
    options.defaultExtension = "asset.json";
    options.filters = {{"asset files", "*.asset.json"}};
    std::string path;
    if (!newui::Dialog::showSaveFile(frame_.frameHandle(), options, path)) return;
    std::string type = std::filesystem::path(path).filename().string();
    type = type.substr(0, type.find('.'));
    size_t updated = 0;
    message_ = model_.createAsset(id, type, path, true, &updated);
    if (message_.empty() && updated > 0) message_ = std::to_string(updated) + " existing instance(s) updated; undo history was reset";
    edited();
}

void EditorWindow::toggleViewportMaximized() {
    viewportMaximized_ = !viewportMaximized_;
    for (newui::SubView* panel : {palettePanel_, graphPanel_, paramsPanel_}) panel->setVisible(!viewportMaximized_);
    // The side column has a fixed width in the thumbnail state; with the other panels hidden it takes all the room.
    sidePanel_->setLayoutParams(std::make_unique<newui::FlexLayoutParams>(viewportMaximized_ ? 1.0f : 0.0f));
    if (newui::View* row = sidePanel_->parent()) row->updateLayout();
    viewport_->resetFit();
    viewportCanvas_->redraw();
}

void EditorWindow::showViewportMenu() {
    newui::MenuItem menu;
    bool exportChosen = false;
    menu.addChild(std::make_unique<newui::MenuItem>("Export..."))->onClick.add([&exportChosen](newui::MenuItem&) {
        exportChosen = true;
        return newui::SyncReturn::Handled;
    });
    POINT at{};
    ::GetCursorPos(&at);
    newui::ContextMenu popup;
    popup.show(frame_.frameHandle(), menu, at.x, at.y);
    if (exportChosen) guarded([this] { doExportImage(); });
}

void EditorWindow::doExportImage() {
    if (!viewport_->geometry()) {
        message_ = "there is nothing to export";
        refresh();
        return;
    }
    newui::FileDialogOptions options;
    options.title = "Export image";
    options.defaultFileName = "viewport.png";
    options.defaultExtension = "png";
    options.filters = {{"PNG image", "*.png"}, {"JPEG image", "*.jpg;*.jpeg"}};
    std::string path;
    if (!newui::Dialog::showSaveFile(frame_.frameHandle(), options, path)) return;
    if (std::filesystem::path(path).extension().empty()) path += ".png";
    const Image image = viewportCanvas_->renderForExport();
    message_ = exportImage(image, path);
    if (message_.empty()) message_ = "exported " + baseName(path) + " (" + std::to_string(image.width) + " x " + std::to_string(image.height) + ")";
    refresh();
}

void EditorWindow::afterNavigation() {
    canvas_->fit();
    viewport_->resetFit();
    guarded([this] { edited(); });
}

void EditorWindow::startPlayback() {
    if (timer_ != newui::RunLoop::kInvalidTimerHandle) return;
    using namespace std::chrono_literals;
    timer_ = newui::Application::instance().runLoop().postDelayed(33ms, [this] {
        if (!model_.playing()) {
            timer_ = newui::RunLoop::kInvalidTimerHandle;
            return true;
        }
        model_.advance(0.033);
        guarded([this] { edited(); });
        return false;
    });
}

bool EditorWindow::confirmDiscard() {
    if (!model_.modified()) return true;
    return newui::Dialog::showMessageBox(frame_.frameHandle(), "Discard the unsaved changes?", "jitnodes Editor", newui::MessageBoxButtons::YesNo,
                                         newui::MessageBoxIcon::Question) == newui::DialogResult::Yes;
}

void EditorWindow::doNew() {
    if (!confirmDiscard()) return;
    model_.newGraph();
    viewport_->resetFit();
    edited();
}

void EditorWindow::doOpen() {
    if (!confirmDiscard()) return;
    std::string path;
    if (!newui::Dialog::showOpenFile(frame_.frameHandle(), graphDialog("Open graph"), path)) return;
    model_.open(path);
    viewport_->resetFit();
    canvas_->fit();
    edited();
}

void EditorWindow::openFile(const std::string& path) {
    guarded([&] {
        model_.open(path);
        viewport_->resetFit();
        canvas_->fit();
        edited();
    });
}

void EditorWindow::doSave(bool askPath) {
    std::string path = model_.path();
    if (askPath || path.empty()) {
        newui::FileDialogOptions options = graphDialog("Save graph");
        options.defaultFileName = baseName(path);
        if (!newui::Dialog::showSaveFile(frame_.frameHandle(), options, path)) return;
    }
    model_.save(path);
    refresh();
}

}  // namespace jitnodes::editor
