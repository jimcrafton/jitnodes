#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "jitnodes/jitnodes.h"

namespace jitnodes::editor {

struct CookResult {
    bool ok = true;
    double ms = 0.0;
    size_t points = 0;
    std::string error;
};

struct ParamInfo {
    std::string name;
    bool isText = false;     // a string parameter (otherwise a float)
    bool multiline = false;  // code-like text that needs more than one line
    std::string text;        // the value, the expression, or for a keyframed float its value at the playhead
    bool expression = false;
    bool keyed = false;
};

struct ProfileRow {
    NodeId id = invalidNodeId;
    int depth = 0;  // 0 for the root graph, 1 inside a subnet, ...
    std::string name;
    std::string type;
    size_t cooks = 0;
    double lastMs = 0.0;
    double totalMs = 0.0;
    std::string note;  // fused chain, fused into, compile time
};

class EditorModel {
public:
    EditorModel();

    // The network being edited: the root graph, or the contents of the subnet the editor has entered.
    Network& network() { return current(); }
    const Network& network() const { return current(); }
    Network& rootNetwork() { return net_; }
    const Network& rootNetwork() const { return net_; }

    // Entering subnets. Only subnets of the current network can be entered (not asset instances, whose contents come from
    // the asset definition). Entering and leaving clear the selection, except that leaving selects the subnet.
    bool canEnter(NodeId id) const;
    std::string enter(NodeId id);  // returns an error message
    bool up();
    size_t depth() const { return subnetPath_.size(); }
    std::string pathText() const;  // "/" or "/subnet1/inner"
    // Assets. A registered asset is a node type that can be added like any other (addNode with its name).
    std::vector<std::string> assetNames() const;
    bool isAssetInstance(NodeId id) const;
    // Registers the assets of a file; instances in the graph built from an older definition are rebuilt (see
    // updateAssetInstances), which resets the undo history. Returns an error message; `updated` gets the number rebuilt.
    std::string loadAssetFile(const std::string& path, size_t* updated = nullptr);
    // Registers every *.asset.json file of a directory (missing directory: nothing happens); returns the first error.
    std::string loadAssetsFrom(const std::string& directory, size_t* loaded = nullptr);
    // Saves the subnet as an asset definition file and registers it as the node type `typeName`. The version is one more
    // than the registered one (1 for a new asset). With `replace` the subnet becomes an instance of the new asset (same
    // name, wiring and promoted parameter values); other instances of an older version are updated. One undo step unless
    // instances were updated, which resets the history. Returns an error message and changes nothing on failure.
    std::string createAsset(NodeId subnet, const std::string& typeName, const std::string& path, bool replace, size_t* updated = nullptr);
    // Turns an asset instance back into an ordinary subnet holding a copy of the contents.
    std::string unlockAsset(NodeId id);

    // Adds a promoted float parameter to a subnet (readable inside as `parent.<name>`). One undo step.
    std::string addSpareParameter(NodeId id, const std::string& name);

    void newGraph();
    void open(const std::string& path);
    void save(const std::string& path);
    const std::string& path() const { return path_; }
    bool modified() const;

    // Adds a node of the given type, wires the selected node into its first input, makes it the display node
    // and selects it. Throws std::runtime_error for an unknown type (the graph is left unchanged).
    NodeId addNode(const std::string& typeName);
    NodeId selected() const;
    void select(NodeId id) { selected_ = id; }
    // Positions are layout hints kept on the nodes; moveNode does not commit (the caller commits when a drag ends).
    void moveNode(NodeId id, float x, float y) { current().node(id).setPosition(x, y); }
    void removeNode(NodeId id);
    void setDisplay(NodeId id);
    void ensurePositions();

    // Parameters of a node as the panel shows them: floats first, then strings. A float's text is its number, its
    // expression, or (keyframed) its value at the playhead.
    std::vector<ParamInfo> parameters(NodeId id);
    // Applies text typed into a parameter field: a number sets the value (and removes an expression or keyframes), empty
    // text removes the animation, anything else becomes an expression; strings are set as they are. Returns an error
    // message and changes nothing when the text is not acceptable; otherwise commits one undo step.
    std::string setParameter(NodeId id, const std::string& name, const std::string& text);
    // fx: turns a float into an expression holding its current value, or an expression back into its value.
    std::string toggleExpression(NodeId id, const std::string& name);
    // The keyframes of a float parameter (empty when it is not keyframed).
    std::vector<Keyframe> keyframes(NodeId id, const std::string& name) const;
    // Replaces a float parameter's keyframes (from the keyframe dialog). No keys removes the animation and keeps the value
    // the parameter had at the playhead. One undo step; returns an error message and changes nothing on failure.
    std::string setKeyframes(NodeId id, const std::string& name, std::vector<Keyframe> keys);
    // key: sets a linear keyframe at the playhead holding the current value (replacing the animation by keyframes).
    std::string keyframeNow(NodeId id, const std::string& name);

    bool canUndo() const { return history_->canUndo(); }
    bool canRedo() const { return history_->canRedo(); }
    bool undo();
    bool redo();
    const std::string& undoLabel() const { return history_->undoLabel(); }
    const std::string& redoLabel() const { return history_->redoLabel(); }
    void commit(const std::string& label) { history_->commit(label); }

    // One row per node (subnet contents follow their subnet, one level deeper) from the cook statistics.
    std::vector<ProfileRow> profile() const;
    void resetProfile() { net_.resetProfile(); }

    CookResult cook();
    const CookResult& lastCook() const { return lastCook_; }
    // Geometry of the display node from the latest successful cook (null when there is none or the cook failed).
    const GeometryHandle& geometry() const { return geometry_; }

    double time() const { return net_.time(); }
    double duration() const { return duration_; }
    void setDuration(double seconds) { duration_ = seconds > 0.1 ? seconds : 0.1; }
    double playheadFraction() const { return net_.time() / duration_; }
    void setPlayheadFraction(double fraction);
    bool playing() const { return playing_; }
    void setPlaying(bool playing) { playing_ = playing; }
    void advance(double seconds);

    std::string statusLine() const;

private:
    void resetHistory();
    void rebaseHistory();
    std::map<std::string, size_t> assetRevisions() const;
    size_t updateChangedAssets(const std::map<std::string, size_t>& before);
    Network& current();
    const Network& current() const;
    void validatePath();

    Network net_;
    std::unique_ptr<History> history_;
    GraphSnapshot saved_;
    std::string path_;
    std::vector<NodeId> subnetPath_;
    NodeId selected_ = invalidNodeId;
    CookResult lastCook_;
    GeometryHandle geometry_;
    double duration_ = 5.0;
    bool playing_ = false;
};

}  // namespace jitnodes::editor
