#pragma once

#include <functional>
#include <optional>

#include "editorModel.h"
#include "graphLayout.h"

namespace jitnodes::editor {

struct Vec {
    float x = 0.0f;
    float y = 0.0f;
};

enum MouseButton : unsigned { leftButton = 1, middleButton = 2, rightButton = 4 };

// Interaction logic of the node graph canvas, free of any UI toolkit: events arrive in screen (view) coordinates and
// act on the EditorModel. Every event returns true when the canvas has to be repainted.
//   left button: on an output port starts a wire; on a connected input port detaches that wire and carries it; on a node
//   selects and drags it; on empty space deselects and pans. Middle and right button pan. The wheel zooms around the
//   cursor. Dropping a wire on an input port connects that input, dropping it on a node uses its first free input.
class GraphController {
public:
    explicit GraphController(EditorModel& model) : model_(model) {}

    // Called after a change that was committed to the history (graphChanged) or after a selection change.
    std::function<void(bool graphChanged)> onChanged;
    // Called after the editor entered or left a subnet (the view should be refitted).
    std::function<void()> onNavigated;

    EditorModel& model() const { return model_; }
    float zoom() const { return zoom_; }
    Vec pan() const { return pan_; }
    Vec toWorld(Vec screen) const { return {(screen.x - pan_.x) / zoom_, (screen.y - pan_.y) / zoom_}; }
    Vec toScreen(Vec world) const { return {world.x * zoom_ + pan_.x, world.y * zoom_ + pan_.y}; }

    Vec inputPort(NodeId id, int index) const;
    Vec outputPort(NodeId id) const;

    std::optional<NodeId> nodeAt(Vec screen) const;
    // Port hit tests use a fixed screen-space radius.
    std::optional<NodeId> outputPortAt(Vec screen) const;
    std::optional<std::pair<NodeId, int>> inputPortAt(Vec screen) const;

    bool mouseDown(Vec screen, unsigned buttons);
    bool mouseMove(Vec screen);
    bool mouseUp(Vec screen);
    // Double-clicking a subnet enters it; any other node becomes the display node.
    bool doubleClick(Vec screen);
    bool enterSelected();
    bool up();
    bool wheel(Vec screen, float delta);
    bool deleteSelected();
    void frameAll(float viewWidth, float viewHeight);

    struct WireDrag {
        NodeId from = invalidNodeId;
        Vec to;
    };
    const std::optional<WireDrag>& wireDrag() const { return wire_; }
    NodeId hovered() const { return hovered_; }
    const std::string& message() const { return message_; }

private:
    enum class Mode { idle, pan, move, wire };

    void notify(bool graphChanged) {
        if (onChanged) onChanged(graphChanged);
    }

    EditorModel& model_;
    float zoom_ = 1.0f;
    Vec pan_{20.0f, 20.0f};
    Mode mode_ = Mode::idle;
    Vec last_;
    Vec grabOffset_;
    bool moved_ = false;
    bool detached_ = false;
    std::optional<WireDrag> wire_;
    NodeId hovered_ = invalidNodeId;
    std::string message_;
};

}  // namespace jitnodes::editor
