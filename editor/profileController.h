#pragma once

#include <optional>
#include <vector>

#include "editorModel.h"

namespace jitnodes::editor {

// Scrolling and hit testing of the profile list, free of any UI toolkit. Rows are a fixed height; `y` arguments are in
// view coordinates (0 at the top of the list).
class ProfileController {
public:
    static constexpr float rowHeight = 20.0f;

    void setRows(std::vector<ProfileRow> rows);
    const std::vector<ProfileRow>& rows() const { return rows_; }
    void setViewHeight(float height);

    NodeId selected() const { return selected_; }
    void setSelected(NodeId id) { selected_ = id; }

    float scroll() const { return scroll_; }
    float maxScroll() const;
    // Scrolls by three rows per wheel step (a positive delta moves up); returns true when the position changed.
    bool wheel(float delta);
    std::optional<size_t> rowAt(float y) const;
    // The root-graph node of a row, or invalidNodeId for rows inside a subnet (they are not selectable here).
    NodeId selectableNode(size_t row) const;

private:
    std::vector<ProfileRow> rows_;
    float viewHeight_ = 0.0f;
    float scroll_ = 0.0f;
    NodeId selected_ = invalidNodeId;
};

}  // namespace jitnodes::editor
