#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

#include "jitnodes/graph/network.h"

namespace jitnodes::python {

class NetworkRef;

// Python edits that must not happen: names of engine methods that change a network or a node (set_float, add_node, connect, undo, ...).
bool isMutatingName(const std::string& name);

// The runtime half of the rules for asset events (docs/assets.md, "Scripts"). While an EventRestriction exists, the handles refuse (with
// PermissionError) the edits the event may not make:
//   Mode::instanceAndInputs (onFrameChanged): the instance, anything inside it, and anything it reads from. What the instance reads from is
//     found lazily, on the first edit of some other node of its network, by following the wiring upstream; it is cached until the network's
//     structure changes. Nodes that only come after the instance, and everything else, may be edited.
//   Mode::readOnly (onVersionUpdated): nothing in any network.
// Whatever the mode, the asset library cannot be changed from inside an event. An event cannot start itself: a second restriction for the same
// instance and event throws std::runtime_error. Restrictions nest (an event can make another one run); every active one is checked.
// Main thread only (scripts reach the document from there, see curvy's script dispatch).
class EventRestriction {
public:
    enum class Mode { instanceAndInputs, readOnly };

    // `instance` is a node of `network` (a subnet or asset instance).
    EventRestriction(Network& network, NodeId instance, std::string event, Mode mode);
    ~EventRestriction();
    EventRestriction(const EventRestriction&) = delete;
    EventRestriction& operator=(const EventRestriction&) = delete;

    static bool active();
    // Each throws a Python PermissionError (as a pybind11::error_already_set) when an active restriction forbids the edit; `what` names the call.
    static void checkNode(const NetworkRef& network, NodeId id, const std::string& what);
    static void checkNetwork(const NetworkRef& network, const std::string& what);
    // Adding a node: refused only in a read-only event and inside the instance (a new node is not something the instance reads from).
    static void checkCreate(const NetworkRef& network, const std::string& what);
    static void checkLibrary(const std::string& what);

private:
    Network* network_;
    NodeId instance_;
    std::string event_;
    Mode mode_;
    Network* inner_;
    std::weak_ptr<void> alive_;
    std::unordered_set<NodeId> upstream_;
    std::uint64_t upstreamRevision_ = 0;
    bool upstreamBuilt_ = false;
};

}  // namespace jitnodes::python
