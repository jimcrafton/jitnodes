#pragma once

#include "newui/newui.h"
#include "newui/rootview.h"
#include "newui/runloop.h"
#include "newui/subview.h"
#include "newui/view.h"

#include "viewportController.h"

namespace jitnodes::editor {

// Draws the cooked geometry of the display node: grid, axes, primitives as polylines and, when there are few enough, the
// points. Heap-only like every newui view; the parent takes ownership through addChild.
class ViewportCanvas : public newui::SubView {
public:
    explicit ViewportCanvas(ViewportController& controller);

    void paint(BLContext& ctx) override;

private:
    void drawSprites(BLContext& ctx);

    ViewportController& controller_;
};

}  // namespace jitnodes::editor
