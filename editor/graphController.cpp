#include "graphController.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace jitnodes::editor {

namespace {

constexpr float portHitRadius = 9.0f;
constexpr float minZoom = 0.25f;
constexpr float maxZoom = 3.0f;

float distance(Vec a, Vec b) { return std::hypot(a.x - b.x, a.y - b.y); }

}  // namespace

Vec GraphController::inputPort(NodeId id, int index) const {
    const Node& n = model_.network().node(id);
    const int count = std::max(1, n.inputCount());
    return {n.posX() + nodeWidth * static_cast<float>(index + 1) / static_cast<float>(count + 1), n.posY()};
}

Vec GraphController::outputPort(NodeId id) const {
    const Node& n = model_.network().node(id);
    return {n.posX() + nodeWidth / 2, n.posY() + nodeHeight};
}

std::optional<NodeId> GraphController::nodeAt(Vec screen) const {
    const Vec w = toWorld(screen);
    std::optional<NodeId> hit;
    for (NodeId id : model_.network().nodeIds()) {
        const Node& n = model_.network().node(id);
        if (w.x >= n.posX() && w.x <= n.posX() + nodeWidth && w.y >= n.posY() && w.y <= n.posY() + nodeHeight) hit = id;
    }
    return hit;
}

std::optional<NodeId> GraphController::outputPortAt(Vec screen) const {
    for (NodeId id : model_.network().nodeIds())
        if (distance(toScreen(outputPort(id)), screen) <= portHitRadius) return id;
    return std::nullopt;
}

std::optional<std::pair<NodeId, int>> GraphController::inputPortAt(Vec screen) const {
    for (NodeId id : model_.network().nodeIds()) {
        const int count = model_.network().node(id).inputCount();
        for (int i = 0; i < count; ++i)
            if (distance(toScreen(inputPort(id, i)), screen) <= portHitRadius) return std::make_pair(id, i);
    }
    return std::nullopt;
}

bool GraphController::mouseDown(Vec screen, unsigned buttons, unsigned modifiers) {
    message_.clear();
    last_ = screen;
    moved_ = false;
    detached_ = false;
    collapseOnRelease_ = false;
    if (buttons & (middleButton | rightButton)) {
        mode_ = Mode::pan;
        return false;
    }
    if (!(buttons & leftButton)) return false;

    Network& net = model_.network();
    if (const auto in = inputPortAt(screen)) {
        const NodeId upstream = net.inputOf(in->first, in->second);
        if (upstream != invalidNodeId) {
            net.disconnect(in->first, in->second);
            detached_ = true;
            wire_ = WireDrag{upstream, toWorld(screen)};
            mode_ = Mode::wire;
            return true;
        }
    }
    if (const auto out = outputPortAt(screen)) {
        wire_ = WireDrag{*out, toWorld(screen)};
        mode_ = Mode::wire;
        return true;
    }
    if (const auto node = nodeAt(screen)) {
        if (modifiers & (shiftKey | ctrlKey)) {
            model_.toggleSelected(*node);
        } else if (model_.isSelected(*node)) {
            collapseOnRelease_ = true;
        } else {
            model_.select(*node);
        }
        grabbed_.clear();
        if (model_.isSelected(*node)) {
            for (NodeId id : model_.selection()) grabbed_[id] = {net.node(id).posX(), net.node(id).posY()};
            grabWorld_ = toWorld(screen);
            mode_ = Mode::move;
        } else {
            mode_ = Mode::idle;
        }
        notify(false);
        return true;
    }
    const bool additive = (modifiers & (shiftKey | ctrlKey)) != 0;
    const bool hadSelection = !model_.selection().empty();
    baseSelection_ = additive ? model_.selection() : std::vector<NodeId>{};
    if (!additive) model_.select(invalidNodeId);
    marquee_ = Box{screen, screen};
    mode_ = Mode::marquee;
    if (hadSelection && !additive) notify(false);
    return true;
}

bool GraphController::mouseMove(Vec screen) {
    const Vec delta{screen.x - last_.x, screen.y - last_.y};
    last_ = screen;
    switch (mode_) {
        case Mode::pan:
            pan_.x += delta.x;
            pan_.y += delta.y;
            return true;
        case Mode::move: {
            const Vec w = toWorld(screen);
            for (const auto& [id, start] : grabbed_) model_.moveNode(id, start.x + w.x - grabWorld_.x, start.y + w.y - grabWorld_.y);
            moved_ = true;
            return true;
        }
        case Mode::marquee: {
            marquee_->to = screen;
            const Vec a = toWorld(marquee_->from), b = toWorld(screen);
            const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
            std::vector<NodeId> chosen = baseSelection_;
            const Network& net = model_.network();
            for (NodeId id : net.nodeIds()) {
                const Node& n = net.node(id);
                const bool touches = n.posX() <= x1 && n.posX() + nodeWidth >= x0 && n.posY() <= y1 && n.posY() + nodeHeight >= y0;
                if (touches && std::find(chosen.begin(), chosen.end(), id) == chosen.end()) chosen.push_back(id);
            }
            if (chosen != model_.selection()) {
                model_.setSelection(chosen);
                notify(false);
            }
            return true;
        }
        case Mode::wire:
            wire_->to = toWorld(screen);
            return true;
        case Mode::idle:
            break;
    }
    const NodeId over = nodeAt(screen).value_or(invalidNodeId);
    const bool changed = over != hovered_;
    hovered_ = over;
    return changed;
}

