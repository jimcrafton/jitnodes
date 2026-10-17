#include <cstdio>

#include "jitnodes/jitnodes.h"

using namespace jitnodes;

int main() {
    Network net;
    NodeId line = net.addNode("line", "line");
    NodeId wrangle = net.addNode("wrangle", "wrangle");
    NodeId out = net.addNode("null", "out");

    net.node(line).setFloat("count", 8.0f);
    net.node(line).setFloat("length", 7.0f);
    net.node(line).setFloat("y", 10.0f);
    net.node(wrangle).setString("code", "x = x * 2; y = y + (x > a ? b : 0);");
    net.node(wrangle).setFloat("a", 5.0f);
    net.node(wrangle).setFloat("b", 100.0f);

    net.connect(line, wrangle);
    net.connect(wrangle, out);
    net.setDisplayNode(out);

    std::printf("jit supported: %s\n", net.compiler().jitSupported() ? "yes (AVX2)" : "no (interpreter)");
    auto geo = net.cookDisplay();
    for (size_t i = 0; i < geo->pointCount(); ++i) std::printf("[%zu] x=%g y=%g\n", i, geo->x()[i], geo->y()[i]);

    net.node(wrangle).setFloat("a", 9.0f);
    geo = net.cookDisplay();
    std::printf("after a=9: y[5]=%g (kernel compiles: %zu)\n", geo->y()[5], net.compiler().compileCount());
    return 0;
}
