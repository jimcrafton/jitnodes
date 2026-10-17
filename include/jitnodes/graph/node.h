#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "jitnodes/geo/geometry.h"
#include "jitnodes/jit/kernel.h"

namespace jitnodes {

using NodeId = uint32_t;
inline constexpr NodeId invalidNodeId = 0;

class Network;

struct CookContext {
    KernelCompiler& compiler;
    double time = 0.0;
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

    void setFloat(const std::string& name, float value);
    float getFloat(const std::string& name) const;
    void setString(const std::string& name, const std::string& value);
    const std::string& getString(const std::string& name) const;

    bool dirty() const { return dirty_; }
    size_t cookCount() const { return cookCount_; }

    virtual GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& inputs) = 0;

    // Point-wise nodes have one input, keep topology, and only rewrite point channels; the
    // network can fuse chains of them into a single kernel.
    virtual bool isPointwise() const { return false; }
    virtual bool pointKernel(PointKernel&) { return false; }

protected:
    void declareFloat(const std::string& name, float defaultValue);
    void declareString(const std::string& name, const std::string& defaultValue);

private:
    friend class Network;
    void markChanged();

    NodeId id_ = invalidNodeId;
    std::string typeName_;
    std::string name_;
    int inputCount_ = 0;
    Network* network_ = nullptr;
    std::map<std::string, float> floats_;
    std::map<std::string, std::string> strings_;
    bool dirty_ = true;
    size_t cookCount_ = 0;
    GeometryHandle cooked_;
};

}  // namespace jitnodes
