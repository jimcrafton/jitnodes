#pragma once

#include <memory>
#include <string>

#include "jitnodes/image/imageData.h"

namespace jitnodes {

// Backend-defined storage for one evaluation tile (a CPU float buffer, a GPU texture, ...). Immutable once published.
class TileData {
public:
    virtual ~TileData() = default;
};
using TileHandle = std::shared_ptr<const TileData>;

enum class TileStatus : uint8_t { ready, pending, failed };

struct TileResult {
    TileStatus status = TileStatus::pending;
    TileHandle tile;    // null with status ready means every pixel of the tile equals the image's fill
    std::string error;  // set when failed
};

// Hash of everything that decides the pixels of one tile: description, tile size and coordinate, the recipe, and the versions of every input
// tile it reads (through inputRect) or, for an external image, its tileVersion(). Providers use it as their cache key.
uint64_t tileVersionOf(const ImageData& image, TileCoord coord, int tileSize);

// Turns the recipe in an ImageData into pixels, one evaluation tile at a time, and owns the tile cache. All methods are called
// from one thread (the host's).
class TileProvider {
public:
    virtual ~TileProvider() = default;

    // Edge of an evaluation tile in pixels; fixed for the provider's lifetime.
    virtual int tileSize() const = 0;

    // Non-blocking: ready with the tile, pending if work was queued or is in flight (call again after pump()), or failed.
    virtual TileResult acquire(const ImageData& image, TileCoord coord) = 0;

    // Cache key part. Equal versions mean identical pixels; it changes when a parameter, a wire or an external tile it reads changes.
    virtual uint64_t tileVersion(const ImageData& image, TileCoord coord) = 0;

    // Runs queued work for up to budgetMs; true if work remains. Synchronous providers have none.
    virtual bool pump(double budgetMs) = 0;
    // Drops queued work for a superseded recipe.
    virtual void cancel(const ImageData& image) = 0;

    // Copies pixels to `out` (rect.w * rect.h * channels floats, packed, fill outside the image). For tests, export and adapters;
    // may stall on a GPU readback. False if any tile failed.
    virtual bool readRect(const ImageData& image, const IRect& rect, float* out) = 0;
};

}  // namespace jitnodes
