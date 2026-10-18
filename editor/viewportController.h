#pragma once

#include <optional>
#include <string>
#include <vector>

#include "graphController.h"

namespace jitnodes::editor {

// View logic of the 2D viewport, free of any UI toolkit: the cooked geometry of the display node, a view transform with
// world y pointing up, fit, pan (left or middle drag) and zoom around the cursor. The first non-empty geometry after a
// reset is fitted automatically once the view size is known.
class ViewportController {
public:
    struct Bounds {
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
        bool valid = false;
    };

    // An attribute the viewport can colour by: a point or primitive attribute, or the length of a vector attribute
    // (`name.x`, `name.y`, optionally `name.z`).
    struct AttribChoice {
        std::string label;
        AttribClass cls = AttribClass::point;
        std::string name;  // for a magnitude: the vector's base name
        bool magnitude = false;
    };

    void setGeometry(GeometryHandle geometry);
    // The attributes of the current geometry; the selection is kept across geometries by label (none when the new geometry
    // has no such attribute).
    const std::vector<AttribChoice>& choices() const { return choices_; }
    void selectChoice(std::optional<size_t> index);
    std::optional<size_t> selectedChoice() const;
    bool hasValues() const { return !values_.empty(); }
    AttribClass valueClass() const { return valueClass_; }
    const std::vector<float>& values() const { return values_; }
    float valueMin() const { return valueMin_; }
    float valueMax() const { return valueMax_; }
    const std::string& valueLabel() const { return valueLabel_; }
    enum class ColorMap { viridis, magma, grayscale, coolWarm };
    static constexpr size_t colorMapCount = 4;
    static const char* colorMapName(ColorMap map);
    ColorMap colorMap() const { return colorMap_; }
    void setColorMap(ColorMap map) { colorMap_ = map; }
    // The range the colour map spans. It is the smallest and largest value unless an end is set; each end is cleared
    // by passing nullopt, and both are cleared when another attribute is chosen.
    void setRange(std::optional<float> low, std::optional<float> high);
    std::optional<float> manualLow() const { return manualLow_; }
    std::optional<float> manualHigh() const { return manualHigh_; }
    // Maps t in [0, 1] to 0xAARRGGBB (grey for values that are not finite); the default map is viridis-like.
    static uint32_t colormap(float t, ColorMap map = ColorMap::viridis);
    uint32_t colorOf(float value) const;

    // The point nearest to a screen position within `radiusPx`, and the hovered point (set by mouse moves without a button).
    std::optional<size_t> nearestPoint(Vec screen, float radiusPx) const;
    std::optional<size_t> hover() const { return hover_; }
    void clearHover() { hover_.reset(); }
    std::string describePoint(size_t index) const;
    // Points with a `radius` attribute are drawn as sprites (see the editor README) while no colour attribute is chosen.
    bool spritesActive() const { return !hasValues() && geometry_ && geometry_->findAttrib(AttribClass::point, "radius"); }
    const GeometryHandle& geometry() const { return geometry_; }
    const Bounds& bounds() const { return bounds_; }
    std::string info() const;

    void setViewSize(float width, float height);
    void resetFit() { fitted_ = false; }
    void fit();

    float zoom() const { return zoom_; }
    Vec pan() const { return pan_; }
    Vec toScreen(Vec world) const { return {pan_.x + world.x * zoom_, pan_.y - world.y * zoom_}; }
    Vec toWorld(Vec screen) const { return {(screen.x - pan_.x) / zoom_, (pan_.y - screen.y) / zoom_}; }

    bool mouseDown(Vec screen, unsigned buttons);
    bool mouseMove(Vec screen);
    bool mouseUp(Vec screen);
    bool wheel(Vec screen, float delta);
    bool doubleClick() {
        fit();
        return true;
    }

    // Largest step so that drawing every step-th element stays within the budget (1 when everything fits).
    static size_t drawStride(size_t count, size_t budget) { return count <= budget || budget == 0 ? 1 : (count + budget - 1) / budget; }

private:
    void fitIfNeeded();
    void rebuildChoices();
    void recomputeValues();

    GeometryHandle geometry_;
    Bounds bounds_;
    float viewW_ = 0.0f;
    float viewH_ = 0.0f;
    float zoom_ = 1.0f;
    Vec pan_{0.0f, 0.0f};
    bool fitted_ = false;
    bool dragging_ = false;
    std::vector<AttribChoice> choices_;
    std::string selectedLabel_;
    ColorMap colorMap_ = ColorMap::viridis;
    std::optional<float> manualLow_, manualHigh_;
    std::vector<float> values_;
    AttribClass valueClass_ = AttribClass::point;
    float valueMin_ = 0.0f, valueMax_ = 1.0f;
    std::string valueLabel_;
    std::optional<size_t> hover_;
    Vec last_;
};

}  // namespace jitnodes::editor
