#pragma once

#include <stdexcept>

#include "jitnodes/graph/node.h"
#include "jitnodes/image/imageData.h"

namespace jitnodes {

// Base of nodes whose inputs and output are images. Cooking only builds a recipe (ImageData); pixels are produced later, per
// tile, by a TileProvider. Image nodes are never fused or absorbed into a merge.
class ImageNode : public Node {
public:
    using Node::Node;

    DataKind inputType(int) const override { return DataKind::image; }
    DataKind outputType() const override { return DataKind::image; }

    DataHandle cookData(CookContext& ctx, const std::vector<DataHandle>& inputs) override;

protected:
    // Description of the result given the input descriptions (a blur grows the bounds, a crop replaces them, ...).
    virtual ImageDesc outputDesc(const std::vector<ImageDesc>& inputs) const = 0;
    // Captures the current (resolved) parameters. The evaluator must not refer back to the node.
    virtual std::shared_ptr<const ImageEvaluator> makeEvaluator() const = 0;

    // Everything that influences the pixels besides the parameters and inputs (a captured geometry, say) can be mixed in here.
    virtual uint64_t extraHash() const { return 0; }

private:
    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>&) final {
        throw std::logic_error(name() + " is an image node: it cooks through cookData()");
    }
};

}  // namespace jitnodes
