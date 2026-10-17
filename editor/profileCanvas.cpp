#include "profileCanvas.h"

#include <cstdio>

#include "drawText.h"
#include "newui/mouse_constants.h"

namespace jitnodes::editor {

namespace {

// Offsets of the five columns, equal to the header labels of the mockup (widths 160, 100, 60, 80, then the rest, 8 px apart).
constexpr float columnX[] = {0.0f, 168.0f, 276.0f, 344.0f, 432.0f};

std::string millis(double ms) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", ms);
    return buf;
}

}  // namespace

ProfileCanvas::ProfileCanvas(ProfileController& controller) : controller_(controller) {
    setVisible(true);
    onMouseDown.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>(
        [this](newui::View&, const newui::Point& p, std::uint32_t, std::uint32_t) {
            if (const auto row = controller_.rowAt(p.y)) {
                const NodeId node = controller_.selectableNode(*row);
                if (node != invalidNodeId && onRowClicked) onRowClicked(node);
            }
            return newui::SyncReturn::Handled;
        }));
    onMouseWheel.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, float)>(
        [this](newui::View&, const newui::Point&, float delta) {
            if (controller_.wheel(delta)) redraw();
            return newui::SyncReturn::Handled;
        }));
}

void ProfileCanvas::paint(BLContext& ctx) {
    const float w = bounds().width(), h = bounds().height();
    controller_.setViewHeight(h);
    ctx.save();
    ctx.set_fill_style(BLRgba32(0xFF25272B));
    ctx.fill_rect(BLRect(0, 0, w, h));

    const auto& rows = controller_.rows();
    const float rowH = ProfileController::rowHeight;
    for (size_t i = 0; i < rows.size(); ++i) {
        const float y = static_cast<float>(i) * rowH - controller_.scroll();
        if (y + rowH < 0.0f || y > h) continue;
        const ProfileRow& r = rows[i];
        if (r.depth == 0 && r.id == controller_.selected()) {
            ctx.set_fill_style(BLRgba32(0x805A7CFF));
            ctx.fill_rect(BLRect(0, y, w, rowH));
        } else if (i % 2 == 1) {
            ctx.set_fill_style(BLRgba32(0xFF2A2C31));
            ctx.fill_rect(BLRect(0, y, w, rowH));
        }
        const float base = y + 14.0f;
        const BLRgba32 normal(0xFFE6E6E6), muted(0xFF9A9CA3);
        drawText(ctx, r.name, 11.0f, columnX[0] + 12.0f * static_cast<float>(r.depth), base, normal);
        drawText(ctx, r.type, 11.0f, columnX[1], base, muted);
        drawText(ctx, std::to_string(r.cooks), 11.0f, columnX[2], base, normal);
        drawText(ctx, r.cooks ? millis(r.lastMs) : std::string("-"), 11.0f, columnX[3], base, normal);
        drawText(ctx, r.note, 11.0f, columnX[4], base, muted);
    }
    ctx.restore();
}

}  // namespace jitnodes::editor
