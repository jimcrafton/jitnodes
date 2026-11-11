#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "jitnodes/audio/audioStep.h"
#include "jitnodes/geo/geometry.h"
#include "jitnodes/graph/animation.h"
#include "jitnodes/jit/kernel.h"

namespace jitnodes {

using NodeId = uint32_t;
inline constexpr NodeId invalidNodeId = 0;

class Network;

struct CookTimings {
    double compileMs = 0.0;
    double kernelMs = 0.0;
};

struct CookContext {
    KernelCompiler& compiler;
    double time = 0.0;
    ThreadPool* pool = nullptr;      // for data-parallel work inside a node; may be null
    CookTimings* timings = nullptr;  // nodes add their compile/kernel time here; may be null
    Network* network = nullptr;      // the network that is cooking this node
    NodeId node = invalidNodeId;     // the node being cooked (an id of `network`)

    // For a node whose cook takes long (a simulation step loop): `fraction` (0..1) is how far through its own work it is. Reaches the
    // network's progress callback, and, like checkCancelled(), throws CookCancelled when Network::cancel() was called. Cheap; may be called
    // from any thread of the cook. Does nothing without a network.
    void progress(double fraction) const;
    void checkCancelled() const;
};

struct NodeStats {
    size_t cooks = 0;
    double lastCookMs = 0.0;
    double totalCookMs = 0.0;
    double lastCompileMs = 0.0;
    double lastKernelMs = 0.0;
    size_t lastChainLength = 1;
    NodeId fusedInto = 0;  // tail node that cooked this node as part of a fused chain (0 = none)
};

struct PointKernel {
    KernelSpec spec;
    std::vector<float> params;
};

class Node;

// What Node::save and Node::load talk to, so a node does not depend on the file format. standard() writes or reads what every node has
// (position, bypass, freeze, spare names, parameters, expressions, keyframes); number, text and geometry are fields of the node's own, kept
// apart from those. A reader returns false for a field that is not in the file (a file from before the field existed).
class NodeWriter {
public:
    virtual ~NodeWriter() = default;
    virtual void standard(const Node& node) = 0;
    virtual void number(const std::string& name, double value) = 0;
    virtual void text(const std::string& name, const std::string& value) = 0;
    virtual void geometry(const std::string& name, const GeometryHandle& value) = 0;
};

class NodeReader {
public:
    virtual ~NodeReader() = default;
    virtual void standard(Node& node) = 0;
    virtual bool number(const std::string& name, double& value) const = 0;
    virtual bool text(const std::string& name, std::string& value) const = 0;
    virtual bool geometry(const std::string& name, GeometryHandle& value) const = 0;
};

class Node {
public:
    Node(std::string typeName, int inputCount);
    virtual ~Node();

    NodeId id() const { return id_; }
    const std::string& typeName() const { return typeName_; }
    const std::string& name() const { return name_; }
    int inputCount() const { return inputCount_; }

    // Float parameters have a base value and may instead be driven by an expression or keyframes.
    // getFloat returns the value resolved for the time of the latest evaluation (cook).
    void setFloat(const std::string& name, float value);
    float getFloat(const std::string& name) const;
    bool hasFloat(const std::string& name) const { return floats_.count(name) != 0; }

    // Spare parameters are added at run time (promoted subnet parameters, user parameters). They behave
    // like declared ones and are saved with the node.
    void addSpareFloat(const std::string& name, float defaultValue);
    bool isSpare(const std::string& name) const { return spare_.count(name) != 0; }
    std::vector<std::string> spareNames() const { return std::vector<std::string>(spare_.begin(), spare_.end()); }
    float baseFloat(const std::string& name) const;  // the stored value, ignoring any expression or keyframes
    std::vector<std::string> floatNames() const;
    std::vector<std::string> stringNames() const;

    // Parameter tags describe a float parameter the way tags describe a node: each is a plain value or "key:value". The node type adds them
    // where it declares the parameter (tagParam), so they are not saved with the node and never affect cooking. An editor reads them: parameters
    // sharing a "group:<name>" tag belong in one row, "kind:color" with "channel:r|g|b|a" says that row is a colour, "unit:<step>" is how much a
    // drag moves the value per pixel.
    const std::vector<std::string>& paramTags(const std::string& name) const;
    bool hasParamTag(const std::string& name, const std::string& tag) const;
    std::optional<std::string> paramTagValue(const std::string& name, const std::string& key) const;  // as tagValue, for one parameter

