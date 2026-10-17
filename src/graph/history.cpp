#include "jitnodes/graph/history.h"

#include <algorithm>
#include <set>

#include "jitnodes/graph/serialize.h"

namespace jitnodes {

History::History(Network& net, size_t maxStates) : net_(net), maxStates_(std::max<size_t>(maxStates, 2)) {
    states_.push_back({"initial", snapshotNetwork(net_)});
}

bool History::commit(const std::string& label) {
    GraphSnapshot snapshot = snapshotNetwork(net_, &states_[index_].snapshot);
    if (sameGraph(snapshot, states_[index_].snapshot)) return false;
    states_.resize(index_ + 1);
    states_.push_back({label, std::move(snapshot)});
    if (states_.size() > maxStates_) states_.erase(states_.begin());
    index_ = states_.size() - 1;
    return true;
}

void History::restore(size_t index) {
    const GraphSnapshot live = snapshotNetwork(net_, &states_[index_].snapshot);
    lastRestore_ = restoreSnapshot(net_, states_[index].snapshot, &live);
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

size_t History::storedBytes() const {
    std::set<const std::string*> seen;
    size_t bytes = 0;
    for (const State& s : states_)
        for (const auto& entry : s.snapshot.nodes)
            if (seen.insert(entry.second.text.get()).second) bytes += entry.second.text->size();
    return bytes;
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
