#pragma once

#include "jitnodes/jitnodes.h"

namespace jitnodes::editor {

inline constexpr float nodeWidth = 120.0f;
inline constexpr float nodeHeight = 36.0f;

// Gives every node without a position one: below its first input when it has one, otherwise in the top row, moving
// right until it does not overlap another node. Nodes that already have a position are left alone. Subnets are laid out too.
void placeNewNodes(Network& net);

}  // namespace jitnodes::editor
