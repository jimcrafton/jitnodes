#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jitnodes/core/threadPool.h"
#include "jitnodes/graph/node.h"

namespace jitnodes {

class CookCancelled : public std::runtime_error {
public:
    CookCancelled() : std::runtime_error("cook cancelled") {}
};

// Handed to an asset migration hook. `fresh` is built from the current definition and already holds the parameters
// of `old` that exist under the same name (value, expression or keyframes); the hook fixes up the rest: renamed or
// rescaled parameters, new parameters, rewired inputs. `old` carries only the saved parameters (no child graph).
// `inputs` holds the node ids wired to the old inputs, padded or trimmed to the new input count; the hook may
// reorder it (ids must belong to the same network and must not create cycles). Throwing aborts the migration of
// that instance.
struct AssetUpgrade {
    int fromVersion;
    int toVersion;
    const Node& old;
    Node& fresh;
    std::vector<NodeId> inputs;
};
using AssetMigrationFn = std::function<void(AssetUpgrade&)>;

class NodeRegistry {
public:
    using Creator = std::function<std::unique_ptr<Node>()>;

    static NodeRegistry& instance();
    void registerType(const std::string& typeName, Creator creator);
    std::unique_ptr<Node> create(const std::string& typeName) const;
    std::vector<std::string> typeNames() const;
    bool has(const std::string& typeName) const { return creators_.count(typeName) != 0; }

    // Asset types are registered through registerAsset (serialize.h); the definition text is kept here.
    // The revision changes whenever the definition text does; instances remember the revision they were built from.
    void setAssetText(const std::string& typeName, std::string text, int version = 1) {
        Asset& a = assets_[typeName];
        if (a.revision == 0 || a.text != text) ++a.revision;
        a.text = std::move(text);
        a.version = version;
    }
    const std::string* assetText(const std::string& typeName) const {
        auto it = assets_.find(typeName);
        return it == assets_.end() ? nullptr : &it->second.text;
    }
    std::vector<std::string> assetNames() const {
        std::vector<std::string> names;
        for (const auto& entry : assets_) names.push_back(entry.first);
        return names;
    }
    int assetVersion(const std::string& typeName) const {
        auto it = assets_.find(typeName);
        return it == assets_.end() ? 0 : it->second.version;
    }
    size_t assetRevision(const std::string& typeName) const {
        auto it = assets_.find(typeName);
        return it == assets_.end() ? 0 : it->second.revision;
    }

    // Migration hook of an asset type, called when an instance built from an older assetVersion is brought up to
    // date (see updateAssetInstances and loadNetwork). Independent of registerAsset; an empty function removes it.
    void setAssetMigration(const std::string& typeName, AssetMigrationFn fn) {
        if (fn) migrations_[typeName] = std::move(fn);
        else migrations_.erase(typeName);
    }
    const AssetMigrationFn* assetMigration(const std::string& typeName) const {
        auto it = migrations_.find(typeName);
        return it == migrations_.end() ? nullptr : &it->second;
    }

private:
    struct Asset {
        std::string text;
        int version = 1;
        size_t revision = 0;
    };

    std::map<std::string, Creator> creators_;
    std::map<std::string, Asset> assets_;
    std::map<std::string, AssetMigrationFn> migrations_;
};

void registerBuiltinNodes();
void registerTopologyNodes();
void registerSubnetNodes();

class Network {
public:
    // Cooking, saving and undo recurse through nested subnets, so deeper nesting is refused when the subnet node is added.
    static constexpr size_t maxSubnetDepth = 64;

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
    // Swaps the node for a fresh one (same id and name), keeping the wiring that still fits unless `inputs` gives
    // the new wiring. Not allowed while cooking.
    void replaceNode(NodeId id, std::unique_ptr<Node> fresh, const std::vector<NodeId>* inputs = nullptr);
    Node& node(NodeId id);
    const Node& node(NodeId id) const;
    NodeId findNode(const std::string& name) const;
    // A name no node of this network has: `base` itself, or its letters followed by the smallest free number.
    std::string uniqueName(const std::string& base) const;
    // Renames a node. Names are identifiers (letters, digits, underscore; not starting with a digit) and unique in the
    // network. Expressions of this network that refer to `oldName.param` are rewritten; throws std::runtime_error otherwise.
    void renameNode(NodeId id, const std::string& newName);
    // Rewrites the `old.param` expression references of one node according to `renames` (old name to new name, applied
    // together so that swapping names works).
    void rewriteReferences(NodeId id, const std::map<std::string, std::string>& renames);

    // Throws if the connection would create a cycle or the input index is out of range.
    void connect(NodeId from, NodeId to, int inputIndex = 0);
    void disconnect(NodeId to, int inputIndex);
    NodeId inputOf(NodeId to, int inputIndex) const;

    void markDirty(NodeId id);

    // Called by a node when the node names its expressions refer to change.
    void referencesChanged(NodeId id, const std::set<std::string>& before, const std::set<std::string>& after);
    // True when the reverse indexes agree with a scan of the graph (for tests).
    bool indexesConsistent() const;

    GeometryHandle cook(NodeId id);
    void setDisplayNode(NodeId id) {
        displayNode_ = id;
        notifyOwner();
    }
    GeometryHandle cookDisplay() { return cook(displayNode_); }

