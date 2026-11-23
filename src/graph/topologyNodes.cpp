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

// Comma separated, spaces around an item dropped, empty items skipped.
std::vector<std::string> splitList(const std::string& list) {
    std::vector<std::string> items;
    for (size_t start = 0; start <= list.size();) {
        size_t end = list.find(',', start);
        if (end == std::string::npos) {
            end = list.size();
        }
        size_t from = start;
        size_t to = end;
        while (from < to && list[from] == ' ') {
            ++from;
        }
        while (to > from && list[to - 1] == ' ') {
            --to;
        }
        if (to > from) {
            items.push_back(list.substr(from, to - from));
        }
        start = end + 1;
    }
    return items;
}

class CircleNode : public Node {
public:
    CircleNode() : Node("circle", 0) {
        declareFloat("count", 32.0f);
        declareFloat("radius", 1.0f);
        declareFloat("cx", 0.0f);
        declareFloat("cy", 0.0f);
        for (const char* length : {"radius", "cx", "cy"}) {
            tagParam(length, "quantity:length");
        }
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>&) override {
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
        if (n) geo->addShapePrimitive({0, static_cast<uint32_t>(n), true}, ctx.shapeKey());
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
        o.vectors = splitList(getString("vectors"));
        return std::make_shared<Geometry>(symmetry(*in[0], o, ctx.pool));
    }
};

// The first rig node: moves, turns and scales only the shapes a binding names, leaving every other shape and every key as it was. Several
// bindings are separated by commas. The turn and scale happen around the pivot (the centre of the bound shapes, or a given point), then the move.
class ShapeTransformNode : public Node {
public:
    ShapeTransformNode() : Node("shapeTransform", 1) {
        declareString("bind", "");
        declareFloat("tx", 0.0f);
        declareFloat("ty", 0.0f);
        declareFloat("angle", 0.0f);
        declareFloat("scaleX", 1.0f);
        declareFloat("scaleY", 1.0f);
        declareString("pivot", "centre");
        declareFloat("pivotX", 0.0f);
        declareFloat("pivotY", 0.0f);
    }

    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>& in) override {
        const std::string& pivotMode = getString("pivot");
        if (pivotMode != "centre" && pivotMode != "point") {
            throw std::runtime_error("shapeTransform: pivot must be 'centre' or 'point', got '" + pivotMode + "'");
        }
        auto geo = std::make_shared<Geometry>(*in[0]);
        const std::vector<std::string> bindings = splitList(getString("bind"));
        if (bindings.empty() || geo->pointCount() == 0) {
            return geo;
        }

        std::vector<char> bound(geo->pointCount(), 0);
        bool any = false;
        for (const ShapeRange& shape : geo->shapes()) {
            const bool matches = std::any_of(bindings.begin(), bindings.end(), [&](const std::string& b) { return shapeKeyMatches(shape.key, b); });
            if (!matches) {
                continue;
            }
            for (uint32_t p = shape.first; p < shape.first + shape.count; ++p) {
                const Primitive& prim = geo->primitives()[p];
                std::fill(bound.begin() + prim.start, bound.begin() + prim.start + prim.count, char(1));
                any = true;
            }
        }
        if (!any) {
            return geo;
        }

        float* x = geo->x();
        float* y = geo->y();
        float px = getFloat("pivotX"), py = getFloat("pivotY");
        if (pivotMode == "centre") {
            float minX = 0, maxX = 0, minY = 0, maxY = 0;
            bool first = true;
            for (size_t i = 0; i < bound.size(); ++i) {
                if (!bound[i]) {
                    continue;
                }
                minX = first ? x[i] : std::min(minX, x[i]);
                maxX = first ? x[i] : std::max(maxX, x[i]);
                minY = first ? y[i] : std::min(minY, y[i]);
                maxY = first ? y[i] : std::max(maxY, y[i]);
                first = false;
            }
            px = 0.5f * (minX + maxX);
            py = 0.5f * (minY + maxY);
        }
        const float a = getFloat("angle") * 0.01745329252f;
        const float c = std::cos(a), s = std::sin(a);
        const float sx = getFloat("scaleX"), sy = getFloat("scaleY"), tx = getFloat("tx"), ty = getFloat("ty");
        for (size_t i = 0; i < bound.size(); ++i) {
            if (!bound[i]) {
                continue;
            }
            const float dx = (x[i] - px) * sx, dy = (y[i] - py) * sy;
            x[i] = px + c * dx - s * dy + tx;
            y[i] = py + s * dx + c * dy + ty;
        }
        return geo;
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
    add<ShapeTransformNode>("shapeTransform");
}

}  // namespace jitnodes
