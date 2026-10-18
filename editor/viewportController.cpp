#include "viewportController.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace jitnodes::editor {

namespace {

constexpr float fitMargin = 24.0f;
constexpr float minZoom = 1e-4f;
constexpr float maxZoom = 1e6f;

}  // namespace

void ViewportController::setGeometry(GeometryHandle geometry) {
    if (geometry == geometry_) return;
    geometry_ = std::move(geometry);
    bounds_ = Bounds{};
    if (geometry_ && geometry_->pointCount() > 0) {
        const float* x = geometry_->x();
        const float* y = geometry_->y();
        bounds_.minX = bounds_.maxX = x[0];
        bounds_.minY = bounds_.maxY = y[0];
        for (size_t i = 1; i < geometry_->pointCount(); ++i) {
            bounds_.minX = std::min(bounds_.minX, x[i]);
            bounds_.maxX = std::max(bounds_.maxX, x[i]);
            bounds_.minY = std::min(bounds_.minY, y[i]);
            bounds_.maxY = std::max(bounds_.maxY, y[i]);
        }
        bounds_.valid = std::isfinite(bounds_.minX) && std::isfinite(bounds_.maxX) && std::isfinite(bounds_.minY) && std::isfinite(bounds_.maxY);
    }
    hover_.reset();
    rebuildChoices();
    fitIfNeeded();
}

void ViewportController::rebuildChoices() {
    choices_.clear();
    if (geometry_) {
        const auto& pointAttribs = geometry_->attribs(AttribClass::point);
        for (const auto& entry : pointAttribs) choices_.push_back({entry.first, AttribClass::point, entry.first, false});
        for (const auto& entry : pointAttribs) {
            const std::string& n = entry.first;
            if (n.size() > 2 && n.compare(n.size() - 2, 2, ".x") == 0 && pointAttribs.count(n.substr(0, n.size() - 2) + ".y")) {
                const std::string base = n.substr(0, n.size() - 2);
                choices_.push_back({base + " (length)", AttribClass::point, base, true});
            }
        }
        for (const auto& entry : geometry_->attribs(AttribClass::primitive)) choices_.push_back({"prim: " + entry.first, AttribClass::primitive, entry.first, false});
    }
    recomputeValues();
}

void ViewportController::selectChoice(std::optional<size_t> index) {
    const std::string label = index && *index < choices_.size() ? choices_[*index].label : std::string();
    if (label != selectedLabel_) manualLow_ = manualHigh_ = std::nullopt;
    selectedLabel_ = label;
    recomputeValues();
}

void ViewportController::setRange(std::optional<float> low, std::optional<float> high) {
    manualLow_ = low && std::isfinite(*low) ? low : std::nullopt;
    manualHigh_ = high && std::isfinite(*high) ? high : std::nullopt;
    recomputeValues();
}

std::optional<size_t> ViewportController::selectedChoice() const {
    for (size_t i = 0; i < choices_.size(); ++i)
        if (choices_[i].label == selectedLabel_) return i;
    return std::nullopt;
}