    // Expression symbols: own parameter names, time, frame, pi, and node.param references to other
    // nodes by name. Setting an expression replaces keyframes; an empty text clears it. Throws on
    // syntax errors and leaves the parameter unchanged.
    void setExpression(const std::string& name, const std::string& text);
    const std::string& expression(const std::string& name) const;
    void setKeyframes(const std::string& name, std::vector<Keyframe> keys);
    const std::vector<Keyframe>& keyframes(const std::string& name) const;
    void clearAnimation(const std::string& name);
    bool isAnimated(const std::string& name) const;

    void setString(const std::string& name, const std::string& value);
    const std::string& getString(const std::string& name) const;

    // Geometry parameters hold authored geometry (a drawn curve, a point cloud): saved with the node in full, so they take part in undo
    // snapshots, clips and copies like any other parameter. A null handle and an empty geometry are the same, and read back as an empty one.
    void setGeometryParam(const std::string& name, GeometryHandle geometry);
    const GeometryHandle& geometryParam(const std::string& name) const;
    bool hasGeometryParam(const std::string& name) const { return geometries_.count(name) != 0; }
    std::vector<std::string> geometryNames() const;

    // Bypass: the node is skipped. It returns its first input as it is (an empty geometry when that input is not connected) and does no work of
    // its own. Wins over freeze. Saved with the node.
    void setBypassed(bool bypassed);
    bool bypassed() const { return bypassed_; }

    // Freeze: the node returns this geometry and nothing upstream of it is planned or cooked for it, and an edit upstream does not dirty it or
    // what follows it. A null handle unfreezes. The geometry is saved inline with the node. Network::freezeNode cooks the node and freezes the result.
    void setFrozenGeometry(GeometryHandle geometry);
    bool frozen() const { return frozen_ != nullptr; }
    const GeometryHandle& frozenGeometry() const { return frozen_; }

    // Tags label a node. Each is a plain value ("layer") or "key:value" ("color:red"). They are saved with the node and never affect cooking, so
    // changing them does not dirty it. Matching is on the exact text.
    const std::vector<std::string>& tags() const { return tags_; }
    void setTags(std::vector<std::string> tags);
    bool addTag(const std::string& tag);        // false for an empty tag or one already there
    bool removeTag(const std::string& tag);     // false when absent
    bool hasTag(const std::string& tag) const;
    std::optional<std::string> tagValue(const std::string& key) const;  // the text after the first ':' of the first "key:..." tag

    // Editor layout hint, saved with the graph; it never affects cooking and does not dirty the node.
    void setPosition(float x, float y) {
        if (hasPosition_ && posX_ == x && posY_ == y) return;
        posX_ = x;
        posY_ = y;
        hasPosition_ = true;
        touch();
    }
    // Back to "no saved position" (the editor lays the node out itself).
    void clearPosition() {
        if (!hasPosition_) return;
        posX_ = posY_ = 0.0f;
        hasPosition_ = false;
        touch();
    }
    // Changes to a fresh process-wide value whenever anything the node saves changes (also for a different node made under the
    // same id), so a snapshot can reuse the text it wrote for a node whose revision it still sees.
    uint64_t revision() const { return revision_; }
    // A revision no node has had and none will: for recording a change that is not an edit of one node's own data.
    static uint64_t freshRevision();
    bool hasPosition() const { return hasPosition_; }
    float posX() const { return posX_; }
    float posY() const { return posY_; }

    bool dirty() const { return dirty_; }
    size_t cookCount() const { return cookCount_; }
    const NodeStats& stats() const { return stats_; }

    virtual GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& inputs) = 0;

    // Point-wise nodes have one input, keep topology, and only rewrite point channels; the
    // network can fuse chains of them into a single kernel.
    virtual bool isPointwise() const { return false; }
    virtual AttribClass attribClass() const { return AttribClass::point; }
    virtual bool pointKernel(PointKernel&) { return false; }

    // Sequential audio nodes (delay, biquad) have one input and are neither point-wise nor fusable: an AudioProgram runs the returned
    // step between kernel stages. `params` receives the values the step reads each block. cook() runs the same step over the whole input.
    virtual std::unique_ptr<AudioStep> audioStep(std::vector<float>&) { return nullptr; }

    // Merge nodes accept any number of inputs in cook, so the network flattens trees of them into one task.
    virtual bool isMerge() const { return false; }

    // Nodes with variable inputs start with inputCount() inputs and the network keeps exactly one unconnected input after
    // the last connected one; unconnected inputs are skipped when cooking.
    virtual bool hasVariableInputs() const { return false; }
    // For variable-input nodes: cook() also works with no input connected.
    virtual bool inputsOptional() const { return false; }

