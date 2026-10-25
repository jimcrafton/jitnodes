#pragma once

#include <string>

#include "newui/newui.h"
#include "newui/application.h"
#include "newui/controls.h"
#include "newui/frame.h"
#include "newui/rootview.h"
#include "newui/runloop.h"
#include "newui/subview.h"
#include "newui/view.h"

#include <memory>

#include "editorModel.h"
#include "graphCanvas.h"
#include "graphController.h"
#include "parameterPanel.h"
#include "profileCanvas.h"
#include "profileController.h"
#include "viewportCanvas.h"
#include "viewportController.h"

namespace jitnodes::editor {

// Connects the views of mainWindow.newui (by name) to an EditorModel. Throws std::runtime_error naming the first
// view that is missing or has the wrong class.
class EditorWindow {
public:
    EditorWindow(newui::Frame& frame, EditorModel& model);
    ~EditorWindow();

    void refresh();
    size_t assetCount() const { return assetNames_.size(); }
    // Selects an entry of the asset list by name; false when there is none.
    bool selectAsset(const std::string& name);
    // Labels of the "Color by" list, starting with "(none)".
    const std::vector<std::string>& attributeLabels() const { return attributeLabels_; }
    // Opens a graph file without asking (used for the command line); errors are shown in the status line.
    void openFile(const std::string& path);
    // Chooses an entry of the Color by list by its label (for example "temp" or "N (length)"); false when there is none.
    bool selectAttribute(const std::string& label);
    size_t viewCount() const { return viewCount_; }
    // The viewport panel takes over the main area (the palette, the graph and the parameters are hidden), or goes back to its place
    // in the side column. Double-clicking the viewport does this.
    void toggleViewportMaximized();
    bool viewportMaximized() const { return viewportMaximized_; }

    newui::Button& button(const char* name) { return view<newui::Button>(name); }
    newui::TextField& textField(const char* name) { return view<newui::TextField>(name); }
    GraphCanvas& canvas() { return *canvas_; }
    GraphController& controller() { return *controller_; }
    ParameterPanel& parameters() { return *params_; }
    ViewportController& viewport() { return *viewport_; }
    ProfileController& profile() { return *profile_; }
    ProfileCanvas& profileCanvas() { return *profileCanvas_; }
    ViewportCanvas& viewportCanvas() { return *viewportCanvas_; }

private:
    template <class T>
    T& view(const char* name);
    template <class F>
    void onClick(const char* name, F action);
    void guarded(const std::function<void()>& action);
    void edited();
    void startPlayback();
    void editKeys(const std::string& name);
    void renameSelected();
    bool systemClipboardHasNodes();
    void afterNavigation();
    void refreshAssets();
    void updateAssetDetails();
    std::string selectedAsset() const;
    void refreshAttributes();
    void applyRange();
    void showViewportMenu();
    void doExportImage();
    void doLoadAsset();
    void doCreateAsset();
    bool confirmDiscard();
    void doNew();
    void doOpen();
    void doSave(bool askPath);
    void setText(const char* name, const std::string& text);

    newui::Frame& frame_;
    EditorModel& model_;
    newui::RootView& root_;
    std::unique_ptr<GraphController> controller_;
    GraphCanvas* canvas_ = nullptr;
    std::unique_ptr<ParameterPanel> params_;
    std::unique_ptr<ViewportController> viewport_;
    std::unique_ptr<ProfileController> profile_;
    ProfileCanvas* profileCanvas_ = nullptr;
    ViewportCanvas* viewportCanvas_ = nullptr;
    newui::SubView* palettePanel_ = nullptr;
    newui::SubView* graphPanel_ = nullptr;
    newui::SubView* sidePanel_ = nullptr;
    newui::SubView* paramsPanel_ = nullptr;
    bool viewportMaximized_ = false;
    size_t viewCount_ = 0;
    bool updatingUi_ = false;
    newui::RunLoop::TimerHandle timer_ = newui::RunLoop::kInvalidTimerHandle;
    std::string message_;
    std::vector<std::string> assetNames_;
    newui::TreeView* assetTree_ = nullptr;
    unsigned long clipboardSequence_ = 0;
    bool clipboardHasNodes_ = false;
    std::vector<std::string> attributeLabels_;
};

}  // namespace jitnodes::editor
