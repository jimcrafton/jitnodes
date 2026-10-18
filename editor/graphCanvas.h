#pragma once

#include <functional>

#include "newui/newui.h"
#include "newui/rootview.h"
#include "newui/runloop.h"
#include "newui/subview.h"
#include "newui/view.h"

#include "graphController.h"

namespace jitnodes::editor {

// Draws the node graph with Blend2D and forwards mouse and key input to a GraphController. Heap-only like every newui
// view; the parent view takes ownership through addChild.
class GraphCanvas : public newui::SubView {
public:
    explicit GraphCanvas(GraphController& controller);

    void paint(BLContext& ctx) override;

    // Called when F2 is pressed over the canvas.
    std::function<void()> onRenameRequested;

    void fit() { controller_.frameAll(bounds().width(), bounds().height()); }

private:
    GraphController& controller_;
};

}  // namespace jitnodes::editor
