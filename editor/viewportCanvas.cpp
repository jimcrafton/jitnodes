#include "viewportCanvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

#include "drawText.h"
#include "newui/mouse_constants.h"

namespace jitnodes::editor {

namespace {

constexpr size_t segmentBudget = 40000;
constexpr size_t pointBudget = 20000;
constexpr size_t fewPoints = 64;
constexpr size_t coloredBudget = 20000;
constexpr size_t spriteBudget = 5000;

Vec vec(const newui::Point& p) { return {p.x, p.y}; }

unsigned buttonsOf(std::uint32_t mask) {
    return ((mask & newui::mbmLeftButton) ? leftButton : 0) | ((mask & newui::mbmMiddleButton) ? middleButton : 0) |
           ((mask & newui::mbmRightButton) ? rightButton : 0);
}

std::string stepLabel(double step) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "grid %g", step);
    return buf;
}

}  // namespace

ViewportCanvas::ViewportCanvas(ViewportController& controller) : controller_(controller) {
    setVisible(true);

    onMouseDown.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>(
        [this](newui::View&, const newui::Point& p, std::uint32_t buttons, std::uint32_t) {
            if (controller_.mouseDown(vec(p), buttonsOf(buttons))) redraw();
            return newui::SyncReturn::Handled;
        }));
    onMouseMove.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>(
        [this](newui::View&, const newui::Point& p, std::uint32_t, std::uint32_t) {
            if (controller_.mouseMove(vec(p))) redraw();
            return newui::SyncReturn::Handled;
        }));
    onMouseLeft.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>(
        [this](newui::View&, const newui::Point&, std::uint32_t, std::uint32_t) {
            if (controller_.hover()) {
                controller_.clearHover();
                redraw();
            }
            return newui::SyncReturn::Handled;
        }));
    onMouseUp.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>(
        [this](newui::View&, const newui::Point& p, std::uint32_t, std::uint32_t) {
            controller_.mouseUp(vec(p));
            return newui::SyncReturn::Handled;
        }));
    onMouseDblClick.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, std::uint32_t, std::uint32_t)>(
        [this](newui::View&, const newui::Point&, std::uint32_t, std::uint32_t) {
            if (controller_.doubleClick()) redraw();
            return newui::SyncReturn::Handled;
        }));
    onMouseWheel.add(std::function<newui::SyncReturn(newui::View&, const newui::Point&, float)>(
        [this](newui::View&, const newui::Point& p, float delta) {
            if (controller_.wheel(vec(p), delta)) redraw();
            return newui::SyncReturn::Handled;
        }));
}

void ViewportCanvas::drawSprites(BLContext& ctx) {
    const Geometry& geo = *controller_.geometry();
    const float zoom = controller_.zoom();
    const AttribArray* radius = geo.findAttrib(AttribClass::point, "radius");
    const AttribArray* red = geo.findAttrib(AttribClass::point, "Cd.x");
    const AttribArray* green = geo.findAttrib(AttribClass::point, "Cd.y");
    const AttribArray* blue = geo.findAttrib(AttribClass::point, "Cd.z");
    const AttribArray* alpha = geo.findAttrib(AttribClass::point, "alpha");
    const AttribArray* softness = geo.findAttrib(AttribClass::point, "softness");
    const AttribArray* aspect = geo.findAttrib(AttribClass::point, "aspect");
    const AttribArray* shape = geo.findAttrib(AttribClass::point, "shape");
    auto channel = [](const AttribArray* a, size_t i, float fallback) { return a ? a->at(i) : fallback; };
    auto byte = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };

    const size_t stride = ViewportController::drawStride(geo.pointCount(), spriteBudget);
    for (size_t i = 0; i < geo.pointCount(); i += stride) {
        const float r = radius->at(i) * zoom;
        if (!(r > 0.25f)) continue;
        const Vec s = controller_.toScreen({geo.x()[i], geo.y()[i]});
        const uint32_t cr = byte(channel(red, i, 1.0f)), cg = byte(channel(green, i, 1.0f)), cb = byte(channel(blue, i, 1.0f));
        const float a = std::clamp(channel(alpha, i, 1.0f), 0.0f, 1.0f);
        const float soft = std::clamp(channel(softness, i, 1.0f), 0.0f, 1.0f);
        ctx.save();
        ctx.translate(s.x, s.y);
        const float stretch = channel(aspect, i, 1.0f);
        ctx.scale(stretch > 0.0f ? stretch : 1.0f, 1.0f);
        if (channel(shape, i, 0.0f) >= 0.5f) {
            ctx.set_fill_style(BLRgba32(cr, cg, cb, byte(a)));
            ctx.fill_rect(BLRect(-r, -r, 2.0 * r, 2.0 * r));
        } else if (soft < 0.01f) {
            ctx.set_fill_style(BLRgba32(cr, cg, cb, byte(a)));
            ctx.fill_circle(0.0, 0.0, r);
        } else {
            const float core = 1.0f - soft;
            BLGradient gradient(BLRadialGradientValues(0.0, 0.0, 0.0, 0.0, r));
            gradient.add_stop(0.0, BLRgba32(cr, cg, cb, byte(a)));
            if (core > 0.01f) gradient.add_stop(core, BLRgba32(cr, cg, cb, byte(a)));
            gradient.add_stop(core + soft * 0.25f, BLRgba32(cr, cg, cb, byte(a * 0.78f)));
            gradient.add_stop(core + soft * 0.5f, BLRgba32(cr, cg, cb, byte(a * 0.4f)));
            gradient.add_stop(core + soft * 0.75f, BLRgba32(cr, cg, cb, byte(a * 0.12f)));
            gradient.add_stop(1.0, BLRgba32(cr, cg, cb, 0));
            ctx.set_fill_style(gradient);
            ctx.fill_circle(0.0, 0.0, r);
        }
        ctx.restore();
    }
}

