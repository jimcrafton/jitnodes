#pragma once

#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "jitnodes/image/tileProvider.h"

namespace jitnodes {

// tileSize * tileSize * channels floats, tile-local row-major.
class CpuTile final : public TileData {
public:
    std::vector<float> pixels;
};

// What a node's evaluateCpu sees: the output tile and its input regions.
class CpuTileContext {
public:
    TileCoord coord() const { return coord_; }
    int tileSize() const { return tileSize_; }
    // Pixels of this tile that lie inside the image bounds; the rest of the tile stays `fill`.
    const IRect& outRect() const { return outRect_; }
    const ImageDesc& outDesc() const { return *outDesc_; }
    const ImageDesc& inputDesc(int input) const { return inputs_.at(static_cast<size_t>(input)).image->desc(); }

    // Output pixels, tile-local, row-major with stride tileSize(), outDesc().channels floats per pixel, prefilled with fill.
    float* output() { return output_.data(); }
    // Pixel (x, y) in document coordinates; outRect() contains it.
    float* outputAt(int x, int y);

    // Copies `rect` of input `input` into `out` (rect.w * rect.h * that input's channels, packed). Pixels outside the input's
    // bounds are its fill. `rect` should lie within what inputRect() declared; other tiles are not available.
    void readInput(int input, const IRect& rect, float* out) const;

private:
    friend class CpuTileProvider;
    struct Input {
        ImageHandle image;
        std::map<TileCoord, TileHandle> tiles;  // null handle = all fill
    };

    TileCoord coord_;
    int tileSize_ = 0;
    IRect outRect_;
    const ImageDesc* outDesc_ = nullptr;
    std::vector<Input> inputs_;
    std::vector<float> output_;
};

// Reference provider: synchronous, float32 storage whatever the desc says, FIFO-evicted cache. Used by tests and tools.
class CpuTileProvider final : public TileProvider {
public:
    explicit CpuTileProvider(int tileSize = 256, size_t maxCachedTiles = 4096);

    int tileSize() const override { return tileSize_; }
    TileResult acquire(const ImageData& image, TileCoord coord) override;
    uint64_t tileVersion(const ImageData& image, TileCoord coord) override;
    bool pump(double) override { return false; }
    void cancel(const ImageData&) override {}
    bool readRect(const ImageData& image, const IRect& rect, float* out) override;

    size_t cachedTiles() const { return cache_.size(); }
    size_t evaluations() const { return evaluations_; }  // tiles computed (cache misses), for tests and profiling
    void clearCache();

private:
    TileResult evaluateOp(const ImageData& image, const ImageOp& op, TileCoord coord);
    TileResult readExternal(const ImageData& image, TileCoord coord);
    void store(uint64_t key, const TileHandle& tile);

    int tileSize_;
    size_t maxCachedTiles_;
    size_t evaluations_ = 0;
    std::unordered_map<uint64_t, TileHandle> cache_;
    std::list<uint64_t> order_;
};

}  // namespace jitnodes
