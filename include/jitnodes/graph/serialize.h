#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "jitnodes/graph/network.h"

namespace jitnodes {

// Graphs are stored as JSON (written with quoted keys); loading accepts full JSON5, so hand-edited
// files may contain comments, trailing commas and unquoted keys. Node ids, names, parameter base
// values, expressions, keyframes, spare parameters, wiring, the display node, fps and (optionally) the
// playhead are saved, and subnets store their child graph inline. Cooked geometry and caches are not
// saved. Asset definitions used by the graph are embedded so the file is self-contained.
// compact writes one line without indentation, for graphs embedded in other files.
std::string saveNetwork(const Network& net, bool includeTime = true, bool compact = false);

// Replaces the contents of net. Throws std::runtime_error describing the problem (with the node
// name where relevant); on failure net is left untouched (asset definitions embedded in the file may
// already have been registered).
void loadNetwork(Network& net, const std::string& text);

void saveNetworkToFile(const Network& net, const std::string& path);
void loadNetworkFromFile(Network& net, const std::string& path);

// Assets: a subnet saved as a reusable node type. The definition holds the type name, number of inputs,
// the promoted parameters with their defaults, and the child graph. Registering it adds a node type that
// can be created like any other; instances copy the definition at creation time.
// assetVersion is stored in the definition; when a graph file embeds a newer version of an asset than the
// registered one, the embedded one is registered while loading.
std::string saveAsset(const Network& net, NodeId subnet, const std::string& typeName, int assetVersion = 1);
void saveAssetToFile(const Network& net, NodeId subnet, const std::string& typeName, const std::string& path, int assetVersion = 1);
// Registers (or replaces) the asset type described by the text. Throws if the name belongs to a built-in type.
// Existing instances keep the definition they were built from until updateAssetInstances is called.
void registerAsset(const std::string& definitionText);
void registerAssetFromFile(const std::string& path);

// Rebuilds the instances (also inside subnets and assets) whose registered definition changed since they were
// created, optionally only for one type. Parameters with the same name keep their value, expression or keyframes,
// parameters that no longer exist are dropped, and wiring that still fits is kept. Returns the number rebuilt.
// Not undoable and not allowed while the network is cooking.
size_t updateAssetInstances(Network& net, const std::string& typeName = {});
size_t outdatedAssetInstances(const Network& net, const std::string& typeName = {});

// Replaces an asset instance by an ordinary subnet (same id, name and wiring) holding a copy of the asset's contents and
// promoted parameters with the instance's values, so the contents can be edited. Throws std::runtime_error when the node is
// not an asset instance or has more than four inputs. Not allowed while the network cooks.
void unlockAssetInstance(Network& net, NodeId id);

// Structural snapshots of a graph's root level for undo: unchanged nodes share their serialized text with the
// previous snapshot, and restoring only touches the nodes that differ, so caches of the others survive. Subnet
// contents are part of the subnet node's text and are reloaded when it changes.
struct GraphSnapshot;

struct NodeMeta {
    std::string type;
    std::string name;
    std::vector<std::string> spare;
    std::vector<NodeId> inputs;
    bool operator==(const NodeMeta& o) const { return type == o.type && name == o.name && spare == o.spare && inputs == o.inputs; }
};

struct NodeSnapshot {
    std::shared_ptr<const std::string> text;  // the node's JSON without its wiring (and without a subnet's child graph, see child)
    uint64_t revision = 0;                    // Node::revision() when the text was written; 0 = never reuse it (asset instances)
    std::shared_ptr<const GraphSnapshot> child;  // a subnet's contents, snapshotted the same way so an edit inside costs what an edit at the root does
    std::shared_ptr<const NodeMeta> meta;     // shared with the previous snapshot while it is unchanged, so copying and comparing stay cheap
};

// The nodes of a snapshot by id, in blocks of 64 ids that snapshots share: copying one copies a pointer per block, writing a node copies
// only its block, and two snapshots can skip every block they hold in common (same pointer) when they are compared or restored.
class SnapshotNodes {
public:
    using Entry = std::pair<NodeId, NodeSnapshot>;
    struct Chunk {
        std::vector<Entry> entries;   // by id, never empty
    };
    using Chunks = std::vector<std::pair<std::uint32_t, std::shared_ptr<Chunk>>>;   // by block number

    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Entry;
        using difference_type = std::ptrdiff_t;
        using pointer = const Entry*;
        using reference = const Entry&;

        const_iterator() = default;
        const_iterator(const Chunks* chunks, std::size_t chunk, std::size_t at) : chunks_(chunks), chunk_(chunk), at_(at) {}
        reference operator*() const { return (*chunks_)[chunk_].second->entries[at_]; }
        pointer operator->() const { return &**this; }
        const_iterator& operator++() {
            if (++at_ >= (*chunks_)[chunk_].second->entries.size()) {
                ++chunk_;
                at_ = 0;
            }
            return *this;
        }
        bool operator==(const const_iterator& o) const { return chunk_ == o.chunk_ && at_ == o.at_; }
        bool operator!=(const const_iterator& o) const { return !(*this == o); }

