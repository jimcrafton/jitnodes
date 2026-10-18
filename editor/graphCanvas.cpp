#include "graphCanvas.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "drawText.h"
#include "newui/keyboard_constants.h"
#include "newui/mouse_constants.h"

namespace jitnodes::editor {

namespace {

Vec vec(const newui::Point& p) { return {p.x, p.y}; }

unsigned buttonsOf(std::uint32_t mask) {
    return ((mask & newui::mbmLeftButton) ? leftButton : 0) | ((mask & newui::mbmMiddleButton) ? middleButton : 0) |
           ((mask & newui::mbmRightButton) ? rightButton : 0);
}

unsigned modifiersOf(std::uint32_t keys) {
    return ((keys & newui::kmShift) ? shiftKey : 0) | ((keys & newui::kmCtrl) ? ctrlKey : 0);
}

BLRgba32 nodeColor(const Node& n) {
    if (n.childNetwork()) return BLRgba32(0xFF7B4FA8);
    if (n.isMerge()) return BLRgba32(0xFFB5762E);
    if (n.isPointwise()) return BLRgba32(0xFF3B5BA8);
    if (n.inputCount() == 0) return BLRgba32(0xFF3F8F5F);
    return BLRgba32(0xFF4A5566);
}

void wirePath(BLPath& path, Vec from, Vec to, float zoom) {
    const double reach = std::max(30.0 * zoom, std::fabs(to.y - from.y) * 0.5);
    path.move_to(from.x, from.y);
    path.cubic_to(from.x, from.y + reach, to.x, to.y - reach, to.x, to.y);
}

}  // namespace

GraphCanvas::GraphCanvas(GraphController& controller) : controller_(controller) {
    setVisible(true);
    setAcceptsFocus(true);

    onMouseDown.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>(
        [this](newui::View&, const newui::Point& p, std::uint32_t buttons, std::uint32_t keys) {
            if (newui::RootView* root = rootView()) root->setFocusedSubView(this);
            if (controller_.mouseDown(vec(p), buttonsOf(buttons), modifiersOf(keys))) redraw();
            return newui::SyncReturn::Handled;
        }));
    onMouseMove.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>(
        [this](newui::View&, const newui::Point& p, std::uint32_t, std::uint32_t) {
            if (controller_.mouseMove(vec(p))) redraw();
            return newui::SyncReturn::Handled;
        }));
    onMouseUp.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>(
        [this](newui::View&, const newui::Point& p, std::uint32_t, std::uint32_t) {
            if (controller_.mouseUp(vec(p))) redraw();
            return newui::SyncReturn::Handled;
        }));
    onMouseDblClick.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>(
        [this](newui::View&, const newui::Point& p, std::uint32_t, std::uint32_t) {
            if (controller_.doubleClick(vec(p))) redraw();
            return newui::SyncReturn::Handled;
        }));
    onMouseWheel.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, float)>(
        [this](newui::View&, const newui::Point& p, float delta) {
            if (controller_.wheel(vec(p), delta)) redraw();
            return newui::SyncReturn::Handled;
        }));
    onKeyDown.add(std::function<newui::SyncReturn(newui::View&, std::uint32_t, int, int, std::uint32_t)>(
        [this](newui::View&, std::uint32_t mask, int, int, std::uint32_t key) {
            if (mask & newui::kmCtrl) {
                const bool handled = key == newui::vkLetterC ? controller_.copySelected()
                                     : key == newui::vkLetterX ? controller_.cutSelected()
                                     : key == newui::vkLetterV ? controller_.paste()
                                     : key == newui::vkLetterA ? controller_.selectAll()
                                                               : false;
                if (handled) {
                    redraw();
                    return newui::SyncReturn::Handled;
                }
            }
            if (key == newui::vkDelete && controller_.deleteSelected()) {
                redraw();
                return newui::SyncReturn::Handled;
            }
            if ((key == newui::vkReturn && controller_.enterSelected()) || (key == newui::vkBackSpace && controller_.up())) {
                redraw();
                return newui::SyncReturn::Handled;
            }
            return newui::SyncReturn::Ignored;
        }));
}

