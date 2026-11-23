#pragma once

#include <memory>
#include <stdexcept>

#include <pybind11/pybind11.h>

#include "jitnodes/graph/history.h"
#include "jitnodes/graph/network.h"

// Python access to jitnodes. The classes scriptgen generates (module jitnodes._core) wrap the engine's
// objects directly, so a script that kept one after its node or document went away would crash. What a
// script sees instead are the handles below: they hold an id and a lifetime token, check both on every
// call, raise StaleHandleError when what they point at is gone, and forward the call to the generated class.
namespace jitnodes::python {

class StaleHandleError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Reaches a Network from Python. A network a script created is owned by the reference (it lives as long as any
// handle to it); an application's network is reached through its lifetime token only, so closing the document
// is never held up by a script.
class NetworkRef {
public:
    static NetworkRef owning(std::shared_ptr<Network> network);
    static NetworkRef borrowed(Network& network);

    bool alive() const { return !lifetime_.expired(); }
    Network& get() const;   // throws StaleHandleError when the network is gone
    Network* raw() const { return raw_; }
    bool owns() const { return owner_ != nullptr; }

private:
    Network* raw_ = nullptr;
    std::weak_ptr<void> lifetime_;
    std::shared_ptr<Network> owner_;
};

class PyNetwork {
public:
    explicit PyNetwork(NetworkRef ref) : ref_(std::move(ref)) {}
    const NetworkRef& ref() const { return ref_; }

private:
    NetworkRef ref_;
};

class PyNode {
public:
    PyNode(NetworkRef network, NodeId id);
    const NetworkRef& network() const { return network_; }
    NodeId id() const { return id_; }
    // The network's generation when the handle was made; clear() changes it, which makes the handle stale even if the
    // id is taken again by a new node.
    std::uint64_t generation() const { return generation_; }
    bool alive() const;
    Node& get() const;   // throws StaleHandleError when the network or the node is gone

private:
    NetworkRef network_;
    NodeId id_;
    std::uint64_t generation_ = 0;
};

class PyHistory {
public:
    PyHistory(NetworkRef network, size_t maxStates);
    const NetworkRef& network() const { return network_; }
    std::shared_ptr<History> shared() const { return history_; }
    History& get() const;   // throws StaleHandleError when the network is gone

private:
    NetworkRef network_;
    std::shared_ptr<History> history_;
};

// Adds the jitnodes classes to `module`: the generated ones under module._core, the handles (Network, Node,
// History) and the plain value types (Geometry, Keyframe and the enums) in the module itself.
void registerModule(pybind11::module_& module);

// A handle to a network the application owns: it expires with the network, and does not keep it alive.
pybind11::object wrapNetwork(Network& network);

}  // namespace jitnodes::python
