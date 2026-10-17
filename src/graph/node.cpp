#include "jitnodes/graph/node.h"

#include <stdexcept>

#include "jitnodes/graph/network.h"

namespace jitnodes {

Node::Node(std::string typeName, int inputCount) : typeName_(std::move(typeName)), inputCount_(inputCount) {}

Node::~Node() = default;

void Node::declareFloat(const std::string& name, float defaultValue) { floats_[name] = defaultValue; }

void Node::declareString(const std::string& name, const std::string& defaultValue) { strings_[name] = defaultValue; }

void Node::setFloat(const std::string& name, float value) {
    auto it = floats_.find(name);
    if (it == floats_.end()) throw std::runtime_error("unknown float parameter '" + name + "' on " + typeName_);
    if (it->second == value) return;
    it->second = value;
    markChanged();
}

float Node::getFloat(const std::string& name) const {
    auto it = floats_.find(name);
    if (it == floats_.end()) throw std::runtime_error("unknown float parameter '" + name + "' on " + typeName_);
    return it->second;
}

void Node::setString(const std::string& name, const std::string& value) {
    auto it = strings_.find(name);
    if (it == strings_.end()) throw std::runtime_error("unknown string parameter '" + name + "' on " + typeName_);
    if (it->second == value) return;
    it->second = value;
    markChanged();
}

const std::string& Node::getString(const std::string& name) const {
    auto it = strings_.find(name);
    if (it == strings_.end()) throw std::runtime_error("unknown string parameter '" + name + "' on " + typeName_);
    return it->second;
}

void Node::markChanged() {
    if (network_) network_->markDirty(id_);
    else dirty_ = true;
}

}  // namespace jitnodes
