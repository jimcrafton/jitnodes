#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "jitnodes/core/data.h"

namespace jitnodes {

struct IRect {
    int x = 0, y = 0, w = 0, h = 0;

    bool empty() const { return w <= 0 || h <= 0; }
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool operator==(const IRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool operator!=(const IRect& o) const { return !(*this == o); }

    static IRect intersect(const IRect& a, const IRect& b) {
        const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
        const int x1 = std::min(a.right(), b.right()), y1 = std::min(a.bottom(), b.bottom());
        return x1 <= x0 || y1 <= y0 ? IRect{} : IRect{x0, y0, x1 - x0, y1 - y0};
    }
    static IRect unite(const IRect& a, const IRect& b) {
        if (a.empty()) return b;
        if (b.empty()) return a;
        const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
        return {x0, y0, std::max(a.right(), b.right()) - x0, std::max(a.bottom(), b.bottom()) - y0};
    }
    IRect grown(int dx, int dy) const { return empty() ? *this : IRect{x - dx, y - dy, w + 2 * dx, h + 2 * dy}; }
};

// Tile (0, 0) starts at pixel (0, 0); negative pixels belong to negative tiles.
struct TileCoord {
    int tx = 0, ty = 0;
    bool operator==(const TileCoord& o) const { return tx == o.tx && ty == o.ty; }
    bool operator!=(const TileCoord& o) const { return !(*this == o); }
    bool operator<(const TileCoord& o) const { return ty != o.ty ? ty < o.ty : tx < o.tx; }
};

inline int floorDiv(int a, int b) {
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

inline IRect tileRect(const TileCoord& c, int tileSize) { return {c.tx * tileSize, c.ty * tileSize, tileSize, tileSize}; }

// Tiles touched by `rect` as [first, last] inclusive; first > last when the rect is empty.
struct TileRange {
    TileCoord first, last;
    bool empty() const { return first.tx > last.tx || first.ty > last.ty; }
};
inline TileRange tilesCovering(const IRect& rect, int tileSize) {
    if (rect.empty()) return {{0, 0}, {-1, -1}};
    return {{floorDiv(rect.x, tileSize), floorDiv(rect.y, tileSize)}, {floorDiv(rect.right() - 1, tileSize), floorDiv(rect.bottom() - 1, tileSize)}};
}

enum class PixelType : uint8_t { float16, float32 };

struct ImageDesc {
    IRect bounds;  // outside it every pixel equals `fill`
    PixelType type = PixelType::float16;
    int channels = 4;  // 1..4; four channels are premultiplied RGBA
    float fill[4] = {0, 0, 0, 0};
    std::string colorSpace;  // opaque tag, compared for equality by nodes that need it; the host interprets it
};

class ImageData;
class CpuTileContext;

// Parameters of one image node captured when it cooked. Immutable, so tiles can be evaluated later (and on other threads)
// while the node is edited.
class ImageEvaluator {
public:
    virtual ~ImageEvaluator() = default;
    // Pixels of input `input` needed to compute the output rect `out`.
    virtual IRect inputRect(int input, const IRect& out) const = 0;
    // Reference implementation: write the pixels of ctx.outRect() into ctx.output().
    virtual void evaluateCpu(CpuTileContext& ctx) const = 0;
};

struct ImageOp {
    std::string type;  // node type name; hosts register their own kernels under it
    uint64_t hash = 0; // type, parameters and the hashes of the inputs: equal hashes mean the same image
    std::vector<std::shared_ptr<const ImageData>> inputs;
    std::shared_ptr<const ImageEvaluator> evaluator;
};

// Pixels that do not come from a node (a document layer, a file, a constant). Called from the provider's thread only.
class ExternalImage {
public:
    virtual ~ExternalImage() = default;
    virtual ImageDesc desc() const = 0;
    // Changes whenever the pixels of that tile (of the provider's tile size) change.
    virtual uint64_t tileVersion(TileCoord coord, int tileSize) const = 0;
    // Writes tileSize * tileSize * channels floats, row-major, tile-local. False means the tile holds only `fill`.
    virtual bool readTile(TileCoord coord, int tileSize, float* out) const = 0;
};

// The result of cooking an image node: a description and a recipe, never pixels. A TileProvider turns it into tiles.
class ImageData final : public Data {
public:
    static std::shared_ptr<const ImageData> fromOp(ImageDesc desc, ImageOp op);
    static std::shared_ptr<const ImageData> fromExternal(std::shared_ptr<ExternalImage> source);
    static std::shared_ptr<const ImageData> empty(const ImageDesc& desc = ImageDesc());  // every pixel is `fill`

    DataKind kind() const override { return DataKind::image; }
    const ImageDesc& desc() const { return desc_; }
    const ImageOp* op() const { return op_.get(); }
    const std::shared_ptr<ExternalImage>& external() const { return external_; }

private:
    ImageData() = default;

    ImageDesc desc_;
    std::shared_ptr<const ImageOp> op_;
    std::shared_ptr<ExternalImage> external_;
};

using ImageHandle = std::shared_ptr<const ImageData>;

// Null when the handle is empty or holds another kind of data.
inline ImageHandle asImage(const DataHandle& data) {
    return data && data->kind() == DataKind::image ? std::static_pointer_cast<const ImageData>(data) : nullptr;
}

}  // namespace jitnodes
