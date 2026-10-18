#pragma once

#include <cmath>
#include <map>
#include <memory>
#include <string>

#include "newui/newui.h"
#include "newui/font.h"

namespace jitnodes::editor {

// Draws UTF-8 text with the baseline at y; sizes under 6 px are skipped.
inline void drawText(BLContext& ctx, const std::string& s, float size, float x, float y, BLRgba32 color) {
    if (size < 6.0f) return;
    // A graph draws thousands of labels per paint; building a font for each one is wasted work.
    static std::map<int, std::unique_ptr<newui::Font>> fonts;
    const int halfPoints = static_cast<int>(std::lround(size * 2.0f));
    auto& font = fonts[halfPoints];
    if (!font) font = std::make_unique<newui::Font>("Segoe UI", static_cast<float>(halfPoints) / 2.0f);
    BLFont* f = font->blFont();
    if (!f) return;
    ctx.set_fill_style(color);
    ctx.fill_utf8_text(BLPoint(x, y), *f, s.c_str(), s.size());
}

}  // namespace jitnodes::editor