    // Container nodes (subnets, assets) own a child network.
    virtual Network* childNetwork() { return nullptr; }
    virtual const Network* childNetwork() const { return nullptr; }
    virtual bool isAssetInstance() const { return false; }  // child graph comes from the asset definition, not the file
    virtual bool childDependsOnTime() const { return false; }
    // True for nodes that read the playhead (CookContext::time) themselves, such as a simulation: the network recooks them when the time changes.
    virtual bool dependsOnPlayhead() const { return false; }
    // Asked while planning a cook: false means cook() will not look at its inputs, so they are neither cooked nor required to be connected.
    virtual bool needsInputs() { return true; }
    // A selector node (switch) cooks only one of its inputs. selectedInput() is asked while planning, after the node's parameters have
    // been evaluated for the current time; it returns the input slot to use, and cook() then receives that one geometry.
    virtual bool selectsInput() const { return false; }
    virtual int selectedInput() const { return -1; }
    virtual void onAttached(Network&) {}  // called once the node has an id and owner

    // How the node writes and reads its body: saving, undo snapshots, copy and paste, and loading all go through these. The defaults write and
    // read the standard body only. An override that keeps state of its own writes it with the NodeWriter fields and reads it back in load; it calls
    // Node::save / Node::load for the standard body, and savedStateChanged() whenever that state changes. A subnet's child graph is not part of the
    // body (a snapshot keeps it apart).
    virtual void save(NodeWriter& out) const { out.standard(*this); }
    virtual void load(NodeReader& in) { in.standard(*this); }

protected:
    // For overrides of save and load: a change to the state they write, so a snapshot writes the node out again. It does not dirty the cook.
    void savedStateChanged() { touch(); }
    void declareFloat(const std::string& name, float defaultValue);
    // Adds a tag to a declared float parameter (throws for an unknown one); a repeated tag is ignored.
    void tagParam(const std::string& name, const std::string& tag);
    void declareString(const std::string& name, const std::string& defaultValue);
    void declareGeometry(const std::string& name);
    const Network* owner() const { return network_; }
    // For container nodes that hand a value to the graph inside them (the loop counter, read there as `parent.name`): sets a declared
    // parameter during a cook without marking the node dirty.
    void publishFloat(const std::string& name, float value);

private:
    friend class Network;

    void setInputCount(int count) { inputCount_ = count; }

    // Almost every parameter is a plain number, and nodes are made by the thousand, so the expression and keys live out of line.
    struct FloatParam {
        struct Animation {
            std::string expression;
            KernelSpec expressionSpec;
            std::vector<Keyframe> keys;
        };
        float value = 0.0f;
        float resolved = 0.0f;
        std::vector<std::string> tags;
        std::unique_ptr<Animation> animation;
        bool animated() const { return animation && (!animation->expression.empty() || !animation->keys.empty()); }
        const std::string& expression() const { return animation ? animation->expression : emptyText(); }
        const KernelSpec& expressionSpec() const { return animation ? animation->expressionSpec : emptySpec(); }
        const std::vector<Keyframe>& keys() const { return animation ? animation->keys : emptyKeys(); }
        Animation& edit() {
            if (!animation) animation = std::make_unique<Animation>();
            return *animation;
        }
        void clear() { animation.reset(); }
        static const std::string& emptyText() { static const std::string none; return none; }
        static const KernelSpec& emptySpec() { static const KernelSpec none; return none; }
        static const std::vector<Keyframe>& emptyKeys() { static const std::vector<Keyframe> none; return none; }
    };

    FloatParam& floatParam(const std::string& name);
    const FloatParam& floatParam(const std::string& name) const;
    void rebuildReferences();
    void markChanged(bool structural = true);
    void touch();

    NodeId id_ = invalidNodeId;
    uint64_t revision_;
    std::string typeName_;
    std::string name_;
    int inputCount_ = 0;
    Network* network_ = nullptr;
    std::map<std::string, FloatParam> floats_;
    std::map<std::string, std::string> strings_;
    std::map<std::string, GeometryHandle> geometries_;
    std::set<std::string> referencedNodes_;
    std::set<std::string> spare_;
    std::vector<std::string> tags_;
    bool dirty_ = true;
    bool bypassed_ = false;
    GeometryHandle frozen_;
    bool anchor_ = false;    // a fused chain stops below this node (see Network::noteEdited)
    size_t lastPoints_ = 0;   // points in the latest cook of the node, or of the fused chain it was part of
    float posX_ = 0.0f;
    float posY_ = 0.0f;
    bool hasPosition_ = false;
    size_t cookCount_ = 0;
    NodeStats stats_;
    GeometryHandle cooked_;
};

}  // namespace jitnodes
