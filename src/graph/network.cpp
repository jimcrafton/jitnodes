#include "jitnodes/graph/network.h"

#include <algorithm>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>

#include "jitnodes/jit/fuse.h"

namespace jitnodes {

NodeRegistry& NodeRegistry::instance() {
    static NodeRegistry registry;
    return registry;
}

void NodeRegistry::registerType(const std::string& typeName, Creator creator) {
    creators_[typeName] = std::move(creator);
}

std::unique_ptr<Node> NodeRegistry::create(const std::string& typeName) const {
    auto it = creators_.find(typeName);
    if (it == creators_.end()) throw std::runtime_error("unknown node type '" + typeName + "'");
    return it->second();
}

std::vector<std::string> NodeRegistry::typeNames() const {
    std::vector<std::string> names;
    for (const auto& entry : creators_) names.push_back(entry.first);
    return names;
}

Network::Network() {
    static std::once_flag once;
    std::call_once(once, registerBuiltinNodes);
}

Network::Slot& Network::slot(NodeId id) {
    auto it = slots_.find(id);
    if (it == slots_.end()) throw std::runtime_error("invalid node id " + std::to_string(id));
    return it->second;
}

const Network::Slot& Network::slot(NodeId id) const {
    auto it = slots_.find(id);
    if (it == slots_.end()) throw std::runtime_error("invalid node id " + std::to_string(id));
    return it->second;
}

NodeId Network::addNode(const std::string& typeName, const std::string& name) {
    std::unique_ptr<Node> n = NodeRegistry::instance().create(typeName);
    NodeId id = nextId_++;
    n->id_ = id;
    n->network_ = this;
    n->name_ = name.empty() ? typeName + std::to_string(id) : name;
    Slot s;
    s.inputs.assign(static_cast<size_t>(n->inputCount()), invalidNodeId);
    s.node = std::move(n);
    slots_.emplace(id, std::move(s));
    return id;
}

void Network::removeNode(NodeId id) {
    slot(id);
    for (auto& [otherId, s] : slots_) {
        for (size_t i = 0; i < s.inputs.size(); ++i) {
            if (s.inputs[i] == id) {
                s.inputs[i] = invalidNodeId;
                markDirty(otherId);
            }
        }
    }
    slots_.erase(id);
    if (displayNode_ == id) displayNode_ = invalidNodeId;
}

Node& Network::node(NodeId id) { return *slot(id).node; }
const Node& Network::node(NodeId id) const { return *slot(id).node; }

NodeId Network::findNode(const std::string& name) const {
    for (const auto& [id, s] : slots_)
        if (s.node->name() == name) return id;
    return invalidNodeId;
}

bool Network::reaches(NodeId from, NodeId target) const {
    if (from == target) return true;
    for (NodeId in : slot(from).inputs)
        if (in != invalidNodeId && reaches(in, target)) return true;
    return false;
}

void Network::connect(NodeId from, NodeId to, int inputIndex) {
    slot(from);
    Slot& dst = slot(to);
    if (inputIndex < 0 || static_cast<size_t>(inputIndex) >= dst.inputs.size())
        throw std::runtime_error("input index out of range on " + dst.node->name());
    if (reaches(from, to)) throw std::runtime_error("connection would create a cycle");
    dst.inputs[static_cast<size_t>(inputIndex)] = from;
    markDirty(to);
}

void Network::disconnect(NodeId to, int inputIndex) {
    Slot& dst = slot(to);
    if (inputIndex < 0 || static_cast<size_t>(inputIndex) >= dst.inputs.size())
        throw std::runtime_error("input index out of range on " + dst.node->name());
    dst.inputs[static_cast<size_t>(inputIndex)] = invalidNodeId;
    markDirty(to);
}

NodeId Network::inputOf(NodeId to, int inputIndex) const {
    const Slot& s = slot(to);
    return s.inputs.at(static_cast<size_t>(inputIndex));
}

void Network::markDirty(NodeId id) {
    slot(id);
    std::set<NodeId> seen;
    std::vector<NodeId> pending{id};
    while (!pending.empty()) {
        NodeId current = pending.back();
        pending.pop_back();
        if (!seen.insert(current).second) continue;
        Node& n = *slot(current).node;
        n.dirty_ = true;
        n.cooked_.reset();
        for (const auto& [otherId, other] : slots_)
            if (std::find(other.inputs.begin(), other.inputs.end(), current) != other.inputs.end()) pending.push_back(otherId);
    }
}

void Network::setTime(double t) {
    if (t == time_) return;
    time_ = t;
    for (auto& entry : slots_) markDirty(entry.first);
}

GeometryHandle Network::cook(NodeId id) {
    Slot& s = slot(id);
    Node& n = *s.node;
    if (!n.dirty_ && n.cooked_) return n.cooked_;
    if (fusionEnabled_ && n.isPointwise()) return cookPointwiseChain(id);

    std::vector<GeometryHandle> inputs;
    inputs.reserve(s.inputs.size());
    for (size_t i = 0; i < s.inputs.size(); ++i) {
        if (s.inputs[i] == invalidNodeId)
            throw std::runtime_error(n.name() + ": input " + std::to_string(i) + " is not connected");
        inputs.push_back(cook(s.inputs[i]));
    }

    CookContext ctx{compiler_, time_};
    GeometryHandle result = n.cook(ctx, inputs);
    n.cooked_ = result;
    n.dirty_ = false;
    ++n.cookCount_;
    return result;
}

size_t Network::consumerCount(NodeId id) const {
    size_t count = 0;
    for (const auto& entry : slots_)
        count += static_cast<size_t>(std::count(entry.second.inputs.begin(), entry.second.inputs.end(), id));
    return count;
}

GeometryHandle Network::cookPointwiseChain(NodeId id) {
    std::vector<NodeId> chain{id};
    for (;;) {
        NodeId up = slot(chain.back()).inputs[0];
        if (up == invalidNodeId) break;
        const Node& upNode = *slot(up).node;
        const bool fusable = upNode.isPointwise() && (upNode.dirty_ || !upNode.cooked_) && up != displayNode_ &&
                             consumerCount(up) == 1;
        if (!fusable) break;
        chain.push_back(up);
    }

    const NodeId baseId = slot(chain.back()).inputs[0];
    if (baseId == invalidNodeId) throw std::runtime_error(node(chain.back()).name() + ": input 0 is not connected");
    GeometryHandle base = cook(baseId);

    PointKernel fused;
    for (size_t i = chain.size(); i-- > 0;) {
        PointKernel k;
        node(chain[i]).pointKernel(k);
        if (i + 1 == chain.size()) {
            fused = std::move(k);
        } else {
            fused.spec = composeKernels(fused.spec, k.spec);
            fused.params.insert(fused.params.end(), k.params.begin(), k.params.end());
        }
    }

    auto geo = std::make_shared<Geometry>(*base);
    compiler_.compile(fused.spec)->run(*geo, fused.params);

    Node& tail = node(id);
    tail.cooked_ = geo;
    tail.dirty_ = false;
    ++tail.cookCount_;
    if (chain.size() > 1) ++fusedChainCount_;
    return geo;
}

std::vector<NodeId> Network::executionOrder(NodeId id) const {
    std::vector<NodeId> order;
    std::set<NodeId> seen;
    std::function<void(NodeId)> visit = [&](NodeId n) {
        if (!seen.insert(n).second) return;
        for (NodeId in : slot(n).inputs)
            if (in != invalidNodeId) visit(in);
        order.push_back(n);
    };
    visit(id);
    return order;
}

}  // namespace jitnodes
