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
    // An estimate: each distinct node state counts its geometry and a fixed amount for the node itself.
    constexpr size_t nodeBytes = 512;
    std::set<const Node*> seenNodes;
    std::set<const Geometry*> seenGeometry;
    size_t bytes = 0;
    const auto addGeometry = [&](const GeometryHandle& handle) {
        if (handle && seenGeometry.insert(handle.get()).second) bytes += handle->memoryBytes();
    };
    for (const State& s : states_) {
        for (const auto& entry : s.snapshot.nodes) {
            const Node& state = *entry.second.state;
            if (!seenNodes.insert(&state).second) continue;
            bytes += nodeBytes;
            addGeometry(state.frozenGeometry());
            for (const std::string& name : state.geometryNames()) addGeometry(state.geometryParam(name));
        }
    }
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
