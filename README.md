# jitnodes

JIT compiled execution nodes: a C++20 procedural node-graph library (Houdini-style cooking) whose per-point
math is compiled to SIMD machine code (SSE2, AVX2 or AVX-512) with [asmjit](https://asmjit.com).

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

## Tools
```
jitnodesCli examples/graphs/wobble.json --time 1.5 --svg wobble.svg --profile
jitnodesCli --asset examples/assets/wobble.asset.json examples/graphs/assetDemo.json --time 1 --svg demo.svg
jitnodesCli --types
jitnodesCli examples/graphs/nebula.json --png nebula.png
```
Graphs are saved with `saveNetwork` / loaded with `loadNetwork` (JSON, JSON5 accepted on load). Subnets
(`subnet`, `subnet2`, ...) contain child networks; `saveAsset` / `registerAsset` turn a subnet into a reusable node type, and `updateAssetInstances` rebuilds existing instances after a definition
changes. `History` provides undo/redo that only touches the nodes that changed.

## More
- [docs/architecture.md](docs/architecture.md): code layout, the node execution (cooking) design with diagrams, the JIT, and performance figures.
- [docs/wrangle.md](docs/wrangle.md): the wrangle scripting language: syntax, types, operators, functions, examples.
- [docs/assets.md](docs/assets.md): assets (reusable nodes): how to write them, use them in a graph, in code and in the editor, and update them.
- [docs/fx.md](docs/fx.md): the point and image nodes (`generate`, `scatter`, `noise`, `warp`, `colorRamp`, `relax`, `attribBlur`, `wave`, `attractor`, `render`) and the nebula sample.
- [docs/editor.md](docs/editor.md): the newui desktop editor: how to use it and how it is built.
- [docs/stress-test.md](docs/stress-test.md): `jitnodesStress`, a test with thousands of nodes and tens of thousands of points per node, and what it found.
- [docs/design.md](docs/design.md): design, research notes and roadmap.
- [docs/performance.md](docs/performance.md): benchmarks with the JIT off and on, method and caveats.
