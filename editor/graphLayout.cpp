#include "graphLayout.h"

namespace jitnodes::editor {

namespace {

constexpr float gapX = 30.0f;
constexpr float gapY = 50.0f;

bool overlaps(const Network& net, float x, float y) {
    for (NodeId id : net.nodeIds()) {
        const Node& n = net.node(id);
        if (!n.hasPosition()) continue;
        if (x < n.posX() + nodeWidth + gapX / 2 && n.posX() < x + nodeWidth + gapX / 2 && y < n.posY() + nodeHeight + gapY / 2 &&
            n.posY() < y + nodeHeight + gapY / 2)
            return true;
    }
    return false;
}

void place(Network& net, NodeId id) {
    Node& n = net.node(id);
    if (n.hasPosition()) return;
    float x = 40.0f, y = 40.0f;
    const NodeId parent = n.inputCount() > 0 ? net.inputOf(id, 0) : invalidNodeId;
    if (parent != invalidNodeId) {
        place(net, parent);
        x = net.node(parent).posX();
        y = net.node(parent).posY() + nodeHeight + gapY;
    }
    while (overlaps(net, x, y)) x += nodeWidth + gapX;
    n.setPosition(x, y);
}

}  // namespace

void placeNewNodes(Network& net) {
    for (NodeId id : net.nodeIds()) place(net, id);
    for (NodeId id : net.nodeIds())
        if (Network* child = net.node(id).childNetwork()) placeNewNodes(*child);
}

}  // namespace jitnodes::editor
