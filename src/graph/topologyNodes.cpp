#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

#include "jitnodes/geo/topology.h"
#include "jitnodes/graph/network.h"

namespace jitnodes {

namespace {

AttribClass classFromString(const std::string& s) {
    if (s == "point") return AttribClass::point;
    if (s == "primitive") return AttribClass::primitive;
    throw std::runtime_error("attribute class must be 'point' or 'primitive', got '" + s + "'");
}

class CircleNode : public Node {
public:
    CircleNode() : Node("circle", 0) {
        declareFloat("count", 32.0f);
        declareFloat("radius", 1.0f);
        declareFloat("cx", 0.0f);
        declareFloat("cy", 0.0f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>&) override {
        auto geo = std::make_shared<Geometry>();
        const size_t n = static_cast<size_t>(std::max(getFloat("count"), 0.0f));
        geo->resizePoints(n);
        float* x = geo->x();
        float* y = geo->y();
        for (size_t i = 0; i < n; ++i) {
            const float a = 6.28318530718f * static_cast<float>(i) / static_cast<float>(n);
            x[i] = getFloat("cx") + getFloat("radius") * std::cos(a);
            y[i] = getFloat("cy") + getFloat("radius") * std::sin(a);
        }
        if (n) geo->addPrimitive({0, static_cast<uint32_t>(n), true});
        return geo;
    }
};

class ResampleNode : public Node {
public:
    ResampleNode() : Node("resample", 1) {
        declareFloat("count", 16.0f);
        declareFloat("spacing", 0.0f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        return std::make_shared<Geometry>(
            resamplePolylines(*in[0], static_cast<size_t>(std::max(getFloat("count"), 0.0f)), getFloat("spacing")));
    }
};

class SubdivideNode : public Node {
public:
    SubdivideNode() : Node("subdivide", 1) { declareFloat("iterations", 1.0f); }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        const int iterations = std::clamp(static_cast<int>(getFloat("iterations")), 0, 10);
        return std::make_shared<Geometry>(subdividePolylines(*in[0], iterations));
    }
};

class FlattenNode : public Node {
public:
    FlattenNode() : Node("flatten", 1) { declareFloat("tolerance", 0.25f); }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        return std::make_shared<Geometry>(flattenCurves(*in[0], getFloat("tolerance")));
    }
};

class CopyToPointsNode : public Node {
public:
    CopyToPointsNode() : Node("copyToPoints", 2) {
        declareFloat("scale", 1.0f);
        declareFloat("angle", 0.0f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        return std::make_shared<Geometry>(copyToPoints(*in[0], *in[1], getFloat("scale"), getFloat("angle")));
    }
};

class BlastNode : public Node {
public:
    BlastNode() : Node("blast", 1) {
        declareString("group", "");
        declareString("class", "point");
        declareFloat("keepGroup", 0.0f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        return std::make_shared<Geometry>(
            blast(*in[0], classFromString(getString("class")), getString("group"), getFloat("keepGroup") != 0.0f));
    }
};

class MirrorNode : public Node {
public:
    MirrorNode() : Node("mirror", 1) {
        declareString("mode", "mirror");
        declareFloat("originX", 0.0f);
        declareFloat("originY", 0.0f);
        declareFloat("angle", 90.0f);
        declareFloat("folds", 2.0f);
        declareFloat("keepOriginal", 1.0f);
        declareString("vectors", "");
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& in) override {
        SymmetryOptions o;
        const std::string& mode = getString("mode");
        if (mode == "mirror") o.mode = SymmetryMode::mirror;
        else if (mode == "rotate") o.mode = SymmetryMode::rotate;
        else if (mode == "kaleidoscope") o.mode = SymmetryMode::kaleidoscope;
        else throw std::runtime_error("mirror: mode must be 'mirror', 'rotate' or 'kaleidoscope', got '" + mode + "'");
        o.originX = getFloat("originX");
        o.originY = getFloat("originY");
        o.angle = getFloat("angle");
        o.folds = static_cast<int>(std::clamp(getFloat("folds"), 1.0f, 64.0f));
        o.keepOriginal = getFloat("keepOriginal") != 0.0f;
        const std::string& list = getString("vectors");
        for (size_t start = 0; start <= list.size();) {
            size_t end = list.find(',', start);
            if (end == std::string::npos) end = list.size();
            size_t from = start, to = end;
            while (from < to && list[from] == ' ') ++from;
            while (to > from && list[to - 1] == ' ') --to;
            if (to > from) o.vectors.push_back(list.substr(from, to - from));
            start = end + 1;
        }
        return std::make_shared<Geometry>(symmetry(*in[0], o, ctx.pool));
    }
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerTopologyNodes() {
    add<CircleNode>("circle");
    add<ResampleNode>("resample");
    add<SubdivideNode>("subdivide");
    add<FlattenNode>("flatten");
    add<CopyToPointsNode>("copyToPoints");
    add<BlastNode>("blast");
    add<MirrorNode>("mirror");
}

}  // namespace jitnodes
