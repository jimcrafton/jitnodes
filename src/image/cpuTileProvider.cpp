#include "jitnodes/image/cpuTileProvider.h"

#include <algorithm>
#include <cstring>
#include <exception>

#include "jitnodes/core/xxhash.h"

namespace jitnodes {

namespace {

uint64_t mix(uint64_t a, uint64_t b) { return a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2)); }

uint64_t mixCoord(uint64_t h, TileCoord c) {
    return mix(mix(h, static_cast<uint64_t>(static_cast<uint32_t>(c.tx))), static_cast<uint64_t>(static_cast<uint32_t>(c.ty)));
}

uint64_t hashDesc(const ImageDesc& d) {
    uint64_t h = mix(static_cast<uint64_t>(d.channels), static_cast<uint64_t>(d.type));
    for (int v : {d.bounds.x, d.bounds.y, d.bounds.w, d.bounds.h}) h = mix(h, static_cast<uint64_t>(static_cast<uint32_t>(v)));
    for (float f : d.fill) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof bits);
        h = mix(h, bits);
    }
    return h;
}

void fillPixels(float* out, size_t pixels, const ImageDesc& d) {
    const size_t ch = static_cast<size_t>(d.channels);
    for (size_t i = 0; i < pixels; ++i)
        for (size_t c = 0; c < ch; ++c) out[i * ch + c] = d.fill[c];
}

// Copies the part of `clip` that lies in the tile at `coord` into `out`, which is packed with stride dst.w and starts at dst.x, dst.y.
void copyFromTile(const float* tile, TileCoord coord, int tileSize, int channels, const IRect& clip, const IRect& dst, float* out) {
    const IRect region = IRect::intersect(tileRect(coord, tileSize), clip);
    const size_t ch = static_cast<size_t>(channels);
    const size_t ts = static_cast<size_t>(tileSize);
    for (int y = region.y; y < region.bottom(); ++y) {
        const float* src = tile + (static_cast<size_t>(y - coord.ty * tileSize) * ts + static_cast<size_t>(region.x - coord.tx * tileSize)) * ch;
        float* to = out + (static_cast<size_t>(y - dst.y) * static_cast<size_t>(dst.w) + static_cast<size_t>(region.x - dst.x)) * ch;
        std::memcpy(to, src, static_cast<size_t>(region.w) * ch * sizeof(float));
    }
}

}  // namespace

float* CpuTileContext::outputAt(int x, int y) {
    const size_t ch = static_cast<size_t>(outDesc_->channels);
    return output_.data() + (static_cast<size_t>(y - coord_.ty * tileSize_) * static_cast<size_t>(tileSize_) + static_cast<size_t>(x - coord_.tx * tileSize_)) * ch;
}

void CpuTileContext::readInput(int input, const IRect& rect, float* out) const {
    const Input& in = inputs_.at(static_cast<size_t>(input));
    const ImageDesc& d = in.image->desc();
    fillPixels(out, static_cast<size_t>(std::max(rect.w, 0)) * static_cast<size_t>(std::max(rect.h, 0)), d);
    const IRect inside = IRect::intersect(rect, d.bounds);
    const TileRange range = tilesCovering(inside, tileSize_);
    if (range.empty()) return;
    for (int ty = range.first.ty; ty <= range.last.ty; ++ty)
        for (int tx = range.first.tx; tx <= range.last.tx; ++tx) {
            auto it = in.tiles.find(TileCoord{tx, ty});
            if (it == in.tiles.end() || !it->second) continue;
            const auto* tile = static_cast<const CpuTile*>(it->second.get());
            copyFromTile(tile->pixels.data(), {tx, ty}, tileSize_, d.channels, inside, rect, out);
        }
}

CpuTileProvider::CpuTileProvider(int tileSize, size_t maxCachedTiles)
    : tileSize_(std::max(tileSize, 1)), maxCachedTiles_(std::max<size_t>(maxCachedTiles, 1)) {}

void CpuTileProvider::clearCache() {
    cache_.clear();
    order_.clear();
}

void CpuTileProvider::store(uint64_t key, const TileHandle& tile) {
    if (cache_.emplace(key, tile).second) order_.push_back(key);
    while (cache_.size() > maxCachedTiles_ && !order_.empty()) {
        cache_.erase(order_.front());
        order_.pop_front();
    }
}