bool GraphController::mouseUp(Vec screen) {
    const Mode mode = mode_;
    mode_ = Mode::idle;
    if (mode == Mode::move) {
        if (moved_) {
            model_.commit(grabbed_.size() > 1 ? "move nodes" : "move node");
            notify(true);
        } else if (collapseOnRelease_) {
            const auto node = nodeAt(screen);
            if (node) model_.select(*node);
            notify(false);
        }
        grabbed_.clear();
        return true;
    }
    if (mode == Mode::marquee) {
        marquee_.reset();
        return true;
    }
    if (mode != Mode::wire || !wire_) return mode == Mode::pan;

    const NodeId from = wire_->from;
    wire_.reset();
    Network& net = model_.network();
    std::optional<std::pair<NodeId, int>> target = inputPortAt(screen);
    if (!target) {
        if (const auto node = nodeAt(screen)) {
            for (int i = 0; i < net.node(*node).inputCount(); ++i) {
                if (net.inputOf(*node, i) == invalidNodeId) {
                    target = std::make_pair(*node, i);
                    break;
                }
            }
        }
    }
    bool changed = detached_;
    std::string label = "disconnect";
    if (target && target->first != from) {
        try {
            net.connect(from, target->first, target->second);
            changed = true;
            label = "connect";
        } catch (const std::runtime_error& e) {
            message_ = e.what();
        }
    }
    if (changed) {
        model_.commit(label);
        notify(true);
    }
    return true;
}

bool GraphController::enterSelected() {
    const NodeId id = model_.selected();
    if (id == invalidNodeId || !model_.canEnter(id) || !model_.enter(id).empty()) return false;
    hovered_ = invalidNodeId;
    wire_.reset();
    mode_ = Mode::idle;
    if (onNavigated) onNavigated();
    notify(false);
    return true;
}

bool GraphController::up() {
    if (!model_.up()) return false;
    hovered_ = invalidNodeId;
    wire_.reset();
    mode_ = Mode::idle;
    if (onNavigated) onNavigated();
    notify(false);
    return true;
}

bool GraphController::doubleClick(Vec screen) {
    const auto node = nodeAt(screen);
    if (!node) return false;
    if (model_.canEnter(*node)) {
        model_.select(*node);
        return enterSelected();
    }
    model_.setDisplay(*node);
    notify(true);
    return true;
}

bool GraphController::wheel(Vec screen, float delta) {
    if (delta == 0.0f) return false;
    const Vec before = toWorld(screen);
    zoom_ = std::clamp(zoom_ * (delta > 0 ? 1.1f : 1.0f / 1.1f), minZoom, maxZoom);
    pan_ = {screen.x - before.x * zoom_, screen.y - before.y * zoom_};
    return true;
}

bool GraphController::deleteSelected() {
    if (!model_.removeSelected()) return false;
    hovered_ = invalidNodeId;
    notify(true);
    return true;
}

bool GraphController::nudge(int stepsX, int stepsY) {
    if (!model_.nudgeSelected(static_cast<float>(stepsX) * nodeStep, static_cast<float>(stepsY) * nodeStep)) return false;
    notify(true);
    return true;
}

bool GraphController::selectAll() {
    model_.selectAll();
    notify(false);
    return true;
}

bool GraphController::copySelected() {
    message_.clear();
    if (!model_.copySelected()) return false;
    if (writeClipboard) writeClipboard(model_.clipboardText());
    notify(false);
    return true;
}

bool GraphController::cutSelected() {
    message_.clear();
    if (!model_.cutSelected()) return false;
    if (writeClipboard) writeClipboard(model_.clipboardText());
    hovered_ = invalidNodeId;
    notify(true);
    return true;
}

bool GraphController::paste() {
    if (readClipboard) model_.setClipboardText(readClipboard());
    message_ = model_.paste();
    notify(message_.empty());
    return true;
}

void GraphController::frameAll(float viewWidth, float viewHeight) {
    const Network& net = model_.network();
    float minX = std::numeric_limits<float>::max(), minY = minX, maxX = std::numeric_limits<float>::lowest(), maxY = maxX;
    bool any = false;
    for (NodeId id : net.nodeIds()) {
        const Node& n = net.node(id);
        if (!n.hasPosition()) continue;
        any = true;
        minX = std::min(minX, n.posX());
        minY = std::min(minY, n.posY());
        maxX = std::max(maxX, n.posX() + nodeWidth);
        maxY = std::max(maxY, n.posY() + nodeHeight);
    }
    if (!any || viewWidth <= 0 || viewHeight <= 0) {
        zoom_ = 1.0f;
        pan_ = {20.0f, 20.0f};
        return;
    }
    const float margin = 30.0f;
    const float fit = std::min((viewWidth - 2 * margin) / (maxX - minX), (viewHeight - 2 * margin) / (maxY - minY));
    zoom_ = std::clamp(fit, minZoom, 1.0f);
    pan_ = {(viewWidth - (maxX - minX) * zoom_) / 2 - minX * zoom_, (viewHeight - (maxY - minY) * zoom_) / 2 - minY * zoom_};
}

}  // namespace jitnodes::editor
