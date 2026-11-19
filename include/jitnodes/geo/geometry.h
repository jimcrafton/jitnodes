#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "jitnodes/core/alignedArray.h"
#include "jitnodes/core/threadPool.h"
#include "jitnodes/geo/attribute.h"

namespace jitnodes {

// polyline: the primitive's points are its vertices.
// cubic: the points are cubic Bezier control points in SVG order - anchor, out handle, in handle, anchor, ... An open primitive has 3n + 1
// points (n segments); a closed one has 3n, and the last segment returns to the first anchor. Handles are ordinary points, so nodes that move
// points (transform, wrangle, noise) move them with their anchors, and a point attribute on an anchor can ride on its handles too. Nodes that
// only understand polylines flatten curves first (flattenCurves).
// @scriptable
enum class PrimKind : uint8_t { polyline, cubic };

// @scriptable ignore=true
struct Primitive {
    uint32_t start = 0;
    uint32_t count = 0;
    bool closed = false;
    PrimKind kind = PrimKind::polyline;
};

// Segment count of a cubic primitive; 0 when its point count does not fit the layout above.
inline uint32_t cubicSegments(const Primitive& p) {
    if (p.kind != PrimKind::cubic) return 0;
    if (p.closed) return p.count >= 3 && p.count % 3 == 0 ? p.count / 3 : 0;
    return p.count >= 4 && p.count % 3 == 1 ? (p.count - 1) / 3 : 0;
}

// A shape is a maximal run of consecutive primitives with the same non-empty key (the primitive string attribute shapeKeyAttrib). A compound
// path (a letter and its counter) is one shape of several primitives. The empty key is anonymous: each such primitive is a shape of its own,
// so geometry that was never given keys has one shape per primitive. `key` points into the geometry's string table: a range is valid until
// the geometry is modified. See docs/shape-structure.md in curvy.
// @scriptable ignore=true
struct ShapeRange {
    uint32_t first = 0;  // the first primitive
    uint32_t count = 0;  // how many primitives
    std::string_view key;
};

// Reserved name of the key attribute (the `shape.` prefix is reserved, as `group.` is).
inline constexpr const char* shapeKeyAttrib = "shape.key";

// Whether a shape's key is covered by a binding (what a rig names): the binding is the key itself, or a prefix that ends at a level of the key
// (`/` into a subnet, `#` a part of the node's shape, `@` a copy). An empty binding or an anonymous shape matches nothing.
inline bool shapeKeyMatches(std::string_view key, std::string_view binding) {
    if (binding.empty() || key.size() < binding.size() || key.compare(0, binding.size(), binding) != 0) {
        return false;
    }
    if (key.size() == binding.size()) {
        return true;
    }
    const char next = key[binding.size()];
    return next == '/' || next == '#' || next == '@';
}

// A script gets cooked geometry shared with the node cache, so only the const methods are bound.
// @scriptable holder=shared readonly
class Geometry {
public:
    Geometry();

    size_t pointCount() const { return pointCount_; }
    size_t paddedPointCount() const { return padToLanes(pointCount_); }
    void resizePoints(size_t n);

    size_t primitiveCount() const { return primitives_.size(); }
    void addPrimitive(const Primitive& p);
    const std::vector<Primitive>& primitives() const { return primitives_; }

    size_t count(AttribClass cls) const { return cls == AttribClass::point ? pointCount() : primitiveCount(); }
    size_t paddedCount(AttribClass cls) const { return padToLanes(count(cls)); }

    // Creates the attribute (zero filled) when missing; throws if it exists with another type.
    AttribArray& attrib(AttribClass cls, const std::string& name, AttribType type = AttribType::float32);
    // Creates an attribute of the same type as `like` (a string attribute starts with like's table) unless the name exists. Use it, not
    // attrib(..., like.type()), to make an attribute that will receive like's values, so string ids keep their meaning.
    AttribArray& attribLike(AttribClass cls, const std::string& name, const AttribArray& like);
    // Throws when a string attribute holds an id that is not in its table.
    void checkStrings() const;
    const AttribArray* findAttrib(AttribClass cls, const std::string& name) const;
    bool hasAttrib(AttribClass cls, const std::string& name) const { return findAttrib(cls, name) != nullptr; }
    const std::map<std::string, AttribArray>& attribs(AttribClass cls) const;