uint64_t CpuTileProvider::tileVersion(const ImageData& image, TileCoord coord) {
    uint64_t h = mix(hashDesc(image.desc()), static_cast<uint64_t>(tileSize_));
    h = mixCoord(h, coord);
    if (const ImageOp* op = image.op()) {
        h = mix(h, op->hash);
        const IRect out = IRect::intersect(tileRect(coord, tileSize_), image.desc().bounds);
        if (out.empty()) return h;
        for (size_t i = 0; i < op->inputs.size(); ++i) {
            const ImageData& in = *op->inputs[i];
            const IRect need = IRect::intersect(op->evaluator->inputRect(static_cast<int>(i), out), in.desc().bounds);
            const TileRange range = tilesCovering(need, tileSize_);
            if (range.empty()) continue;
            for (int ty = range.first.ty; ty <= range.last.ty; ++ty)
                for (int tx = range.first.tx; tx <= range.last.tx; ++tx) h = mix(h, tileVersion(in, {tx, ty}));
        }
    } else if (image.external()) {
        h = mix(h, reinterpret_cast<uintptr_t>(image.external().get()));
        h = mix(h, image.external()->tileVersion(coord, tileSize_));
    }
    return h;
}

TileResult CpuTileProvider::readExternal(const ImageData& image, TileCoord coord) {
    const ImageDesc& d = image.desc();
    auto tile = std::make_shared<CpuTile>();
    tile->pixels.resize(static_cast<size_t>(tileSize_) * static_cast<size_t>(tileSize_) * static_cast<size_t>(d.channels));
    fillPixels(tile->pixels.data(), static_cast<size_t>(tileSize_) * static_cast<size_t>(tileSize_), d);
    if (!image.external()->readTile(coord, tileSize_, tile->pixels.data())) return {TileStatus::ready, nullptr, {}};
    return {TileStatus::ready, tile, {}};
}

TileResult CpuTileProvider::evaluateOp(const ImageData& image, const ImageOp& op, TileCoord coord) {
    const ImageDesc& d = image.desc();
    CpuTileContext ctx;
    ctx.coord_ = coord;
    ctx.tileSize_ = tileSize_;
    ctx.outRect_ = IRect::intersect(tileRect(coord, tileSize_), d.bounds);
    ctx.outDesc_ = &d;
    ctx.inputs_.resize(op.inputs.size());
    for (size_t i = 0; i < op.inputs.size(); ++i) {
        const ImageData& in = *op.inputs[i];
        ctx.inputs_[i].image = op.inputs[i];
        const IRect need = IRect::intersect(op.evaluator->inputRect(static_cast<int>(i), ctx.outRect_), in.desc().bounds);
        const TileRange range = tilesCovering(need, tileSize_);
        if (range.empty()) continue;
        for (int ty = range.first.ty; ty <= range.last.ty; ++ty)
            for (int tx = range.first.tx; tx <= range.last.tx; ++tx) {
                TileResult r = acquire(in, {tx, ty});
                if (r.status != TileStatus::ready) return r;
                ctx.inputs_[i].tiles[TileCoord{tx, ty}] = std::move(r.tile);
            }
    }
    const size_t pixels = static_cast<size_t>(tileSize_) * static_cast<size_t>(tileSize_);
    ctx.output_.resize(pixels * static_cast<size_t>(d.channels));
    fillPixels(ctx.output_.data(), pixels, d);
    ++evaluations_;
    try {
        op.evaluator->evaluateCpu(ctx);
    } catch (const std::exception& e) {
        return {TileStatus::failed, nullptr, op.type + ": " + e.what()};
    }
    auto tile = std::make_shared<CpuTile>();
    tile->pixels = std::move(ctx.output_);
    return {TileStatus::ready, tile, {}};
}

TileResult CpuTileProvider::acquire(const ImageData& image, TileCoord coord) {
    if (IRect::intersect(tileRect(coord, tileSize_), image.desc().bounds).empty()) return {TileStatus::ready, nullptr, {}};
    if (!image.op() && !image.external()) return {TileStatus::ready, nullptr, {}};

    const uint64_t key = tileVersion(image, coord);
    if (auto it = cache_.find(key); it != cache_.end()) return {TileStatus::ready, it->second, {}};

    TileResult result = image.op() ? evaluateOp(image, *image.op(), coord) : readExternal(image, coord);
    if (result.status == TileStatus::ready) store(key, result.tile);
    return result;
}

bool CpuTileProvider::readRect(const ImageData& image, const IRect& rect, float* out) {
    const ImageDesc& d = image.desc();
    fillPixels(out, static_cast<size_t>(std::max(rect.w, 0)) * static_cast<size_t>(std::max(rect.h, 0)), d);
    const IRect inside = IRect::intersect(rect, d.bounds);
    const TileRange range = tilesCovering(inside, tileSize_);
    if (range.empty()) return true;
    for (int ty = range.first.ty; ty <= range.last.ty; ++ty)
        for (int tx = range.first.tx; tx <= range.last.tx; ++tx) {
            TileResult r = acquire(image, {tx, ty});
            if (r.status != TileStatus::ready) return false;
            if (!r.tile) continue;
            copyFromTile(static_cast<const CpuTile*>(r.tile.get())->pixels.data(), {tx, ty}, tileSize_, d.channels, inside, rect, out);
        }
    return true;
}

}  // namespace jitnodes
