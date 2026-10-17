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

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerTopologyNodes() {
    add<CircleNode>("circle");
    add<ResampleNode>("resample");
    add<SubdivideNode>("subdivide");
    add<CopyToPointsNode>("copyToPoints");
    add<BlastNode>("blast");
}

}  // namespace jitnodes
