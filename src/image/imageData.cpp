#include "jitnodes/image/imageData.h"

namespace jitnodes {

std::shared_ptr<const ImageData> ImageData::fromOp(ImageDesc desc, ImageOp op) {
    std::shared_ptr<ImageData> d(new ImageData());
    d->desc_ = std::move(desc);
    d->op_ = std::make_shared<const ImageOp>(std::move(op));
    return d;
}

std::shared_ptr<const ImageData> ImageData::fromExternal(std::shared_ptr<ExternalImage> source) {
    std::shared_ptr<ImageData> d(new ImageData());
    d->desc_ = source->desc();
    d->external_ = std::move(source);
    return d;
}

std::shared_ptr<const ImageData> ImageData::empty(const ImageDesc& desc) {
    std::shared_ptr<ImageData> d(new ImageData());
    d->desc_ = desc;
    return d;
}

}  // namespace jitnodes