    // Vector attributes are stored as float components "name.x", "name.y", ... (tupleSize 2..4).
    void addVectorAttrib(AttribClass cls, const std::string& name, int tupleSize);

    // Shapes. Adds `p` and gives it `key`; adding further primitives with the same key right after makes them part of one shape. An empty
    // key adds an ordinary (anonymous) primitive. Nothing is allocated for geometry that never gets a key.
    void addShapePrimitive(const Primitive& p, const std::string& key);
    // Throws std::out_of_range when `prim` is not a primitive. An empty key makes it anonymous again.
    void setShapeKey(size_t prim, const std::string& key);
    // "" when the primitive is anonymous, has no key attribute, or does not exist.
    const std::string& shapeKey(size_t prim) const;
    bool hasShapeKeys() const { return findAttrib(AttribClass::primitive, shapeKeyAttrib) != nullptr; }
    // True when some primitive has no key (a geometry without primitives has none).
    bool hasAnonymousPrimitives() const;
    // Gives every anonymous primitive the key `base` + "#p" + its index, so each becomes a shape of its own with a key that stays the same
    // while the node keeps making the same primitives. Returns how many it stamped.
    size_t stampAnonymousShapes(const std::string& base);
    // The shapes in primitive order: one pass over the key column.
    std::vector<ShapeRange> shapes() const;
    // The shape (an index into shapes()) of every primitive.
    std::vector<uint32_t> shapeOfPrimitive() const;
    // The shape with this key; null when there is none (or for the empty key).
    static const ShapeRange* findShape(const std::vector<ShapeRange>& shapes, std::string_view key);
    // Throws std::runtime_error when a key comes back after another key's run (two shapes with one key, or one shape split up), or a key id is
    // outside its table. For tests and debug checks; a clean geometry is what the nodes that make copies must produce.
    void checkShapes() const;

    // Groups are int attributes named "group.<name>" holding 0/1, so kernels can read and write them.
    AttribArray& createGroup(AttribClass cls, const std::string& name);
    bool inGroup(AttribClass cls, const std::string& name, size_t index) const;

    bool hasPointAttrib(const std::string& name) const { return hasAttrib(AttribClass::point, name); }
    // @scriptable ignore=true
    AlignedArray<float>& pointAttrib(const std::string& name) { return attrib(AttribClass::point, name).floats(); }
    // @scriptable ignore=true
    const AlignedArray<float>* findPointAttrib(const std::string& name) const;

    // @scriptable ignore=true
    float* x() { return pointAttrib("x").data(); }
    // @scriptable ignore=true
    float* y() { return pointAttrib("y").data(); }
    // @scriptable ignore=true
    const float* x() const { return findPointAttrib("x")->data(); }
    // @scriptable ignore=true
    const float* y() const { return findPointAttrib("y")->data(); }

    void setDetail(const std::string& name, float value) { detail_[name] = value; }
    float detail(const std::string& name, float fallback = 0.0f) const;
    const std::map<std::string, float>& details() const { return detail_; }

    // Appends points, primitives and attributes of other; attributes missing on either side are zero filled.
    void append(const Geometry& other);

    // Merges any number of geometries with one allocation per attribute; the copies run on the pool when given.
    static Geometry concat(const std::vector<const Geometry*>& parts, ThreadPool* pool = nullptr);

private:
    std::map<std::string, AttribArray>& attribMap(AttribClass cls) { return cls == AttribClass::point ? pointAttribs_ : primAttribs_; }
    const std::map<std::string, AttribArray>& attribMap(AttribClass cls) const { return cls == AttribClass::point ? pointAttribs_ : primAttribs_; }

    size_t pointCount_ = 0;
    std::map<std::string, AttribArray> pointAttribs_;
    std::map<std::string, AttribArray> primAttribs_;
    std::vector<Primitive> primitives_;
    std::map<std::string, float> detail_;
};

using GeometryHandle = std::shared_ptr<const Geometry>;

// Same points, primitives, attributes (names, types and values) and detail values.
bool sameGeometry(const Geometry& a, const Geometry& b);

}  // namespace jitnodes