void GraphCanvas::paint(BLContext& ctx) {
    const Network& net = controller_.model().network();
    const float w = bounds().width(), h = bounds().height();
    const float zoom = controller_.zoom();

    ctx.save();
    ctx.set_fill_style(BLRgba32(0xFF1A1B1E));
    ctx.fill_rect(BLRect(0, 0, w, h));

    const float step = 40.0f * zoom;
    if (step >= 8.0f) {
        ctx.set_fill_style(BLRgba32(0xFF24262A));
        const Vec pan = controller_.pan();
        for (float x = std::fmod(pan.x, step); x < w; x += step) ctx.fill_rect(BLRect(x, 0, 1, h));
        for (float y = std::fmod(pan.y, step); y < h; y += step) ctx.fill_rect(BLRect(0, y, w, 1));
    }

    ctx.set_stroke_width(std::max(1.5f, 2.0f * zoom));
    ctx.set_stroke_style(BLRgba32(0xFF8A93A6));
    for (NodeId id : net.nodeIds()) {
        const Node& n = net.node(id);
        for (int i = 0; i < n.inputCount(); ++i) {
            const NodeId up = net.inputOf(id, i);
            if (up == invalidNodeId) continue;
            BLPath path;
            wirePath(path, controller_.toScreen(controller_.outputPort(up)), controller_.toScreen(controller_.inputPort(id, i)), zoom);
            ctx.stroke_path(path);
        }
    }
    if (const auto& drag = controller_.wireDrag()) {
        ctx.set_stroke_style(BLRgba32(0xFF5A7CFF));
        BLPath path;
        const Vec to = controller_.toScreen(drag->to);
        wirePath(path, controller_.toScreen(controller_.outputPort(drag->from)), to, zoom);
        ctx.stroke_path(path);
    }

    const NodeId display = net.displayNode();
    for (NodeId id : net.nodeIds()) {
        const Node& n = net.node(id);
        const Vec p = controller_.toScreen({n.posX(), n.posY()});
        const float nw = nodeWidth * zoom, nh = nodeHeight * zoom, r = 6.0f * zoom;
        ctx.set_fill_style(nodeColor(n));
        ctx.fill_round_rect(BLRoundRect(p.x, p.y, nw, nh, r, r));
        const bool isSelected = controller_.model().isSelected(id);
        ctx.set_stroke_width(isSelected ? 2.5f : 1.0f);
        ctx.set_stroke_style(isSelected ? BLRgba32(0xFFFFFFFF) : (id == controller_.hovered() ? BLRgba32(0xFFB8C4E0) : BLRgba32(0xFF1A1B1E)));
        ctx.stroke_round_rect(BLRoundRect(p.x, p.y, nw, nh, r, r));
        if (id == display) {
            ctx.set_fill_style(BLRgba32(0xFFFFD34E));
            ctx.fill_round_rect(BLRoundRect(p.x + nw - 14.0f * zoom, p.y + 5.0f * zoom, 8.0f * zoom, 8.0f * zoom, 2.0f * zoom, 2.0f * zoom));
        }
        drawText(ctx, n.name(), 12.0f * zoom, p.x + 8.0f * zoom, p.y + 15.0f * zoom, BLRgba32(0xFFFFFFFF));
        drawText(ctx, n.typeName(), 10.0f * zoom, p.x + 8.0f * zoom, p.y + 29.0f * zoom, BLRgba32(0xFFD0D6E4));

        ctx.set_fill_style(BLRgba32(0xFFE6E6E6));
        const float pr = 3.5f * zoom;
        for (int i = 0; i < n.inputCount(); ++i) {
            const Vec in = controller_.toScreen(controller_.inputPort(id, i));
            ctx.fill_round_rect(BLRoundRect(in.x - pr, in.y - pr, 2 * pr, 2 * pr, pr, pr));
        }
        const Vec out = controller_.toScreen(controller_.outputPort(id));
        ctx.fill_round_rect(BLRoundRect(out.x - pr, out.y - pr, 2 * pr, 2 * pr, pr, pr));
    }
    if (const auto& box = controller_.marquee()) {
        const double x = std::min(box->from.x, box->to.x), y = std::min(box->from.y, box->to.y);
        const double bw = std::fabs(box->to.x - box->from.x), bh = std::fabs(box->to.y - box->from.y);
        ctx.set_fill_style(BLRgba32(0x305A7CFF));
        ctx.fill_rect(BLRect(x, y, bw, bh));
        ctx.set_stroke_width(1.0f);
        ctx.set_stroke_style(BLRgba32(0xFF5A7CFF));
        ctx.stroke_rect(BLRect(x, y, bw, bh));
    }
    ctx.restore();
}

}  // namespace jitnodes::editor
