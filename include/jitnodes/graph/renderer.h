#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>

#include "jitnodes/core/matrix2d.h"
#include "jitnodes/geo/floatImage.h"
#include "jitnodes/geo/raster.h"
#include "jitnodes/graph/node.h"

namespace jitnodes {

// Where the document is drawn: a transform from document units to output pixels and the output's size. Rasterizing and presenting are separate
// steps with separate views: a renderer draws a region larger than the window at one transform (see RenderView::withMargin), and a viewer places
// that picture under the current window with a cheap shift and scale, so a pan inside the drawn region costs no new render.
struct RenderView {
    Matrix2D transform;             // document units -> output pixels (y down)
    int width = 0, height = 0;      // output size in pixels
    bool transparentPage = false;   // the page is not painted with its colour: the result keeps real alpha

    bool valid() const { return width > 0 && height > 0 && transform.invertible(); }

    // The view to render for a window: the same transform shifted so `margin` of the window's size is added on each side (0.5: twice as wide
    // and tall), so the window can move about inside the drawn picture.
    static RenderView withMargin(const RenderView& window, double margin) {
        RenderView drawn = window;
        const int padX = static_cast<int>(std::lround(window.width * margin)), padY = static_cast<int>(std::lround(window.height * margin));
        drawn.transform = window.transform.then(Matrix2D::translation(padX, padY));
        drawn.width = window.width + 2 * padX;
        drawn.height = window.height + 2 * padY;
        return drawn;
    }

    // Where a point of the window's output lands in a picture drawn at this view: window pixels -> document -> drawn pixels. A viewer draws the
    // picture with the inverse of this.
    Matrix2D fromWindow(const RenderView& window) const { return window.transform.inverse().then(transform); }

    // True when a picture drawn at this view still serves `window`: the same kind of page, the two transforms differ only by a move and a
    // scale within `tolerance` (0.15: 15%), and the whole window lies inside the drawn picture. A turn or a flip between them needs a new render.
    bool covers(const RenderView& window, double tolerance = 0.15) const {
        if (!valid() || !window.valid() || transparentPage != window.transparentPage) {
            return false;
        }
        const Matrix2D toDrawn = fromWindow(window);
        if (!toDrawn.isTranslateScale(1e-9)) {
            return false;
        }
        const double ratio = toDrawn.a;
        if (ratio < 1.0 / (1.0 + tolerance) || ratio > 1.0 + tolerance) {
            return false;
        }
        const auto topLeft = toDrawn.apply(0.0, 0.0);
        const auto bottomRight = toDrawn.apply(window.width, window.height);
        const double slack = 1e-6;
        return topLeft.first >= -slack && topLeft.second >= -slack && bottomRight.first <= width + slack && bottomRight.second <= height + slack;
    }

    bool operator==(const RenderView& other) const {
        return transform == other.transform && width == other.width && height == other.height && transparentPage == other.transparentPage;
    }
    bool operator!=(const RenderView& other) const { return !(*this == other); }
};

// What a renderer node made. Abstract so the pixels can live anywhere: in memory now (ImageResult, or a Blend2D image in an application), in a
// GPU texture later. A viewer that knows the concrete type presents it directly; anything else reads the pixels.
class RenderResult {
public:
    virtual ~RenderResult() = default;
    // The view the pixels were drawn at.
    virtual const RenderView& drawnAt() const = 0;
    // A copy of the pixels, premultiplied 0xAARRGGBB, row 0 at the top. False when they cannot be read.
    virtual bool readPixels(Image& out) const = 0;
    // The same picture as float colour (see FloatImage). The default widens the 8-bit pixels, so every result can be read this way; a renderer
    // that really draws in float overrides it to give the full range, and its readPixels then shows the 8-bit version.
    virtual bool readFloatPixels(FloatImage& out) const {
        Image pixels;
        if (!readPixels(pixels)) {
            return false;
        }
        out = FloatImage::fromArgb8(pixels);
        return true;
    }
};

// A result held as float colour in memory.
class FloatImageResult : public RenderResult {
public:
    FloatImageResult(RenderView view, FloatImage pixels) : view_(view), pixels_(std::move(pixels)) {}
    const RenderView& drawnAt() const override { return view_; }
    bool readPixels(Image& out) const override {
        out = pixels_.toArgb8();
        return true;
    }
    bool readFloatPixels(FloatImage& out) const override {
        out = pixels_;
        return true;
    }
    const FloatImage& image() const { return pixels_; }

private:
    RenderView view_;
    FloatImage pixels_;
};

// A result held as an Image in memory.
class ImageResult : public RenderResult {
public:
    ImageResult(RenderView view, Image pixels) : view_(view), pixels_(std::move(pixels)) {}
    const RenderView& drawnAt() const override { return view_; }
    bool readPixels(Image& out) const override {
        out = pixels_;
        return true;
    }
    const Image& image() const { return pixels_; }

private:
    RenderView view_;
    Image pixels_;
};

// A node that turns the geometry reaching it into a picture: geometry in, an image out. A viewer asks the display node for its image
// (Network::cookImage), and a graph without one shows nothing.
//
// The view is not a parameter: it is set by the viewer, is never saved, makes no undo step and does not touch the node's revision. setView
// only drops the node's cached cook, and only when the picture it holds no longer serves the window, so a pan inside the drawn region leaves it
// alone. cookValue() (written by the renderer) reads renderView(), draws viewToRender(renderView()) and returns the picture as the image of
// the Value; it also hands it to setResult() so a viewer can read the latest picture without cooking.
class RendererNode : public ValueNode {
public:
    using ValueNode::ValueNode;

    PortType outputType() const override { return PortType::image; }

    // Tells the node which window is shown. Drops the cached cook when the held picture does not serve it.
    void setView(const RenderView& view) {
        bool stale = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            renderView_ = view;
            stale = needsRenderLocked(view);
        }
        if (stale) {
            invalidateCook();
        }
    }
    RenderView renderView() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return renderView_;
    }
    bool needsRender(const RenderView& view) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return needsRenderLocked(view);
    }
    // The latest picture; null before the first render.
    std::shared_ptr<const RenderResult> result() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return result_;
    }

protected:
    // The view to draw for a window; a renderer may choose another margin.
    virtual RenderView viewToRender(const RenderView& window) const { return RenderView::withMargin(window, 0.5); }
    void setResult(std::shared_ptr<const RenderResult> result) {
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = std::move(result);
    }

private:
    bool needsRenderLocked(const RenderView& view) const { return !result_ || !result_->drawnAt().covers(view); }

    mutable std::mutex mutex_;
    RenderView renderView_;
    std::shared_ptr<const RenderResult> result_;
};

}  // namespace jitnodes
