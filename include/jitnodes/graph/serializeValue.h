#pragma once

#include <json5/json5.hpp>
#include <json5/json5_builder.hpp>

#include "jitnodes/graph/network.h"

namespace jitnodes {

// A graph as a json5 value instead of text, for files that hold a graph beside other data (curvy's .curvy documents). Same content
// and same rules as saveNetwork / loadNetwork, which are these two functions plus the text step.
//
// saveNetworkValue builds the graph object in `b`'s document and returns it, ready to be stored as a member or array item of the
// caller's own value. The value belongs to the builder's document, so it lives as long as that document does.
// The value holds numbers whole: the stream that writes the document decides how many digits reach the file, so set
// `os << std::setprecision(9)` before json5::to_stream (saveNetwork does) or float parameters and coordinates are rounded to six digits.
// loadNetworkValue replaces the contents of `net` from a value read from a document; it throws std::runtime_error exactly as loadNetwork
// does, and leaves `net` untouched on failure.
json5::value saveNetworkValue(json5::builder& b, const Network& net, bool includeTime = true, bool compact = false);
void loadNetworkValue(Network& net, const json5::value& graph);

}  // namespace jitnodes