void ViewportController::recomputeValues() {
    values_.clear();
    valueLabel_.clear();
    valueMin_ = 0.0f;
    valueMax_ = 1.0f;
    const auto selected = selectedChoice();
    if (!selected || !geometry_) return;
    const AttribChoice& choice = choices_[*selected];
    valueClass_ = choice.cls;
    valueLabel_ = choice.label;
    const size_t count = geometry_->count(choice.cls);
    values_.assign(count, 0.0f);
    if (choice.magnitude) {
        const AttribArray* parts[3] = {geometry_->findAttrib(choice.cls, choice.name + ".x"), geometry_->findAttrib(choice.cls, choice.name + ".y"),
                                       geometry_->findAttrib(choice.cls, choice.name + ".z")};
        for (size_t i = 0; i < count; ++i) {
            float sum = 0.0f;
            for (const AttribArray* a : parts)
                if (a) sum += a->at(i) * a->at(i);
            values_[i] = std::sqrt(sum);
        }
    } else if (const AttribArray* a = geometry_->findAttrib(choice.cls, choice.name)) {
        for (size_t i = 0; i < count; ++i) values_[i] = a->at(i);
    }
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    for (float v : values_) {
        if (!std::isfinite(v)) continue;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (lo > hi) lo = hi = 0.0f;
    if (manualLow_) lo = *manualLow_;
    if (manualHigh_) hi = *manualHigh_;
    if (std::fabs(hi - lo) < 1e-12f) {
        lo -= 0.5f;
        hi += 0.5f;
    }
    valueMin_ = lo;
    valueMax_ = hi;
}

const char* ViewportController::colorMapName(ColorMap map) {
    switch (map) {
        case ColorMap::viridis: return "Viridis";
        case ColorMap::magma: return "Magma";
        case ColorMap::grayscale: return "Grayscale";
        case ColorMap::coolWarm: return "Cool-warm";
    }
    return "";
}

uint32_t ViewportController::colormap(float t, ColorMap map) {
    static const uint32_t viridis[] = {0xFF440154u, 0xFF3B528Bu, 0xFF21918Cu, 0xFF5EC962u, 0xFFFDE725u};
    static const uint32_t magma[] = {0xFF000004u, 0xFF51127Cu, 0xFFB73779u, 0xFFFC8961u, 0xFFFCFDBFu};
    static const uint32_t grayscale[] = {0xFF000000u, 0xFF404040u, 0xFF808080u, 0xFFC0C0C0u, 0xFFFFFFFFu};
    static const uint32_t coolWarm[] = {0xFF3B4CC0u, 0xFF8DB0FEu, 0xFFDDDDDDu, 0xFFF4987Au, 0xFFB40426u};
    const uint32_t* stops = map == ColorMap::magma ? magma : map == ColorMap::grayscale ? grayscale : map == ColorMap::coolWarm ? coolWarm : viridis;
    if (!std::isfinite(t)) return 0xFF808080u;
    t = std::clamp(t, 0.0f, 1.0f) * 4.0f;
    const size_t i = std::min<size_t>(static_cast<size_t>(t), 3);
    const float f = t - static_cast<float>(i);
    auto channel = [&](int shift) {
        const float a = static_cast<float>((stops[i] >> shift) & 255u), b = static_cast<float>((stops[i + 1] >> shift) & 255u);
        return static_cast<uint32_t>(std::lround(a + (b - a) * f));
    };
    return 0xFF000000u | (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

uint32_t ViewportController::colorOf(float value) const {
    return colormap(std::isfinite(value) ? (value - valueMin_) / (valueMax_ - valueMin_) : std::numeric_limits<float>::quiet_NaN(), colorMap_);
}

std::optional<size_t> ViewportController::nearestPoint(Vec screen, float radiusPx) const {
    if (!geometry_ || geometry_->pointCount() == 0 || geometry_->pointCount() > 300000) return std::nullopt;
    const float* xs = geometry_->x();
    const float* ys = geometry_->y();
    float best = radiusPx * radiusPx;
    std::optional<size_t> found;
    for (size_t i = 0; i < geometry_->pointCount(); ++i) {
        const Vec s = toScreen({xs[i], ys[i]});
        const float d = (s.x - screen.x) * (s.x - screen.x) + (s.y - screen.y) * (s.y - screen.y);
        if (d <= best) {
            best = d;
            found = i;
        }
    }
    return found;
}

std::string ViewportController::describePoint(size_t index) const {
    if (!geometry_ || index >= geometry_->pointCount()) return {};
    char buf[160];
    std::snprintf(buf, sizeof(buf), "#%zu  x=%.4g  y=%.4g", index, static_cast<double>(geometry_->x()[index]), static_cast<double>(geometry_->y()[index]));
    std::string text = buf;
    if (hasValues() && valueClass_ == AttribClass::point && index < values_.size()) {
        std::snprintf(buf, sizeof(buf), "  %s=%.4g", valueLabel_.c_str(), static_cast<double>(values_[index]));
        text += buf;
    } else if (hasValues() && valueClass_ == AttribClass::primitive) {
        const auto& prims = geometry_->primitives();
        for (size_t p = 0; p < prims.size() && p < values_.size(); ++p) {
            if (index >= prims[p].start && index < static_cast<size_t>(prims[p].start) + prims[p].count) {
                std::snprintf(buf, sizeof(buf), "  %s=%.4g", valueLabel_.c_str(), static_cast<double>(values_[p]));
                text += buf;
                break;
            }
        }
    }
    return text;
}

std::string ViewportController::info() const {
    if (!geometry_) return "no geometry";
    const size_t points = geometry_->pointCount(), prims = geometry_->primitiveCount();
    return std::to_string(points) + (points == 1 ? " point, " : " points, ") + std::to_string(prims) + (prims == 1 ? " primitive" : " primitives");
}

void ViewportController::setViewSize(float width, float height) {
    viewW_ = width;
    viewH_ = height;
    fitIfNeeded();
}

void ViewportController::fitIfNeeded() {
    if (!fitted_ && bounds_.valid && viewW_ > 0.0f && viewH_ > 0.0f) fit();
}

void ViewportController::fit() {
    if (viewW_ <= 0.0f || viewH_ <= 0.0f) return;
    if (!bounds_.valid) {
        zoom_ = 1.0f;
        pan_ = {viewW_ / 2, viewH_ / 2};
        return;
    }
    const float w = std::max(bounds_.maxX - bounds_.minX, 1e-6f), h = std::max(bounds_.maxY - bounds_.minY, 1e-6f);
    const float availW = std::max(viewW_ - 2 * fitMargin, 1.0f), availH = std::max(viewH_ - 2 * fitMargin, 1.0f);
    zoom_ = std::clamp(std::min(availW / w, availH / h), minZoom, maxZoom);
    const float cx = (bounds_.minX + bounds_.maxX) / 2, cy = (bounds_.minY + bounds_.maxY) / 2;
    pan_ = {viewW_ / 2 - cx * zoom_, viewH_ / 2 + cy * zoom_};
    fitted_ = true;
}

bool ViewportController::mouseDown(Vec screen, unsigned buttons) {
    dragging_ = (buttons & (leftButton | middleButton | rightButton)) != 0;
    last_ = screen;
    return false;
}

bool ViewportController::mouseMove(Vec screen) {
    if (!dragging_) {
        const auto over = nearestPoint(screen, 14.0f);
        const bool changed = over != hover_;
        hover_ = over;
        return changed;
    }
    pan_.x += screen.x - last_.x;
    pan_.y += screen.y - last_.y;
    last_ = screen;
    fitted_ = true;
    return true;
}

bool ViewportController::mouseUp(Vec) {
    dragging_ = false;
    return false;
}

bool ViewportController::wheel(Vec screen, float delta) {
    if (delta == 0.0f) return false;
    const Vec before = toWorld(screen);
    zoom_ = std::clamp(zoom_ * (delta > 0 ? 1.15f : 1.0f / 1.15f), minZoom, maxZoom);
    pan_ = {screen.x - before.x * zoom_, screen.y + before.y * zoom_};
    fitted_ = true;
    return true;
}

}  // namespace jitnodes::editor
