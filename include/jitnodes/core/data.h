#pragma once

#include <memory>

namespace jitnodes {

// Base of everything a node can produce. Results are immutable once cooked.
// image is reserved for port typing; the Image payload class arrives with the image cook path.
enum class DataKind { geometry, image };

inline const char* dataKindName(DataKind kind) { return kind == DataKind::image ? "image" : "geometry"; }

class Data {
public:
    virtual ~Data() = default;
    virtual DataKind kind() const = 0;

protected:
    Data() = default;
    Data(const Data&) = default;
    Data& operator=(const Data&) = default;
};

using DataHandle = std::shared_ptr<const Data>;

}  // namespace jitnodes