    private:
        const Chunks* chunks_ = nullptr;
        std::size_t chunk_ = 0;
        std::size_t at_ = 0;
    };

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    const_iterator begin() const { return const_iterator(&chunks_, 0, 0); }
    const_iterator end() const { return const_iterator(&chunks_, chunks_.size(), 0); }
    const Chunks& chunks() const { return chunks_; }

    const_iterator find(NodeId id) const {
        const auto block = findChunk(id >> kBlockBits);
        if (block == chunks_.end() || block->first != (id >> kBlockBits)) return end();
        const auto& entries = block->second->entries;
        const auto at = std::lower_bound(entries.begin(), entries.end(), id, [](const Entry& e, NodeId key) { return e.first < key; });
        if (at == entries.end() || at->first != id) return end();
        return const_iterator(&chunks_, static_cast<std::size_t>(block - chunks_.begin()), static_cast<std::size_t>(at - entries.begin()));
    }
    std::size_t count(NodeId id) const { return find(id) != end() ? 1 : 0; }
    const NodeSnapshot& at(NodeId id) const {
        const const_iterator it = find(id);
        if (it == end()) throw std::out_of_range("SnapshotNodes::at: no such node");
        return it->second;
    }

    void set(NodeId id, NodeSnapshot value) {
        const std::uint32_t key = id >> kBlockBits;
        auto block = std::lower_bound(chunks_.begin(), chunks_.end(), key, [](const Chunks::value_type& c, std::uint32_t k) { return c.first < k; });
        if (block == chunks_.end() || block->first != key) {
            auto fresh = std::make_shared<Chunk>();
            fresh->entries.emplace_back(id, std::move(value));
            chunks_.insert(block, {key, std::move(fresh)});
            ++size_;
            return;
        }
        if (block->second.use_count() != 1) block->second = std::make_shared<Chunk>(*block->second);
        auto& entries = block->second->entries;
        const auto at = std::lower_bound(entries.begin(), entries.end(), id, [](const Entry& e, NodeId k) { return e.first < k; });
        if (at != entries.end() && at->first == id) {
            at->second = std::move(value);
        } else {
            entries.emplace(at, id, std::move(value));
            ++size_;
        }
    }

private:
    static constexpr unsigned kBlockBits = 6;

    Chunks::const_iterator findChunk(std::uint32_t key) const {
        return std::lower_bound(chunks_.begin(), chunks_.end(), key, [](const Chunks::value_type& c, std::uint32_t k) { return c.first < k; });
    }

    Chunks chunks_;
    std::size_t size_ = 0;
};

struct GraphSnapshot {
    SnapshotNodes nodes;
    NodeId display = invalidNodeId;
    double fps = 24.0;
    // What the network looked like when this was taken, so the next snapshot can start from it and redo only the touched nodes.
    std::uint64_t topology = 0;
    std::uint64_t touchMark = 0;
    bool incremental = false;
    std::vector<NodeId> volatileNodes;  // subnets and asset instances: their contents change without touching the node
};

struct RestoreStats {
    size_t added = 0;
    size_t removed = 0;
    size_t rebuilt = 0;  // type, name or spare parameters changed: removed and created again
    size_t updated = 0;  // parameters or child graph changed in place
    size_t rewired = 0;
};

// A node copied out of a network (parameters, expressions, keyframes, spare parameters and a subnet's contents) for
// pasting into the same or another network. The inputs are kept as the names of the nodes they came from.
struct NodeClip {
    std::string type;
    std::string name;
    std::shared_ptr<const std::string> text;
    std::vector<std::string> inputNames;  // empty string for an unconnected input
};

NodeClip copyNode(const Network& net, NodeId id);
// Clips as text (a JSON document with format "jitnodes-nodes") for the system clipboard or a file. clipsFromText returns
// no clips for text that is not such a document (so arbitrary clipboard text is ignored) and throws std::runtime_error for
// one that is but is corrupt. Asset definitions are not included: pasting an instance of an asset the receiving side has not
// registered fails in pasteNodes.
std::string clipsToText(const std::vector<NodeClip>& clips);
std::vector<NodeClip> clipsFromText(const std::string& text);
// Adds copies with fresh ids and unique names (a clip's name when free). Inputs coming from another clip of the same call
// connect to its copy (and expressions that refer to it follow its new name); other inputs connect to the node of that
// name in net when there is one. Throws std::runtime_error for an unknown type or corrupt clip, leaving net unchanged.
std::vector<NodeId> pasteNodes(Network& net, const std::vector<NodeClip>& clips);
inline NodeId pasteNode(Network& net, const NodeClip& clip) { return pasteNodes(net, {clip}).front(); }

GraphSnapshot snapshotNetwork(const Network& net, const GraphSnapshot* previous = nullptr);
bool sameGraph(const GraphSnapshot& a, const GraphSnapshot& b);
// Makes net match target, starting from `current` (a snapshot of net's present state; taken when null).
RestoreStats restoreSnapshot(Network& net, const GraphSnapshot& target, const GraphSnapshot* current = nullptr);

}  // namespace jitnodes
