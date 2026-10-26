#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>

#include "jitnodes/graph/network.h"
#include "jitnodes/jit/exprParser.h"

namespace jitnodes {

namespace {

AttribClass parseAttribClass(const std::string& s) {
    if (s == "point") return AttribClass::point;
    if (s == "primitive") return AttribClass::primitive;
    throw std::runtime_error("attribute class must be 'point' or 'primitive', got '" + s + "'");
}

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
        if (n) geo->addPrimitive({0, static_cast<uint32_t>(n), false});
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
        const AttribClass cls = attribClass();
        const auto compileStart = std::chrono::steady_clock::now();
        auto compiled = ctx.compiler.compileFor(kernel.spec, *geo, cls);
        const auto runStart = std::chrono::steady_clock::now();
        resolveStringParams(kernel.spec, *geo, cls, kernel.params);
        compiled->run(*geo, kernel.params, cls, ctx.pool);
        if (ctx.timings) {
            const auto runEnd = std::chrono::steady_clock::now();
            ctx.timings->compileMs += std::chrono::duration<double, std::milli>(runStart - compileStart).count();
            ctx.timings->kernelMs += std::chrono::duration<double, std::milli>(runEnd - runStart).count();
        }
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
        declareString("class", "point");
        for (const char* p : {"a", "b", "c", "d"}) declareFloat(p, 0.0f);
    }

    AttribClass attribClass() const override { return parseAttribClass(getString("class")); }

    bool pointKernel(PointKernel& out) override {
        const std::string& code = getString("code");
        if (!parsed_ || code != parsedCode_) {
            spec_ = parseWrangle(code, {"a", "b", "c", "d"});
            parsedCode_ = code;
            parsed_ = true;
        }
        out.spec = spec_;
        out.params = {getFloat("a"), getFloat("b"), getFloat("c"), getFloat("d")};
        out.params.resize(static_cast<size_t>(out.spec.paramCount), 0.0f);  // the slots after the named ones belong to string literals
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

    bool isMerge() const override { return true; }
    bool hasVariableInputs() const override { return true; }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        std::vector<const Geometry*> parts;
        parts.reserve(in.size());
        for (const auto& g : in) parts.push_back(g.get());
        return std::make_shared<Geometry>(Geometry::concat(parts, ctx.pool));
    }
};

// Passes one of its inputs through. Only the selected input is cooked, so a branch that is switched off costs nothing; `index` is clamped
// to the connected inputs, and the selected input must be connected.
class SwitchNode : public Node {
public:
    SwitchNode() : Node("switch", 2) { declareFloat("index", 0.0f); }

    bool hasVariableInputs() const override { return true; }
    bool selectsInput() const override { return true; }
    int selectedInput() const override {
        const float index = getFloat("index");
        const int last = std::max(inputCount() - 2, 0);
        return std::isnan(index) ? 0 : static_cast<int>(std::clamp(std::floor(index), 0.0f, static_cast<float>(last)));
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override { return in[0]; }
};

class AttribCreateNode : public Node {
public:
    AttribCreateNode() : Node("attribCreate", 1) {
        declareString("name", "attr");
        declareString("class", "point");
        declareString("type", "float");
        declareFloat("value", 0.0f);
        declareString("text", "");
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        auto geo = std::make_shared<Geometry>(*in[0]);
        const std::string& type = getString("type");
        if (type != "float" && type != "int" && type != "string") throw std::runtime_error("attribCreate: type must be 'float', 'int' or 'string'");
        AttribArray& a = geo->attrib(parseAttribClass(getString("class")), getString("name"),
                                     type == "float" ? AttribType::float32 : type == "int" ? AttribType::int32 : AttribType::string);
        const float v = getFloat("value");
        const int32_t id = a.type() == AttribType::string ? a.intern(getString("text")) : 0;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a.type() == AttribType::string) a.ints()[i] = id;
            else if (a.type() == AttribType::int32) a.ints()[i] = static_cast<int32_t>(v);
            else a.floats()[i] = v;
        }
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
    add<SwitchNode>("switch");
    add<AttribCreateNode>("attribCreate");
    add<NullNode>("null");
    registerTopologyNodes();
    registerGenerateNodes();
    registerFxNodes();
    registerPatternNodes();
    registerParticleNodes();
    registerGasNodes();
    registerFieldNodes();
    registerCurveNodes();
    registerShapeNodes();
    registerGenerativeNodes();
    registerFluidNodes();
    registerFileNodes();
    registerStringNodes();
    registerLoopNodes();
    registerSubnetNodes();
}

}  // namespace jitnodes
