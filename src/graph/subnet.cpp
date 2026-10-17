#include "jitnodes/graph/subnet.h"

#include <memory>
#include <stdexcept>

namespace jitnodes {

namespace {

class SubnetInputNode : public Node {
public:
    SubnetInputNode() : Node("subnetInput", 0) { declareFloat("index", 0.0f); }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>&) override {
        if (!ctx.network) throw std::runtime_error("subnetInput cooked outside a network");
        return ctx.network->externalInput(static_cast<int>(getFloat("index")));
    }
};

template <class T, class... Args>
void add(const char* typeName, Args... args) {
    NodeRegistry::instance().registerType(typeName, [=] { return std::make_unique<T>(args...); });
}

}  // namespace

SubnetNode::SubnetNode(std::string typeName, int inputCount) : Node(std::move(typeName), inputCount) {}

GeometryHandle SubnetNode::cook(CookContext& ctx, const std::vector<GeometryHandle>& inputs) {
    if (!ctx.network) throw std::runtime_error("subnet cooked outside a network");
    struct PropagationGuard {
        Network& net;
        bool previous;
        explicit PropagationGuard(Network& n) : net(n), previous(n.setPropagateToParent(false)) {}
        ~PropagationGuard() { net.setPropagateToParent(previous); }
    } guard(inner_);

    inner_.setFps(ctx.network->fps());
    inner_.setTime(ctx.time);
    inner_.setExternalInputs(inputs);

    std::vector<float> spare;
    for (const std::string& name : spareNames()) spare.push_back(getFloat(name));
    if (!cooked_ || spare != lastSpare_) inner_.invalidateReferencesTo("parent");
    lastSpare_ = std::move(spare);
    cooked_ = true;

    const NodeId output = inner_.displayNode();
    if (output == invalidNodeId) throw std::runtime_error(name() + ": the subnet has no output; set a display node inside it");
    return inner_.cook(output);
}

void registerSubnetNodes() {
    add<SubnetInputNode>("subnetInput");
    add<SubnetNode>("subnet0", std::string("subnet0"), 0);
    add<SubnetNode>("subnet", std::string("subnet"), 1);
    add<SubnetNode>("subnet2", std::string("subnet2"), 2);
    add<SubnetNode>("subnet3", std::string("subnet3"), 3);
    add<SubnetNode>("subnet4", std::string("subnet4"), 4);
}

}  // namespace jitnodes
