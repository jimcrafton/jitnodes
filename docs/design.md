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
3. **Richer data**: vertex/primitive attribute classes, groups, integer and vec2/vec3 attribute types, 3D primitives.
4. **Topology nodes**: subdivide, resample, copy-to-points, boolean ops, stroke; per-primitive kernels
   (segmented loops instead of masks for variable-length work).
5. **Parameters**: expressions, channel references, time dependence and animation, extra inputs, spare parameters.
6. **Execution**: multithreading (band/tile splitting, parallel independent branches), per-node profiling,
   scratch-arena temporaries.
7. **Platform**: AArch64/NEON through asmjit, AVX-512 path, SSE2 fallback instead of the interpreter.
8. **Persistence and tooling**: graph serialization (JSON), subnets/assets, plugin SDK for node types,
   undo/redo, optional ImGui front end, optional Blend2D output.

## Conventions
See `CLAUDE.md`: camelCase functions/variables/files, PascalCase types, `_` member postfix, minimal comments.
