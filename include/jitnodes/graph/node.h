#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

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

    // Editor layout hint, saved with the graph; it never affects cooking and does not dirty the node.
    void setPosition(float x, float y) {
        posX_ = x;
        posY_ = y;
        hasPosition_ = true;
    }
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

    // Merge nodes accept any number of inputs in cook, so the network flattens trees of them into one task.
    virtual bool isMerge() const { return false; }

    // Nodes with variable inputs start with inputCount() inputs and the network keeps exactly one unconnected input after
    // the last connected one; unconnected inputs are skipped when cooking.
    virtual bool hasVariableInputs() const { return false; }

    // Container nodes (subnets, assets) own a child network.
    virtual Network* childNetwork() { return nullptr; }
    virtual const Network* childNetwork() const { return nullptr; }
    virtual bool isAssetInstance() const { return false; }  // child graph comes from the asset definition, not the file
    virtual bool childDependsOnTime() const { return false; }
    // True for nodes that read the playhead (CookContext::time) themselves, such as a simulation: the network recooks them when the time changes.
    virtual bool dependsOnPlayhead() const { return false; }
    virtual void onAttached(Network&) {}  // called once the node has an id and owner

protected:
    void declareFloat(const std::string& name, float defaultValue);
    void declareString(const std::string& name, const std::string& defaultValue);
    void declareGeometry(const std::string& name);

private:
    friend class Network;

    void setInputCount(int count) { inputCount_ = count; }

    struct FloatParam {
        float value = 0.0f;
        float resolved = 0.0f;
        std::string expression;
        KernelSpec expressionSpec;
        std::vector<Keyframe> keys;
        bool animated() const { return !expression.empty() || !keys.empty(); }
    };

    FloatParam& floatParam(const std::string& name);
    const FloatParam& floatParam(const std::string& name) const;
    void rebuildReferences();
    void markChanged();

    NodeId id_ = invalidNodeId;
    std::string typeName_;
    std::string name_;
    int inputCount_ = 0;
    Network* network_ = nullptr;
    std::map<std::string, FloatParam> floats_;
    std::map<std::string, std::string> strings_;
    std::map<std::string, GeometryHandle> geometries_;
    std::set<std::string> referencedNodes_;
    std::set<std::string> spare_;
    bool dirty_ = true;
    float posX_ = 0.0f;
    float posY_ = 0.0f;
    bool hasPosition_ = false;
    size_t cookCount_ = 0;
    NodeStats stats_;
    GeometryHandle cooked_;
};

}  // namespace jitnodes
