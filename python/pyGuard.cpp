#include "pyGuard.h"

#include <algorithm>
#include <vector>

#include <pybind11/pybind11.h>

#include "pyHandles.h"

namespace py = pybind11;

namespace jitnodes::python {

namespace {

std::vector<EventRestriction*>& activeRestrictions() {
    static std::vector<EventRestriction*> list;
    return list;
}

[[noreturn]] void deny(const std::string& message) {
    PyErr_SetString(PyExc_PermissionError, message.c_str());
    throw py::error_already_set();
}

bool startsWith(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }

// True when `network` is `inner` or sits somewhere inside it.
bool isInside(const Network* network, const Network* inner) {
    for (const Network* n = network; n != nullptr; n = n->parent()) {
        if (n == inner) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool isMutatingName(const std::string& name) {
    if (startsWith(name, "set_") || startsWith(name, "add_") || startsWith(name, "remove_") || startsWith(name, "clear_") || startsWith(name, "rename")) {
        return true;
    }
    return name == "connect" || name == "disconnect" || name == "undo" || name == "redo" || name == "clear" || name == "freeze_node" || name == "touch";
}

EventRestriction::EventRestriction(Network& network, NodeId instance, std::string event, Mode mode)
    : network_(&network), instance_(instance), event_(std::move(event)), mode_(mode), inner_(network.node(instance).childNetwork()),
      alive_(network.lifetime()), thread_(std::this_thread::get_id()) {
    for (const EventRestriction* other : activeRestrictions()) {
        if (other->network_ == network_ && other->instance_ == instance_ && other->event_ == event_) {
            throw std::runtime_error(event_ + " is already running for this node");
        }
    }
    activeRestrictions().push_back(this);
}

EventRestriction::~EventRestriction() {
    auto& list = activeRestrictions();
    list.erase(std::remove(list.begin(), list.end(), this), list.end());
}

bool EventRestriction::active() { return !activeRestrictions().empty(); }

void EventRestriction::checkNode(const NetworkRef& network, NodeId id, const std::string& what) {
    for (EventRestriction* r : activeRestrictions()) {
        if (r->alive_.expired() || r->thread_ != std::this_thread::get_id()) {
            continue;
        }
        const Network* target = network.raw();
        if (r->mode_ == Mode::readOnly) {
            deny(r->event_ + " cannot change the document (" + what + ")");
        }
        if (r->mode_ == Mode::onlyThisNode) {
            if (target == r->network_ && id == r->instance_) {
                continue;
            }
            deny(r->event_ + " may only change the new instance (" + what + ")");
        }
        if (r->inner_ != nullptr && isInside(target, r->inner_)) {
            deny(r->event_ + " cannot change what is inside this asset (" + what + ")");
        }
        if (target != r->network_) {
            continue;
        }
        if (id == r->instance_) {
            deny(r->event_ + " cannot change this asset (" + what + ")");
        }
        if (!r->upstreamBuilt_ || r->upstreamRevision_ != r->network_->topologyRevision()) {
            r->upstream_.clear();
            std::vector<NodeId> pending{r->instance_};
            while (!pending.empty()) {
                const NodeId current = pending.back();
                pending.pop_back();
                const int inputs = r->network_->node(current).inputCount();
                for (int i = 0; i < inputs; ++i) {
                    const NodeId from = r->network_->inputOf(current, i);
                    if (from != invalidNodeId && r->upstream_.insert(from).second) {
                        pending.push_back(from);
                    }
                }
            }
            r->upstreamRevision_ = r->network_->topologyRevision();
            r->upstreamBuilt_ = true;
        }
        if (r->upstream_.count(id) != 0) {
            deny(r->event_ + " cannot change what this asset reads from (" + what + ")");
        }
    }
}

void EventRestriction::checkNetwork(const NetworkRef& network, const std::string& what) {
    for (const EventRestriction* r : activeRestrictions()) {
        if (r->alive_.expired() || r->thread_ != std::this_thread::get_id()) {
            continue;
        }
        const Network* target = network.raw();
        if (r->mode_ != Mode::instanceAndInputs || target == r->network_ || (r->inner_ != nullptr && isInside(target, r->inner_))) {
            deny(r->event_ + " cannot change this network (" + what + ")");
        }
    }
}

void EventRestriction::checkCreate(const NetworkRef& network, const std::string& what) {
    for (const EventRestriction* r : activeRestrictions()) {
        if (r->alive_.expired() || r->thread_ != std::this_thread::get_id()) {
            continue;
        }
        if (r->mode_ != Mode::instanceAndInputs || (r->inner_ != nullptr && isInside(network.raw(), r->inner_))) {
            deny(r->event_ + " cannot add nodes here (" + what + ")");
        }
    }
}

void EventRestriction::checkLibrary(const std::string& what) {
    for (const EventRestriction* r : activeRestrictions()) {
        if (!r->alive_.expired() && r->thread_ == std::this_thread::get_id()) {
            deny(r->event_ + " cannot change the asset library (" + what + ")");
        }
    }
}

}  // namespace jitnodes::python
