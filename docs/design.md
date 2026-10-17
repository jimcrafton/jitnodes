# jitnodes design notes

## Goals
A C++20 library for procedural node graphs in the style of SideFX Houdini, whose per-point math is
compiled at runtime to SIMD machine code with [asmjit](https://asmjit.com). Targets 2D vector work first;
3D must fit without redesign.

## Research summary

### Houdini concepts we borrow
| Houdini | jitnodes |
|---|---|
| Node network (SOP context) | `Network` owns `Node`s and their input wires |
| Cooking: a data dependency graph; wires are edges; parameter changes dirty everything downstream; only dirty nodes recook | `Network::markDirty` propagates downstream, `Network::cook` recurses upstream and caches a `GeometryHandle` per node |
| Geometry = points / vertices / primitives / detail, each with named attributes | `Geometry`: SoA point attributes (`x`,`y`, any custom name), primitives as point ranges, detail values |
| Attribute Wrangle / VEX compiled to machine code | `parseWrangle` -> `KernelSpec` -> `KernelCompiler` (asmjit) |
| Parameters evaluated at cook time (sliders do not rebuild the network) | Runtime parameter block read by compiled kernels; no recompile when a value changes |
| Extra/implicit inputs (`addExtraInput`) | Not yet; planned for nodes that read other nodes through parameters |
| Node type registry (`OP_OperatorTable`), HDAs, subnets | `NodeRegistry` now; subnets/assets later |
| Display / render flags | `Network::setDisplayNode` |

Sources: SideFX HDK "OP basics: dependencies"
(https://www.sidefx.com/docs/hdk/_h_d_k__op_basics__overview__dependencies.html) and SideFX forum
discussion of cook/dirty behaviour.

### asmjit
- Vendored at `3rdparty/asmjit` (v1.21), built as a static library from our CMake.
- v1.21 uses **snake_case** API (`add_func`, `new_ymm_ps`, `set_arg`); the older camelCase examples in
  `jit-node-exec.docx` do not compile against it.
- Use `x86::Compiler` (register allocator) and **always** call `func->frame().set_avx_enabled()` when emitting
  AVX: otherwise spills of ymm registers use legacy `movaps` and silently keep only 128 bits (this caused
  nondeterministic wrong results in large kernels before it was fixed).
- Windows x64: asmjit saves xmm6-xmm15; `set_avx_cleanup()` emits `vzeroupper`.
- UJIT (`asmjit/ujit`) is an experimental portable backend; worth evaluating for AArch64.
- `JITNODES_JIT_LOG=1` in the environment dumps generated assembly to stderr.

### Notes on `jit-node-exec.docx`
Ideas adopted: SoA data with 32-byte alignment and lane padding (no tail loop), branchless masks
(`vcmpps` + `vandps`/`vblendvps`), a parameter block so sliders do not recompile, loop fusion,
caching compiled kernels, scratch arena, topological ordering, dirty flags.

Problems in its sample code that we avoided: `pair.key()` and `m_aligned_free` do not exist; embedding
host pointers with `x86::ptr(&param)` is not valid 64-bit addressing; Windows needs `_aligned_malloc`;
the NOT idiom with `vandnps` was wrong; camelCase asmjit API is outdated.

## Architecture (current)
```
Network -> Node::cook(CookContext, inputs) -> GeometryHandle (immutable, shared)
                |
         parseWrangle(text) -> KernelSpec (hash-consed ExprGraph + channel list)
                |
         KernelCompiler::compile (cache by structure) -> Kernel
                |-- AVX2 JIT (8 points per iteration, values stay in registers)
                `-- scalar interpreter (non-AVX2 CPUs, reference for tests)
```
- Layers: `core` (aligned memory, arena), `geo`, `graph`, `jit`.
- Kernel ABI: `void(float* const* channels, const float* params, const float* consts, size_t paddedCount)`.
- Masks are all-ones/all-zeros lanes; `select` maps to `vblendvps`.
- Dead-code elimination: only expression nodes that reach a store are emitted. Constants and parameters are
  broadcast once before the loop.

## Attributes
- Classes: `AttribClass::point` and `AttribClass::primitive` (plus float-only detail values). Each class has its
  own named, SIMD-padded arrays; `Geometry::addPrimitive` keeps primitive arrays sized with the primitive list.
- Types: `AttribType::float32` and `int32` (`AttribArray`). Kernels compute in float; int channels convert at the
  load (`vcvtdq2ps`) and store (`vcvttps2dq`, truncation toward zero, out-of-range gives INT_MIN), so integers are
  exact up to 2^24. Channel types are part of the kernel cache key and come from the geometry via
  `KernelCompiler::compileFor`; running a kernel against an attribute of another type throws.
- Vector attributes are float components named `N.x`, `N.y`, ... (`Geometry::addVectorAttrib`); the wrangle
  language accepts dotted names. Groups are int attributes named `group.<name>` (0/1), so wrangles read and write
  them directly (`group.top = y > a;`). Assigning a comparison stores 1/0.
- Wrangle nodes take `class` = `point` or `primitive`; a primitive wrangle runs over the primitive arrays.
  Fusion only joins point-wise nodes of the same class. `attribCreate` adds a typed, constant filled attribute.
- `Geometry::append` (merge) unions attributes of both classes and throws when an attribute has different types.

## Threading and profiling
- `ThreadPool::parallelFor(count, fn)`: the caller takes part and idle workers help, so it can be called from inside a
  pool task (nested parallelism) and with zero workers (runs inline). The first exception is rethrown once all indices
  have finished. `Network::setThreadCount(n)` sets total threads (1 = caller only; default is hardware threads - 1,
  capped at 8).
- Data parallelism: `Kernel::run(..., pool)` splits ranges of at least 32768 padded elements into chunks that are
  multiples of 8 lanes (so every chunk stays 32-byte aligned) and runs the same compiled function on shifted channel
  pointers. Results are bit-identical to a serial run (tested).
- Task parallelism: `Network::cook` plans the dirty subgraph (one `CookTask` per node or fused chain), evaluates
  animated parameters serially, then runs the tasks level by level (level = longest dependency path), each level with
  `parallelFor`. Independent branches therefore cook concurrently; results do not depend on the thread count.
- Rules for node authors: `cook` may run concurrently with other nodes' `cook`; touch only your own node state, treat
  input geometry as immutable, and use `ctx.pool` for data-parallel work. `KernelCompiler::compile` is internally
  locked; compiled kernels are immutable and shareable.
- Profiling: every cook records `NodeStats` (cooks, last/total cook ms, compile ms, kernel ms, fused chain length, and
  `fusedInto` for interior chain nodes); `Network::profileReport()` prints a table plus total kernel compile time and
  thread count, `resetProfile()` clears it. `KernelCompiler::compileMillis()` is the cumulative JIT time.
- Measured (`kernelBench`, 4M points, one machine, noisy): the kernel is memory-bound, so 4 threads give about 2.3x,
  and more threads than memory bandwidth can feed do not help (8 threads was slower). A network of 8 branches of 1M
  points recooked 60 ms -> 43 ms with 4 threads. The profile shows why it is not better: copying large geometries
  and the merges dominate, and a left-deep chain of merges is entirely serial (use a balanced tree, or an N-ary
  merge once it exists).
- Not covered by tests: a thread sanitizer run (MSVC has none); the suite was stress-run 30 times instead.

## Parameters and animation
A float parameter has a base value and can be driven by exactly one of: an expression or a keyframe curve
(`Node::setExpression`, `Node::setKeyframes`, `Node::clearAnimation`). `Node::getFloat` returns the value resolved
at the latest evaluation; the network evaluates a node's animated parameters just before it cooks (including every
member of a fused chain), so nodes and `pointKernel()` simply call `getFloat`.
- Expressions reuse the wrangle language and IR (a parameter expression is a one-store `KernelSpec`, evaluated by
  `evaluateScalar` with the same `evalOp` semantics as the kernel interpreter). Symbols: own parameter names,
  `time` (seconds), `frame` (`time * fps`, default 24), `pi`, and `nodeName.param` for another node's parameter.
  Functions include min/max/abs/sqrt/clamp/lerp plus `sin cos exp log floor ceil pow`.
- `sin/cos/exp/log/pow` have no vector JIT yet: kernels using them fall back to the interpreter automatically
  (`Kernel::isJitted()` is false). `floor`/`ceil` are JIT-compiled (`vroundps`).
- Keyframes (`Keyframe{time, value, interp}`) support constant, linear and smoothstep segments; values hold outside
  the keyed range. The interpolation mode belongs to the segment starting at that key.
- Dependencies: `Network::markDirty` follows input wires and expression references (`nodeName.param`), so editing a
  referenced parameter recooks the referencing node. `Network::setTime` only invalidates nodes whose parameters
  depend on time (keyframes, `time`/`frame`, or references to such parameters, transitively); everything else keeps
  its cache. Expression cycles and unknown symbols/nodes throw at cook time with the offending name.
- Limits: references use node names, so they break if a node is removed or renamed; per-point time-varying values
  go through a kernel parameter (a scalar per cook), not per-point expressions.

## Topology nodes
Topology nodes are ordinary (non-fused) nodes in `graph/topologyNodes.cpp`; the geometry algorithms live in
`geo/topology.cpp` and are usable without a network. New points are described as `PointSample`s (two source
points and a blend factor) and built by `gatherPoints`, which interpolates float attributes and takes the nearer
point's value for ints (ties take the later point); primitive attributes are carried with `transferPrimAttribs`.
- `circle`: closed polygon generator (count, radius, center).
- `resample`: per primitive, by `count` or by `spacing` (> 0 wins). Open curves keep both endpoints; closed curves
  keep the first point and do not repeat it. Points not owned by a primitive are dropped.
- `subdivide`: Chaikin corner cutting, `iterations` 0..10. Open curves keep their endpoints; closed curves double
  their point count each iteration.
- `copyToPoints`: input 0 is instanced onto every point of input 1 with `scale` and `angle` (radians) multiplied/
  added to optional per-point `scale` and `angle` attributes on the targets. Adds int `copyIndex` on points and
  primitives. The transform is a scalar loop for now; large instance counts could move to a JIT kernel.
- `blast`: deletes a group (`group.<name>`, class point or primitive; `keepGroup` inverts). Deleting primitives
  also deletes their points; deleting points shrinks primitives and drops empty ones.

## Graph fusion
`Node::isPointwise()` / `Node::pointKernel()` let a node expose its `KernelSpec` and current parameter values.
When `Network::cook` reaches a point-wise node it walks upstream while the previous node is point-wise,
dirty (or uncached), has exactly one consumer, and is not the display node. The chain is merged by
`composeKernels` (the later node's loads of a channel are replaced by the earlier node's expression, so
intermediates stay in registers and CSE/dead-code elimination apply across nodes), compiled once, and run on a
single copy of the chain's input geometry. Parameters of all nodes are concatenated into one block, so slider
changes still do not recompile.

Behaviour to know about:
- Only the chain's last node is cooked and cached; interior nodes keep `cookCount() == 0` until cooked directly.
  Editing the tail therefore re-runs the whole fused kernel (cheap) instead of reusing a cached prefix.
- Fan-out, the display node, topology-changing nodes (merge, line, ...) and already-clean cached nodes end a chain.
- `Network::setFusionEnabled(false)` restores node-by-node cooking; tests compare both paths.
- Fixed along the way: `markDirty` no longer stops at nodes that are already dirty-but-uncached, which would have
  skipped downstream invalidation.

## 2D and 3D
Dimension is not baked into the framework: points are a set of named float channels and kernels operate on
whatever channels they reference. 2D data uses `x`,`y`; 3D adds a `z` channel on demand (works today, see the
`threeDimensionalChannelsWork` test). Primitives are index ranges, so polylines are dimension-agnostic.
Remaining 3D work: indexed polygon/triangle primitives (vertex indirection), matrix nodes (3x3 / 4x4 and
2D affine), normal and bounds helpers, and a gather/scatter-aware kernel path for topology-dependent ops.

## Phased plan
1. **Done (this scaffold)**: CMake project, aligned data layer, expression IR + parser, AVX2 JIT + interpreter,
   network with dirty/cook caching and cycle checks, built-in nodes (line, transform, wrangle, merge, null),
   tests and a benchmark.
2. **Graph fusion (done)**: see "Graph fusion" below.
3. **Richer data (first slice done)**: see "Attributes" below. Still open: vertex attributes, indexed 3D primitives,
   string/matrix attribute types.
4. **Topology nodes (first slice done)**: see "Topology nodes" below. Still open: boolean ops, stroke/offset, join,
   sort, per-primitive kernels (segmented loops instead of masks for variable-length work).
5. **Parameters (first slice done)**: see "Parameters and animation" below. Still open: string/int/vector parameters
   and expressions, expression functions with node-graph queries, curve tangents/handles, spare parameters.
6. **Execution (first slice done)**: see "Threading and profiling" below. Still open: scratch-arena temporaries wired into
   cooking, 2D tile/band splitting for raster work, cooking an N-ary merge without repeated copies, cancellation.
7. **Platform**: AArch64/NEON through asmjit, AVX-512 path, SSE2 fallback instead of the interpreter.
8. **Persistence and tooling**: graph serialization (JSON), subnets/assets, plugin SDK for node types,
   undo/redo, optional ImGui front end, optional Blend2D output.

## Conventions
See `CLAUDE.md`: camelCase functions/variables/files, PascalCase types, `_` member postfix, minimal comments.
