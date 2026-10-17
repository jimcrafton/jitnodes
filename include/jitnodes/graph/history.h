#pragma once

#include <string>
#include <vector>

#include "jitnodes/graph/network.h"

namespace jitnodes {

// Snapshot based undo/redo of the graph (nodes, parameters, wiring, display node). Call commit()
// after each user edit. Restoring rebuilds the network, so every node recooks, but node ids are
// preserved and the playhead is untouched.
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

private:
    struct State {
        std::string label;
        std::string text;
    };

    void restore(size_t index);

    Network& net_;
    size_t maxStates_;
    std::vector<State> states_;
    size_t index_ = 0;
};

}  // namespace jitnodes
