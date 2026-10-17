#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "jitnodes/graph/node.h"

namespace jitnodes {

class NodeRegistry {
public:
    using Creator = std::function<std::unique_ptr<Node>()>;

    static NodeRegistry& instance();
    void registerType(const std::string& typeName, Creator creator);
    std::unique_ptr<Node> create(const std::string& typeName) const;
    std::vector<std::string> typeNames() const;

private:
    std::map<std::string, Creator> creators_;
};

void registerBuiltinNodes();

class Network {
public:
    Network();
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    NodeId addNode(const std::string& typeName, const std::string& name = {});
    void removeNode(NodeId id);
    Node& node(NodeId id);
    const Node& node(NodeId id) const;
    NodeId findNode(const std::string& name) const;

    // Throws if the connection would create a cycle or the input index is out of range.
    void connect(NodeId from, NodeId to, int inputIndex = 0);
    void disconnect(NodeId to, int inputIndex);
    NodeId inputOf(NodeId to, int inputIndex) const;

    void markDirty(NodeId id);

    GeometryHandle cook(NodeId id);
    void setDisplayNode(NodeId id) { displayNode_ = id; }
    GeometryHandle cookDisplay() { return cook(displayNode_); }

    // Upstream nodes of id (inclusive) in execution order.
    std::vector<NodeId> executionOrder(NodeId id) const;

    // Chains of point-wise nodes are compiled into one kernel; intermediate nodes of a fused chain
    // are not cooked or cached, and the display node and fan-out points always break a chain.
    void setFusionEnabled(bool enabled) { fusionEnabled_ = enabled; }
    bool fusionEnabled() const { return fusionEnabled_; }
    size_t fusedChainCount() const { return fusedChainCount_; }

    KernelCompiler& compiler() { return compiler_; }
    void setTime(double t);
    double time() const { return time_; }

private:
    struct Slot {
        std::unique_ptr<Node> node;
        std::vector<NodeId> inputs;
    };

    Slot& slot(NodeId id);
    const Slot& slot(NodeId id) const;
    bool reaches(NodeId from, NodeId target) const;
    size_t consumerCount(NodeId id) const;
    GeometryHandle cookPointwiseChain(NodeId id);

    std::map<NodeId, Slot> slots_;
    NodeId nextId_ = 1;
    NodeId displayNode_ = invalidNodeId;
    KernelCompiler compiler_;
    double time_ = 0.0;
    bool fusionEnabled_ = true;
    size_t fusedChainCount_ = 0;
};

}  // namespace jitnodes
