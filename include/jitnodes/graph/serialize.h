#pragma once

#include <string>

#include "jitnodes/graph/network.h"

namespace jitnodes {

// Graphs are stored as JSON (written with quoted keys); loading accepts full JSON5, so hand-edited
// files may contain comments, trailing commas and unquoted keys. Node ids, names, parameter base
// values, expressions, keyframes, wiring, the display node, fps and (optionally) the playhead
// are saved. Cooked geometry and caches are not.
std::string saveNetwork(const Network& net, bool includeTime = true);

// Replaces the contents of net. Throws std::runtime_error describing the problem (with the node
// name where relevant); on failure net is left untouched.
void loadNetwork(Network& net, const std::string& text);

void saveNetworkToFile(const Network& net, const std::string& path);
void loadNetworkFromFile(Network& net, const std::string& path);

}  // namespace jitnodes
