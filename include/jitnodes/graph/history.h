#pragma once

#include <string>
#include <vector>

#include "jitnodes/graph/network.h"
#include "jitnodes/graph/serialize.h"

namespace jitnodes {

// Undo/redo of the graph (nodes, parameters, wiring, display node, fps). Call commit() after each user
// edit. States share the serialized text of unchanged nodes, and restoring only changes the nodes that
// differ, so the rest keep their cooked results. Node ids are preserved and the playhead is untouched.
// Uncommitted edits are discarded by undo/redo. Asset definitions are not part of the history.
class History {
public:
    explicit History(Network& net, size_t maxStates = 100);

    // Records the current graph if it differs from the latest snapshot; discards any redo states.
    bool commit(const std::string& label = {});

    bool canUndo() const { return index_ > 0; }
    bool canRedo() const { return index_ + 1 < states_.size(); }
    bool undo();
    bool redo();

    const std::string& undoLabel() const;  // label of the edit undo would revert
    const std::string& redoLabel() const;  // label of the edit redo would reapply
    size_t size() const { return states_.size(); }
    const RestoreStats& lastRestore() const { return lastRestore_; }
    size_t storedBytes() const;  // bytes of distinct node texts held by all states

private:
    struct State {
        std::string label;
        GraphSnapshot snapshot;
    };

    void restore(size_t index);

    Network& net_;
    size_t maxStates_;
    std::vector<State> states_;
    size_t index_ = 0;
    RestoreStats lastRestore_;
};

}  // namespace jitnodes
