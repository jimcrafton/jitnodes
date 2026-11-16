#include "pyHandles.h"

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/functional.h>
#include <pybind11/stl.h>

// The generated bindings (scriptgen, from the // @scriptable marks in the jitnodes headers).
void registerJitnodesScripting(pybind11::module_& m);

namespace py = pybind11;

namespace jitnodes::python {

void registerGeometryArrays(py::module_& core);

NetworkRef NetworkRef::owning(std::shared_ptr<Network> network) {
    NetworkRef ref;
    ref.raw_ = network.get();
    ref.lifetime_ = network->lifetime();
    ref.owner_ = std::move(network);
    return ref;
}

NetworkRef NetworkRef::borrowed(Network& network) {
    NetworkRef ref;
    ref.raw_ = &network;
    ref.lifetime_ = network.lifetime();
    return ref;
}

Network& NetworkRef::get() const {
    if (!alive() || raw_ == nullptr) {
        throw StaleHandleError("the network this handle points to no longer exists");
    }
    return *raw_;
}

PyNode::PyNode(NetworkRef network, NodeId id) : network_(std::move(network)), id_(id) {
    if (network_.alive()) {
        generation_ = network_.get().generation();
    }
}

bool PyNode::alive() const {
    return network_.alive() && network_.get().generation() == generation_ && network_.get().hasNode(id_);
}

Node& PyNode::get() const {
    Network& network = network_.get();
    if (network.generation() != generation_) {
        throw StaleHandleError("the network was cleared or reloaded since this node handle was made");
    }
    if (!network.hasNode(id_)) {
        throw StaleHandleError("node " + std::to_string(id_) + " has been removed from its network");
    }
    return network.node(id_);
}

PyHistory::PyHistory(NetworkRef network, size_t maxStates)
    : network_(std::move(network)), history_(std::make_shared<History>(network_.get(), maxStates)) {}

History& PyHistory::get() const {
    network_.get();
    return *history_;
}

namespace {

// The classes generated into module._core, kept so results can be recognised and wrapped. Never freed: the
// objects must not be released after the interpreter is finalized.
py::object* coreNetworkClass = nullptr;
py::object* coreNodeClass = nullptr;
py::object* coreHistoryClass = nullptr;

// What a handle's calls are made on; decides how arguments and results are translated.
enum class Level { network, node, other };

// Network methods that return node ids, and the ones that return many.
const std::set<std::string> singleNodeResults = {"add_node", "add_node_with_id", "find_node", "input_of", "display_node"};
const std::set<std::string> listNodeResults = {"node_ids", "consumers_of", "execution_order"};
const std::set<std::string> setNodeResults = {"dependents_of"};

py::object coreOf(Network& network) {
    return py::cast(&network, py::return_value_policy::reference);
}

py::object coreOf(Node& node) {
    return py::cast(&node, py::return_value_policy::reference);
}

py::object nodeHandle(const NetworkRef& network, NodeId id) {
    return py::cast(PyNode(network, id));
}

py::object unwrapArgument(const py::handle& value, Level level, const NetworkRef& network);

py::object unwrapAll(const py::handle& sequence, bool asSet, const NetworkRef& network) {
    py::list items;
    for (const py::handle item : sequence) {
        items.append(unwrapArgument(item, Level::network, network));
    }
    if (asSet) {
        return py::set(items);
    }
    return std::move(items);
}

// Handles become what the generated class expects: a node id for a network method (which must be a node of that
// network: an id means nothing in another one), the raw object otherwise.
py::object unwrapArgument(const py::handle& value, Level level, const NetworkRef& network) {
    if (py::isinstance<PyNode>(value)) {
        const PyNode& node = value.cast<const PyNode&>();
        if (level == Level::network) {
            if (node.network().raw() != network.raw()) {
                throw py::value_error("the node belongs to a different network");
            }
            node.get();
            return py::int_(node.id());
        }
        return coreOf(node.get());
    }
    if (py::isinstance<PyNetwork>(value)) {
        return coreOf(value.cast<const PyNetwork&>().ref().get());
    }
    if (level == Level::network && (py::isinstance<py::list>(value) || py::isinstance<py::tuple>(value))) {
        return unwrapAll(value, false, network);
    }
    if (level == Level::network && py::isinstance<py::set>(value)) {
        return unwrapAll(value, true, network);
    }
    return py::reinterpret_borrow<py::object>(value);
}

NetworkRef refFor(const NetworkRef& known, Network& network) {
    return known.raw() == &network ? known : NetworkRef::borrowed(network);
}

py::object wrapResult(const NetworkRef& network, Level level, const std::string& name, py::object result) {
    if (result.is_none()) {
        return result;
    }
    if (level == Level::network) {
        if (singleNodeResults.count(name)) {
            const NodeId id = result.cast<NodeId>();
            return id == invalidNodeId ? py::none() : nodeHandle(network, id);
        }
        if (listNodeResults.count(name)) {
            py::list nodes;
            for (const py::handle id : result) {
                nodes.append(nodeHandle(network, id.cast<NodeId>()));
            }
            return std::move(nodes);
        }
        if (setNodeResults.count(name)) {
            py::set nodes;
            for (const py::handle id : result) {
                nodes.add(nodeHandle(network, id.cast<NodeId>()));
            }
            return std::move(nodes);
        }
    }
    if (coreNodeClass != nullptr && py::isinstance(result, *coreNodeClass)) {
        // A node a method of this network (or of one of its nodes) hands back belongs to this network.
        return nodeHandle(network, result.cast<Node&>().id());
    }
    if (coreNetworkClass != nullptr && py::isinstance(result, *coreNetworkClass)) {
        return py::cast(PyNetwork(refFor(network, result.cast<Network&>())));
    }
    return result;
}

// A callable that checks the handle, translates the arguments, calls the generated method of the same name and
// translates the result. The handle is checked again at call time, so a bound method kept in a variable is safe too.
py::object makeForwarder(std::function<py::object()> resolve, NetworkRef network, Level level, std::string name) {
    return py::cpp_function([resolve = std::move(resolve), network = std::move(network), level, name](py::args args, py::kwargs kwargs) {
        py::object core = resolve();
        py::tuple callArgs(args.size());
        for (size_t i = 0; i < args.size(); ++i) {
            callArgs[i] = unwrapArgument(args[i], level, network);
        }
        py::dict callKwargs;
        for (const auto item : kwargs) {
            callKwargs[item.first] = unwrapArgument(item.second, level, network);
        }
        py::object result = core.attr(name.c_str())(*callArgs, **callKwargs);
        return wrapResult(network, level, name, std::move(result));
    });
}

// Looks `name` up on the generated class: a method becomes a forwarder, anything else (a field, a property) is read
// from the object itself. Returns an empty object when the generated class has no such member.
py::object forwardAttribute(const py::object& coreClass, const std::function<py::object()>& resolve, const NetworkRef& network,
                            Level level, const std::string& name) {
    if (!py::hasattr(coreClass, name.c_str())) {
        return py::object();
    }
    if (PyCallable_Check(coreClass.attr(name.c_str()).ptr())) {
        return makeForwarder(resolve, network, level, name);
    }
    return wrapResult(network, level, name, resolve().attr(name.c_str()));
}

bool hasString(const Node& node, const std::string& name) {
    const std::vector<std::string> names = node.stringNames();
    return std::find(names.begin(), names.end(), name) != names.end();
}

py::object readParameter(const PyNode& handle, const std::string& name) {
    Node& node = handle.get();
    if (node.hasFloat(name)) {
        return py::float_(node.getFloat(name));
    }
    if (hasString(node, name)) {
        return py::str(node.getString(name));
    }
    if (node.hasGeometryParam(name)) {
        return py::cast(node.geometryParam(name));
    }
    return py::object();
}

[[noreturn]] void noSuchMember(const PyNode& handle, const std::string& name) {
    throw py::attribute_error("node '" + handle.get().name() + "' has no parameter or method '" + name + "'");
}

void writeParameter(const PyNode& handle, const std::string& name, const py::object& value) {
    Node& node = handle.get();
    if (node.hasFloat(name)) {
        node.setFloat(name, value.cast<float>());
    } else if (hasString(node, name)) {
        node.setString(name, value.cast<std::string>());
    } else if (node.hasGeometryParam(name)) {
        node.setGeometryParam(name, value.cast<GeometryHandle>());
    } else {
        noSuchMember(handle, name);
    }
}

bool sameNode(const PyNode& a, const PyNode& b) {
    return a.network().raw() == b.network().raw() && a.id() == b.id() && a.generation() == b.generation();
}

NodeId idOf(const NetworkRef& network, const py::object& value) {
    if (py::isinstance<PyNode>(value)) {
        const PyNode& node = value.cast<const PyNode&>();
        if (node.network().raw() != network.raw()) {
            throw py::value_error("the node belongs to a different network");
        }
        node.get();
        return node.id();
    }
    if (py::isinstance<py::int_>(value)) {
        return value.cast<NodeId>();
    }
    if (py::isinstance<py::str>(value)) {
        return network.get().findNode(value.cast<std::string>());
    }
    throw py::type_error("expected a node, a node id or a node name");
}

py::list nodeHandles(const NetworkRef& network) {
    py::list nodes;
    for (const NodeId id : network.get().nodeIds()) {
        nodes.append(nodeHandle(network, id));
    }
    return nodes;
}

void registerNetwork(py::module_& m) {
    py::class_<PyNetwork> cls(m, "Network",
        "A node graph. Everything you get from it (nodes, histories) is a handle that raises StaleHandleError once what it\n"
        "points to is gone. Methods not listed here are the engine's, in snake_case (see jitnodes._core.Network).");

    cls.def(py::init([](size_t threads) {
            return PyNetwork(NetworkRef::owning(std::make_shared<Network>(threads)));
        }), py::arg("threads") = 0)
        .def_property_readonly("valid", [](const PyNetwork& self) { return self.ref().alive(); })
        .def("__repr__", [](const PyNetwork& self) {
            if (!self.ref().alive()) {
                return std::string("<jitnodes.Network (gone)>");
            }
            return "<jitnodes.Network nodes=" + std::to_string(self.ref().get().nodeCount()) + ">";
        })
        .def("__len__", [](const PyNetwork& self) { return self.ref().get().nodeCount(); })
        .def("nodes", [](const PyNetwork& self) { return nodeHandles(self.ref()); }, "Handles to every node, in id order.")
        .def("__iter__", [](const PyNetwork& self) { return py::iter(nodeHandles(self.ref())); })
        .def("__contains__", [](const PyNetwork& self, const py::object& key) {
            Network& network = self.ref().get();
            if (py::isinstance<PyNode>(key)) {
                const PyNode& node = key.cast<const PyNode&>();
                return node.network().raw() == self.ref().raw() && node.alive();
            }
            if (py::isinstance<py::str>(key)) {
                return network.findNode(key.cast<std::string>()) != invalidNodeId;
            }
            if (py::isinstance<py::int_>(key)) {
                return network.hasNode(key.cast<NodeId>());
            }
            return false;
        })
        .def("__getitem__", [](const PyNetwork& self, const py::object& key) {
            const NodeId id = idOf(self.ref(), key);
            if (id == invalidNodeId || !self.ref().get().hasNode(id)) {
                throw py::key_error(py::str(key).cast<std::string>());
            }
            return nodeHandle(self.ref(), id);
        })
        .def("node", [](const PyNetwork& self, const py::object& key) {
            const NodeId id = idOf(self.ref(), key);
            if (id == invalidNodeId || !self.ref().get().hasNode(id)) {
                throw py::key_error(py::str(key).cast<std::string>());
            }
            return nodeHandle(self.ref(), id);
        }, py::arg("key"), "The node with this id or name.")
        .def("__getattr__", [](const PyNetwork& self, const std::string& name) -> py::object {
            if (name.rfind("__", 0) == 0) {
                throw py::attribute_error(name);
            }
            const NetworkRef ref = self.ref();
            const auto resolve = [ref]() { return coreOf(ref.get()); };
            py::object forwarded = forwardAttribute(*coreNetworkClass, resolve, ref, Level::network, name);
            if (!forwarded) {
                throw py::attribute_error("jitnodes.Network has no member '" + name + "'");
            }
            return forwarded;
        });
}

void registerNode(py::module_& m) {
    py::class_<PyNode> cls(m, "Node",
        "A node in a Network. Parameters read and write like attributes (node.count = 8, node['count']); a >> b connects a\n"
        "to the first input of b. Methods not listed here are the engine's, in snake_case (see jitnodes._core.Node).");

    cls.def_property_readonly("id", &PyNode::id)
        .def_property_readonly("valid", [](const PyNode& self) { return self.alive(); })
        .def_property_readonly("network", [](const PyNode& self) { return PyNetwork(self.network()); })
        .def("__repr__", [](const PyNode& self) {
            if (!self.alive()) {
                return "<jitnodes.Node id=" + std::to_string(self.id()) + " (gone)>";
            }
            const Node& node = self.get();
            return "<jitnodes.Node '" + node.name() + "' id=" + std::to_string(self.id()) + " type=" + node.typeName() + ">";
        })
        .def("__eq__", [](const PyNode& self, const py::object& other) {
            return py::isinstance<PyNode>(other) && sameNode(self, other.cast<const PyNode&>());
        })
        .def("__hash__", [](const PyNode& self) {
            return std::hash<const void*>()(self.network().raw()) ^ (static_cast<size_t>(self.id()) * 0x9e3779b97f4a7c15ull) ^
                   (static_cast<size_t>(self.generation()) * 0xc2b2ae3d27d4eb4full);
        })
        .def("__getitem__", [](const PyNode& self, const std::string& name) {
            py::object value = readParameter(self, name);
            if (!value) {
                throw py::key_error(name);
            }
            return value;
        })
        .def("__setitem__", [](const PyNode& self, const std::string& name, const py::object& value) {
            writeParameter(self, name, value);
        })
        .def("__contains__", [](const PyNode& self, const std::string& name) { return bool(readParameter(self, name)); })
        .def("__getattr__", [](const PyNode& self, const std::string& name) -> py::object {
            if (name.rfind("__", 0) == 0) {
                throw py::attribute_error(name);
            }
            const NetworkRef network = self.network();
            const PyNode handle = self;
            const auto resolve = [handle]() -> py::object { return coreOf(handle.get()); };
            py::object forwarded = forwardAttribute(*coreNodeClass, resolve, network, Level::node, name);
            if (forwarded) {
                return forwarded;
            }
            py::object value = readParameter(self, name);
            if (!value) {
                noSuchMember(self, name);
            }
            return value;
        })
        .def("__setattr__", [](const PyNode& self, const std::string& name, const py::object& value) {
            writeParameter(self, name, value);
        })
        .def("connect", [](const PyNode& self, const PyNode& target, int input) {
            if (target.network().raw() != self.network().raw()) {
                throw py::value_error("the nodes belong to different networks");
            }
            self.get();
            target.get();
            self.network().get().connect(self.id(), target.id(), input);
            return target;
        }, py::arg("target"), py::arg("input") = 0, "Wires this node to an input of `target` and returns `target`.")
        .def("__rshift__", [](const PyNode& self, const PyNode& target) {
            if (target.network().raw() != self.network().raw()) {
                throw py::value_error("the nodes belong to different networks");
            }
            self.get();
            target.get();
            self.network().get().connect(self.id(), target.id(), 0);
            return target;
        })
        .def("disconnect", [](const PyNode& self, int input) {
            self.get();
            self.network().get().disconnect(self.id(), input);
        }, py::arg("input") = 0, "Unwires an input of this node.");
}

void registerHistory(py::module_& m) {
    py::class_<PyHistory> cls(m, "History",
        "Undo and redo of a Network. Methods are the engine's, in snake_case (see jitnodes._core.History).");

    cls.def(py::init([](const PyNetwork& network, size_t maxStates) {
            return PyHistory(network.ref(), maxStates);
        }), py::arg("network"), py::arg("max_states") = 100)
        .def("__getattr__", [](const PyHistory& self, const std::string& name) -> py::object {
            if (name.rfind("__", 0) == 0) {
                throw py::attribute_error(name);
            }
            const NetworkRef network = self.network();
            const std::shared_ptr<History> history = self.shared();
            const auto resolve = [network, history]() -> py::object {
                network.get();
                return py::cast(history.get(), py::return_value_policy::reference);
            };
            py::object forwarded = forwardAttribute(*coreHistoryClass, resolve, network, Level::other, name);
            if (!forwarded) {
                throw py::attribute_error("jitnodes.History has no member '" + name + "'");
            }
            return forwarded;
        });
}

}  // namespace

void registerModule(py::module_& module) {
    py::module_ core = module.def_submodule("_core",
        "The engine's classes exactly as scriptgen generates them. They are raw references: do not keep their objects, use the handles.");
    registerJitnodesScripting(core);
    registerGeometryArrays(core);

    if (coreNetworkClass == nullptr) {
        coreNetworkClass = new py::object(core.attr("Network"));
        coreNodeClass = new py::object(core.attr("Node"));
        coreHistoryClass = new py::object(core.attr("History"));
    }

    // Values that own what they hold are safe to hand out as they are.
    for (const char* name : {"Geometry", "Keyframe", "AttribClass", "AttribType", "PrimKind", "KeyInterp"}) {
        module.attr(name) = core.attr(name);
    }

    py::register_exception<StaleHandleError>(module, "StaleHandleError", PyExc_ReferenceError);
    registerNode(module);
    registerNetwork(module);
    registerHistory(module);
}

py::object wrapNetwork(Network& network) {
    py::module_::import("jitnodes");   // registers the handle classes if nothing has imported the module yet
    return py::cast(PyNetwork(NetworkRef::borrowed(network)));
}

}  // namespace jitnodes::python
