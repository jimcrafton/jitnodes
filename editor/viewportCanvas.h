#pragma once

#include <functional>

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

    // Double click: the window maximizes the viewport or restores it. Right click without dragging (a right drag still pans): the
    // window shows the context menu.
    std::function<void()> onToggleMaximize;
    std::function<void()> onContextMenu;

    // The picture to export: for geometry that went through a render node, the render node's own size and view window (what
    // `jitnodesCli --png` writes); otherwise what the canvas shows right now.
    Image renderForExport();

private:
    void drawSprites(BLContext& ctx);
    void drawRaster(BLContext& ctx, float width, float height);

    ViewportController& controller_;
    ThreadPool pool_;
    // The last rendered image is reused while the geometry and the view stay the same (hover and repaints do not change them).
    Image image_;
    GeometryHandle imageGeometry_;
    float imageZoom_ = 0.0f;
    Vec imagePan_{0.0f, 0.0f};
    bool rightDown_ = false;
    Vec rightStart_{0.0f, 0.0f};
};

}  // namespace jitnodes::editor
