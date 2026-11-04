#include "jitnodes/image/imageNode.h"

#include <stdexcept>

#include "jitnodes/core/xxhash.h"

namespace jitnodes {

namespace {
uint64_t mix(uint64_t a, uint64_t b) { return a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2)); }
}  // namespace

DataHandle ImageNode::cookData(CookContext&, const std::vector<DataHandle>& inputs) {
    ImageOp op;
    op.type = typeName();
    std::vector<ImageDesc> descs;
    uint64_t hash = mix(xxh64(typeName().data(), typeName().size()), paramsHash());
    hash = mix(hash, extraHash());
    for (size_t i = 0; i < inputs.size(); ++i) {
        ImageHandle in = asImage(inputs[i]);
        if (!in) throw std::runtime_error(name() + ": input " + std::to_string(i) + " is not an image");
        const ImageOp* inOp = in->op();
        hash = mix(hash, inOp ? inOp->hash : reinterpret_cast<uintptr_t>(in->external().get()));
        descs.push_back(in->desc());
        op.inputs.push_back(std::move(in));
    }
    op.hash = hash;
    op.evaluator = makeEvaluator();
    return ImageData::fromOp(outputDesc(descs), std::move(op));
}

}  // namespace jitnodes