void ViewportCanvas::paint(BLContext& ctx) {
    const float w = bounds().width(), h = bounds().height();
    controller_.setViewSize(w, h);
    const float zoom = controller_.zoom();

    ctx.save();
    ctx.set_fill_style(BLRgba32(0xFF1A1B1E));
    ctx.fill_rect(BLRect(0, 0, w, h));

    // Grid lines at a power of ten that keeps them at least about 60 px apart.
    const double step = std::pow(10.0, std::ceil(std::log10(60.0 / zoom)));
    const Vec topLeft = controller_.toWorld({0, 0}), bottomRight = controller_.toWorld({w, h});
    auto gridLines = [&](bool vertical) {
        const double lo = vertical ? topLeft.x : bottomRight.y, hi = vertical ? bottomRight.x : topLeft.y;
        if ((hi - lo) / step > 200) return;
        for (double v = std::ceil(lo / step) * step; v <= hi; v += step) {
            const Vec s = controller_.toScreen(vertical ? Vec{static_cast<float>(v), 0.0f} : Vec{0.0f, static_cast<float>(v)});
            if (std::fabs(v) < step * 0.5) continue;
            ctx.set_fill_style(BLRgba32(0xFF24262A));
            if (vertical) ctx.fill_rect(BLRect(s.x, 0, 1, h));
            else ctx.fill_rect(BLRect(0, s.y, w, 1));
        }
    };
    gridLines(true);
    gridLines(false);
    const Vec origin = controller_.toScreen({0, 0});
    ctx.set_fill_style(BLRgba32(0xFF3A3D43));
    ctx.fill_rect(BLRect(origin.x, 0, 1, h));
    ctx.fill_rect(BLRect(0, origin.y, w, 1));
    drawText(ctx, stepLabel(step), 10.0f, 6.0f, h - 6.0f, BLRgba32(0xFF6A6D75));

    if (controller_.spritesActive() && controller_.geometry()->pointCount() > 0) drawSprites(ctx);

    if (const GeometryHandle& geo = controller_.geometry(); geo && geo->pointCount() > 0 && !controller_.spritesActive()) {
        const float* xs = geo->x();
        const float* ys = geo->y();
        const bool pointValues = controller_.hasValues() && controller_.valueClass() == AttribClass::point;
        const bool primValues = controller_.hasValues() && controller_.valueClass() == AttribClass::primitive;
        const size_t stride = ViewportController::drawStride(geo->pointCount(), pointValues ? coloredBudget : segmentBudget);
        const auto screenOf = [&](size_t i) { return controller_.toScreen({xs[i], ys[i]}); };

        if (pointValues) {
            const std::vector<float>& values = controller_.values();
            ctx.set_stroke_width(2.0f);
            auto segment = [&](size_t a, size_t b) {
                const Vec p = screenOf(a), q = screenOf(b);
                ctx.set_stroke_style(BLRgba32(controller_.colorOf((values[a] + values[b]) * 0.5f)));
                ctx.stroke_line(p.x, p.y, q.x, q.y);
            };
            for (const Primitive& prim : geo->primitives()) {
                if (prim.count < 2) continue;
                size_t previous = prim.start;
                for (uint32_t i = static_cast<uint32_t>(stride);; i += static_cast<uint32_t>(stride)) {
                    const uint32_t at = std::min<uint32_t>(i, prim.count - 1);
                    segment(previous, prim.start + at);
                    previous = prim.start + at;
                    if (at == prim.count - 1) break;
                }
                if (prim.closed) segment(previous, prim.start);
            }
        } else {
            ctx.set_stroke_width(1.5f);
            const std::vector<float>& values = controller_.values();
            BLPath all;
            size_t primIndex = 0;
            for (const Primitive& prim : geo->primitives()) {
                ++primIndex;
                if (prim.count == 0) continue;
                BLPath single;
                BLPath& path = primValues ? single : all;
                for (uint32_t i = 0; i < prim.count; i += static_cast<uint32_t>(stride)) {
                    const Vec s = screenOf(prim.start + i);
                    if (i == 0) path.move_to(s.x, s.y);
                    else path.line_to(s.x, s.y);
                }
                const Vec last = screenOf(prim.start + prim.count - 1);
                if (prim.closed) path.close();
                else path.line_to(last.x, last.y);
                if (primValues) {
                    ctx.set_stroke_style(BLRgba32(primIndex - 1 < values.size() ? controller_.colorOf(values[primIndex - 1]) : 0xFF808080u));
                    ctx.stroke_path(path);
                }
            }
            if (!primValues) {
                ctx.set_stroke_style(BLRgba32(0xFF5A7CFF));
                ctx.stroke_path(all);
            }
        }

        // Dots on top of the lines only when they stay readable; point clouds without primitives are all dots.
        const bool lines = geo->primitiveCount() > 0;
        if (!lines || geo->pointCount() <= fewPoints) {
            const size_t dotStride = ViewportController::drawStride(geo->pointCount(), pointBudget);
            ctx.set_fill_style(BLRgba32(0xFFE6E6E6));
            for (size_t i = 0; i < geo->pointCount(); i += dotStride) {
                const Vec s = screenOf(i);
                if (pointValues) ctx.set_fill_style(BLRgba32(controller_.colorOf(controller_.values()[i])));
                ctx.fill_rect(BLRect(s.x - 2.0, s.y - 2.0, 4, 4));
            }
        }

        if (const auto hover = controller_.hover()) {
            const Vec s = screenOf(*hover);
            ctx.set_stroke_width(1.5f);
            ctx.set_stroke_style(BLRgba32(0xFFFFFFFF));
            ctx.stroke_round_rect(BLRoundRect(s.x - 6, s.y - 6, 12, 12, 6, 6));
            drawText(ctx, controller_.describePoint(*hover), 11.0f, 8.0f, 16.0f, BLRgba32(0xFFE6E6E6));
        }
    }

    if (controller_.hasValues()) {
        // Legend: the colour map from the smallest to the largest value.
        const float barW = 120.0f, barH = 8.0f, x0 = w - barW - 10.0f, y0 = h - 24.0f;
        const int steps = 40;
        for (int i = 0; i < steps; ++i) {
            ctx.set_fill_style(BLRgba32(ViewportController::colormap(static_cast<float>(i) / (steps - 1), controller_.colorMap())));
            ctx.fill_rect(BLRect(x0 + barW * static_cast<float>(i) / steps, y0, barW / steps + 0.5f, barH));
        }
        char lo[32], hi[32];
        std::snprintf(lo, sizeof(lo), "%.4g", static_cast<double>(controller_.valueMin()));
        std::snprintf(hi, sizeof(hi), "%.4g", static_cast<double>(controller_.valueMax()));
        const BLRgba32 muted(0xFFB8BBC2);
        drawText(ctx, controller_.valueLabel(), 10.0f, x0, y0 - 4.0f, muted);
        drawText(ctx, lo, 10.0f, x0, y0 + barH + 11.0f, muted);
        drawText(ctx, hi, 10.0f, x0 + barW - 5.5f * static_cast<float>(std::strlen(hi)), y0 + barH + 11.0f, muted);
    }
    ctx.restore();
}

}  // namespace jitnodes::editor
