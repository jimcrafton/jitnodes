#pragma once

#include <string>
#include <vector>

#include "jitnodes/graph/network.h"
#include "jitnodes/graph/node.h"

namespace jitnodes {

// A node that contains a network. Its inputs are exposed inside as `subnetInput` nodes (parameter `index`),
// its result is the child network's display node, and parameters added with addSpareFloat ("promoted"
// parameters) are visible inside as `parent.<name>` in expressions. The child shares the parent's kernel
// cache and thread pool and follows its time and fps.
class SubnetNode : public Node {
public:
    SubnetNode(std::string typeName, int inputCount);

    Network& network() { return inner_; }
    const Network& network() const { return inner_; }

    Network* childNetwork() override { return &inner_; }
    const Network* childNetwork() const override { return &inner_; }
    bool childDependsOnTime() const override { return inner_.anyDependsOnTime(); }
    void onAttached(Network& owner) override { inner_.setParent(&owner, id()); }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& inputs) override;

protected:
    // Cooks the child network at `time` with `externals` as its subnetInput geometry. `refresh` recooks every node that reads `parent.`
    // values (a loop node changes them between calls).
    GeometryHandle cookInner(const Network& outer, double time, const std::vector<GeometryHandle>& externals, bool refresh);

private:
    Network inner_{1};
    std::vector<float> lastSpare_;
    bool cooked_ = false;
};

// An instance of a registered asset definition (see serialize.h). The child graph comes from the definition
// and is not saved with the instance.
class AssetNode : public SubnetNode {
public:
    using SubnetNode::SubnetNode;
    bool isAssetInstance() const override { return true; }
    size_t revision() const { return revision_; }
    void setRevision(size_t revision) { revision_ = revision; }
    int version() const { return version_; }  // assetVersion of the definition this instance was built from
    void setVersion(int version) { version_ = version; }

private:
    size_t revision_ = 0;
    int version_ = 1;
};

}  // namespace jitnodes
