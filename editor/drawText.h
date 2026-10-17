#pragma once

#include <cmath>
#include <string>

#include "newui/newui.h"
#include "newui/font.h"

namespace jitnodes::editor {

// Draws UTF-8 text with the baseline at y; sizes under 6 px are skipped.
inline void drawText(BLContext& ctx, const std::string& s, float size, float x, float y, BLRgba32 color) {
    if (size < 6.0f) return;
    newui::Font font("Segoe UI", std::round(size * 2.0f) / 2.0f);
    BLFont* f = font.blFont();
    if (!f) return;
    ctx.set_fill_style(color);
    ctx.fill_utf8_text(BLPoint(x, y), *f, s.c_str(), s.size());
}

}  // namespace jitnodes::editor
