#include "jitnodes/graph/history.h"

#include <algorithm>

#include "jitnodes/graph/serialize.h"

namespace jitnodes {

History::History(Network& net, size_t maxStates) : net_(net), maxStates_(std::max<size_t>(maxStates, 2)) {
    states_.push_back({"initial", saveNetwork(net_, false)});
}

bool History::commit(const std::string& label) {
    std::string text = saveNetwork(net_, false);
    if (text == states_[index_].text) return false;
    states_.resize(index_ + 1);
    states_.push_back({label, std::move(text)});
    if (states_.size() > maxStates_) states_.erase(states_.begin());
    index_ = states_.size() - 1;
    return true;
}

void History::restore(size_t index) {
    loadNetwork(net_, states_[index].text);
    index_ = index;
}

bool History::undo() {
    if (!canUndo()) return false;
    restore(index_ - 1);
    return true;
}

bool History::redo() {
    if (!canRedo()) return false;
    restore(index_ + 1);
    return true;
}

const std::string& History::undoLabel() const {
    static const std::string none;
    return canUndo() ? states_[index_].label : none;
}

const std::string& History::redoLabel() const {
    static const std::string none;
    return canRedo() ? states_[index_ + 1].label : none;
}

}  // namespace jitnodes
