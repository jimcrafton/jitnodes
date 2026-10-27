#pragma once

#include <map>
#include <memory>
#include <string>
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
struct NodeSnapshot {
    std::shared_ptr<const std::string> text;  // the node's JSON without its wiring
    uint64_t revision = 0;                    // Node::revision() when the text was written; 0 = never reuse it (container nodes)
    std::string type;
    std::string name;
    std::vector<std::string> spare;
    std::vector<NodeId> inputs;
};

struct GraphSnapshot {
    std::map<NodeId, NodeSnapshot> nodes;
    NodeId display = invalidNodeId;
    double fps = 24.0;
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
