#include <algorithm>
#include <memory>

#include "jitnodes/graph/network.h"
#include "jitnodes/jit/exprParser.h"

namespace jitnodes {

namespace {

class LineNode : public Node {
public:
    LineNode() : Node("line", 0) {
        declareFloat("count", 100.0f);
        declareFloat("length", 10.0f);
        declareFloat("y", 0.0f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>&) override {
        auto geo = std::make_shared<Geometry>();
        const size_t n = static_cast<size_t>(std::max(getFloat("count"), 0.0f));
        geo->resizePoints(n);
        const float step = n > 1 ? getFloat("length") / static_cast<float>(n - 1) : 0.0f;
        float* x = geo->x();
        float* y = geo->y();
        for (size_t i = 0; i < n; ++i) {
            x[i] = static_cast<float>(i) * step;
            y[i] = getFloat("y");
        }
        if (n) geo->primitives().push_back({0, static_cast<uint32_t>(n), false});
        return geo;
    }
};

class KernelNode : public Node {
public:
    KernelNode(const char* typeName) : Node(typeName, 1) {}

    bool isPointwise() const override { return true; }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        PointKernel kernel;
        pointKernel(kernel);
        auto geo = std::make_shared<Geometry>(*in[0]);
        ctx.compiler.compile(kernel.spec)->run(*geo, kernel.params);
        return geo;
    }
};

class TransformNode : public KernelNode {
public:
    TransformNode() : KernelNode("transform") {
        declareFloat("sx", 1.0f);
        declareFloat("sy", 1.0f);
        declareFloat("tx", 0.0f);
        declareFloat("ty", 0.0f);
        spec_ = parseWrangle("x = x * sx + tx; y = y * sy + ty;", {"sx", "sy", "tx", "ty"});
    }

    bool pointKernel(PointKernel& out) override {
        out.spec = spec_;
        out.params = {getFloat("sx"), getFloat("sy"), getFloat("tx"), getFloat("ty")};
        return true;
    }

private:
    KernelSpec spec_;
};

class WrangleNode : public KernelNode {
public:
    WrangleNode() : KernelNode("wrangle") {
        declareString("code", "");
        for (const char* p : {"a", "b", "c", "d"}) declareFloat(p, 0.0f);
    }

    bool pointKernel(PointKernel& out) override {
        const std::string& code = getString("code");
        if (!parsed_ || code != parsedCode_) {
            spec_ = parseWrangle(code, {"a", "b", "c", "d"});
            parsedCode_ = code;
            parsed_ = true;
        }
        out.spec = spec_;
        out.params = {getFloat("a"), getFloat("b"), getFloat("c"), getFloat("d")};
        return true;
    }

private:
    KernelSpec spec_;
    std::string parsedCode_;
    bool parsed_ = false;
};

class MergeNode : public Node {
public:
    MergeNode() : Node("merge", 2) {}

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        auto geo = std::make_shared<Geometry>();
        for (const auto& g : in) geo->append(*g);
        return geo;
    }
};

class NullNode : public Node {
public:
    NullNode() : Node("null", 1) {}

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override { return in[0]; }
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerBuiltinNodes() {
    add<LineNode>("line");
    add<TransformNode>("transform");
    add<WrangleNode>("wrangle");
    add<MergeNode>("merge");
    add<NullNode>("null");
}

}  // namespace jitnodes
