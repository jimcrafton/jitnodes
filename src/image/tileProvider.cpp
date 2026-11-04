#include "jitnodes/image/tileProvider.h"

#include <cstring>

#include "jitnodes/core/xxhash.h"

namespace jitnodes {

namespace {

uint64_t mix(uint64_t a, uint64_t b) { return hashCombine(a, b); }

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

}  // namespace

uint64_t tileVersionOf(const ImageData& image, TileCoord coord, int tileSize) {
    uint64_t h = mix(hashDesc(image.desc()), static_cast<uint64_t>(tileSize));
    h = mixCoord(h, coord);
    if (const ImageOp* op = image.op()) {
        h = mix(h, op->hash);
        const IRect out = IRect::intersect(tileRect(coord, tileSize), image.desc().bounds);
        if (out.empty()) return h;
        for (size_t i = 0; i < op->inputs.size(); ++i) {
            const ImageData& in = *op->inputs[i];
            const IRect need = IRect::intersect(op->evaluator->inputRect(static_cast<int>(i), out), in.desc().bounds);
            const TileRange range = tilesCovering(need, tileSize);
            if (range.empty()) continue;
            for (int ty = range.first.ty; ty <= range.last.ty; ++ty)
                for (int tx = range.first.tx; tx <= range.last.tx; ++tx) h = mix(h, tileVersionOf(in, {tx, ty}, tileSize));
        }
    } else if (image.external()) {
        h = mix(h, reinterpret_cast<uintptr_t>(image.external().get()));
        h = mix(h, image.external()->tileVersion(coord, tileSize));
    }
    return h;
}

}  // namespace jitnodes