    // Upstream nodes of id (inclusive) in execution order.
    std::vector<NodeId> executionOrder(NodeId id) const;

    // Chains of point-wise nodes are compiled into one kernel; intermediate nodes of a fused chain
    // are not cooked or cached, and the display node and fan-out points always break a chain.
    void setFusionEnabled(bool enabled) { fusionEnabled_ = enabled; }
    // A network inside a subnet follows its parent's setting.
    bool fusionEnabled() const { return parent_ ? parent_->fusionEnabled() : fusionEnabled_; }
    size_t fusedChainCount() const { return fusedChainCount_.load(); }

    // Independent branches and large kernels run on a pool of `threads` threads in total (1 = single
    // threaded, the caller only). Cooking plans the dirty nodes, groups them by dependency level and runs
    // each level in parallel; results do not depend on the thread count.
    void setThreadCount(size_t threads);
    size_t threadCount() const { return pool()->concurrency(); }

    // Callable from any thread: makes the cook in progress throw CookCancelled before it starts its next node.
    // Nodes that already finished keep their results; ignored when no cook is running.
    void cancel();

    // Per-node timings of the latest cooks; see Node::stats().
    std::string profileReport() const;
    void resetProfile();

    // A network owned by a subnet node shares its parent's kernel cache and thread pool.
    KernelCompiler& compiler() { return parent_ ? parent_->compiler() : ownCompiler_; }
    const KernelCompiler& compiler() const { return parent_ ? parent_->compiler() : ownCompiler_; }
    ThreadPool* pool() const { return parent_ ? parent_->pool() : pool_.get(); }
    Network* parent() const { return parent_; }
    NodeId ownerNode() const { return owner_; }
    void setParent(Network* parent, NodeId owner) {
        parent_ = parent;
        owner_ = owner;
    }
    // Edits inside a child network dirty the owning subnet node. The subnet turns this off while it updates its
    // own child during a cook. Returns the previous setting.
    bool setPropagateToParent(bool enabled) {
        const bool previous = propagateToParent_;
        propagateToParent_ = enabled;
        return previous;
    }

    // Geometry fed to `subnetInput` nodes of this network (set by the owning subnet before it cooks).
    void setExternalInputs(const std::vector<GeometryHandle>& inputs);
    GeometryHandle externalInput(int index) const;
    // Marks every node whose expressions reference `nodeName` (e.g. "parent") dirty.
    void invalidateReferencesTo(const std::string& nodeName);
    bool anyDependsOnTime() const;
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
    void fitInputs(Slot& s);
    size_t consumerCount(NodeId id) const;
    void addEdge(NodeId from, NodeId to) { consumers_[from].push_back(to); }
    void removeEdge(NodeId from, NodeId to);
    void indexReferences(const Node& n, bool add);
    void markAllDirty();
    struct CookTask {
        NodeId tail = invalidNodeId;
        std::vector<NodeId> chain;  // fused point-wise chain, tail first (empty for ordinary nodes)
        std::vector<NodeId> inputs;
        std::vector<NodeId> absorbed;  // merge nodes folded into this merge task
        AttribClass cls = AttribClass::point;
        int level = 0;
    };
    CookTask buildTask(NodeId id);
    void planCook(NodeId id, std::map<NodeId, CookTask>& tasks);
    void appendProfileRows(std::string& out, int depth) const;
    void runTask(const CookTask& task);
    void collectMergeInputs(NodeId id, CookTask& task);
    bool cancelRequested() const { return parent_ ? parent_->cancelRequested() : cancelRequested_.load(); }
    void evaluateParams(Node& n);
    float resolveSymbol(NodeId owner, const std::string& symbol);
    bool paramDependsOnTime(NodeId id, const std::string& name, std::set<std::pair<NodeId, std::string>>& visiting) const;

    // Expressions are evaluated recursively; a longer chain of parameters that refer to each other is an error instead of a stack overflow.
    static constexpr size_t maxExpressionDepth = 400;

    std::map<NodeId, Slot> slots_;
    // Reverse indexes, kept in step with every change of wiring and of expression references: consumers_ holds one entry per wired
    // input (so a node wired twice into one consumer appears twice), referrers_ the nodes whose expressions mention a node name.
    std::unordered_map<NodeId, std::vector<NodeId>> consumers_;
    std::unordered_map<std::string, std::set<NodeId>> referrers_;
    std::unordered_map<std::string, NodeId> names_;
    mutable std::unordered_map<std::string, size_t> suffixHint_;  // uniqueName: every number below the hint is taken for that stem
    void forgetName(const std::string& name);
    NodeId nextId_ = 1;
    NodeId displayNode_ = invalidNodeId;
    KernelCompiler ownCompiler_;
    void notifyOwner() {
        if (parent_ && propagateToParent_) parent_->markDirty(owner_);
    }

    Network* parent_ = nullptr;
    NodeId owner_ = invalidNodeId;
    bool propagateToParent_ = true;
    std::vector<GeometryHandle> externalInputs_;
    double time_ = 0.0;
    double fps_ = 24.0;
    std::set<std::pair<NodeId, std::string>> evaluating_;
    bool fusionEnabled_ = true;
    std::atomic<size_t> fusedChainCount_{0};
    std::atomic<bool> cooking_{false};
    std::atomic<bool> cancelRequested_{false};
    std::unique_ptr<ThreadPool> pool_;
};

}  // namespace jitnodes
