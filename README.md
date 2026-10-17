# jitnodes

JIT compiled execution nodes: a C++20 procedural node-graph library (Houdini-style cooking) whose per-point
math is compiled to AVX2 machine code with [asmjit](https://asmjit.com).

## Build
```
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release
```
On Windows run from a Visual Studio developer prompt (or open the folder in Visual Studio, which reads
`CMakePresets.json`). asmjit is taken from `3rdparty/asmjit`.

Targets: `jitnodes` (static library), `jitnodesTests`, examples `basicGraph` and `kernelBench`.

## Quick look
```cpp
Network net;
NodeId line = net.addNode("line");
NodeId wr = net.addNode("wrangle");
net.node(wr).setString("code", "x = x * 2; y = y + (x > a ? b : 0);");
net.node(wr).setFloat("a", 5);
net.node(wr).setFloat("b", 100);
net.connect(line, wr);
net.setDisplayNode(wr);
GeometryHandle geo = net.cookDisplay();
```

See [docs/design.md](docs/design.md) for the design, research notes and roadmap.
