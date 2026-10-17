#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "jitnodes/core/threadPool.h"
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
void registerTopologyNodes();

class Network {
public:
    // threads = 0 picks a default from the hardware; see setThreadCount.
    explicit Network(size_t threads = 0);
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    NodeId addNode(const std::string& typeName, const std::string& name = {});
    // Used when restoring saved graphs so node ids stay stable. Throws if the id is invalid or taken.
    NodeId addNodeWithId(NodeId id, const std::string& typeName, const std::string& name);
    void clear();  // removes every node; the kernel cache and thread pool are kept
    std::vector<NodeId> nodeIds() const;
    NodeId displayNode() const { return displayNode_; }
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
    size_t fusedChainCount() const { return fusedChainCount_.load(); }

    // Independent branches and large kernels run on a pool of `threads` threads in total (1 = single
    // threaded, the caller only). Cooking plans the dirty nodes, groups them by dependency level and runs
    // each level in parallel; results do not depend on the thread count.
    void setThreadCount(size_t threads);
    size_t threadCount() const { return pool_->concurrency(); }

    // Per-node timings of the latest cooks; see Node::stats().
    std::string profileReport() const;
    void resetProfile();

    KernelCompiler& compiler() { return compiler_; }
    // Only nodes whose parameters depend on time (keyframes, `time`/`frame` in expressions, or references
    // to such parameters on other nodes) are invalidated when the time changes.
    void setTime(double t);
    double time() const { return time_; }
    void setFps(double fps) { fps_ = fps; }
    double fps() const { return fps_; }

    // Value of a float parameter at the current time (evaluates expressions and keyframes).
    float paramValue(NodeId id, const std::string& name);
    bool dependsOnTime(NodeId id) const;

private:
    struct Slot {
        std::unique_ptr<Node> node;
        std::vector<NodeId> inputs;
    };

    Slot& slot(NodeId id);
    const Slot& slot(NodeId id) const;
    bool reaches(NodeId from, NodeId target) const;
    size_t consumerCount(NodeId id) const;
    struct CookTask {
        NodeId tail = invalidNodeId;
        std::vector<NodeId> chain;  // fused point-wise chain, tail first (empty for ordinary nodes)
        std::vector<NodeId> inputs;
        AttribClass cls = AttribClass::point;
        int level = 0;
    };
    void planCook(NodeId id, std::map<NodeId, CookTask>& tasks);
    void runTask(const CookTask& task);
    void evaluateParams(Node& n);
    float resolveSymbol(NodeId owner, const std::string& symbol);
    bool paramDependsOnTime(NodeId id, const std::string& name, std::set<std::pair<NodeId, std::string>>& visiting) const;

    std::map<NodeId, Slot> slots_;
    NodeId nextId_ = 1;
    NodeId displayNode_ = invalidNodeId;
    KernelCompiler compiler_;
    double time_ = 0.0;
    double fps_ = 24.0;
    std::set<std::pair<NodeId, std::string>> evaluating_;
    bool fusionEnabled_ = true;
    std::atomic<size_t> fusedChainCount_{0};
    std::unique_ptr<ThreadPool> pool_;
};

}  // namespace jitnodes
