#include "profileController.h"

#include <algorithm>
#include <cmath>

namespace jitnodes::editor {

void ProfileController::setRows(std::vector<ProfileRow> rows) {
    rows_ = std::move(rows);
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll());
}

void ProfileController::setViewHeight(float height) {
    viewHeight_ = height;
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll());
}

float ProfileController::maxScroll() const { return std::max(0.0f, static_cast<float>(rows_.size()) * rowHeight - viewHeight_); }

bool ProfileController::wheel(float delta) {
    const float before = scroll_;
    scroll_ = std::clamp(scroll_ - (delta > 0 ? 1.0f : -1.0f) * 3.0f * rowHeight, 0.0f, maxScroll());
    return scroll_ != before;
}

std::optional<size_t> ProfileController::rowAt(float y) const {
    if (y < 0.0f) return std::nullopt;
    const size_t row = static_cast<size_t>(std::floor((y + scroll_) / rowHeight));
    if (row >= rows_.size()) return std::nullopt;
    return row;
}

NodeId ProfileController::selectableNode(size_t row) const { return row < rows_.size() && rows_[row].depth == 0 ? rows_[row].id : invalidNodeId; }

}  // namespace jitnodes::editor
