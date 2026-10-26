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
Ideas adopted: SoA data with 64-byte alignment and lane padding (no tail loop), branchless masks
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
                |-- JIT: SSE2 (4 lanes) / AVX2 (8, default) / AVX-512 (16); values stay in registers
                `-- scalar interpreter (non-x86-64 hosts, JIT disabled, reference for tests)
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

## Subnets and assets
- `SubnetNode` (`graph/subnet.h`) is a node that owns a child `Network`. Node types `subnet0`, `subnet` (1 input),
  `subnet2`, `subnet3`, `subnet4` differ only in their number of inputs. Inside, a `subnetInput` node (parameter
  `index`) delivers the geometry wired to that subnet input, and the result of the subnet is the child's display
  node. Cooking is lazy and cached like any other network: the child only recooks nodes affected by changed inputs
  (`Network::setExternalInputs` compares geometry handles), time, or promoted parameters.
- Promoted parameters are spare parameters (`Node::addSpareFloat`, saved with the node, usable on any node). Inside
  the subnet they are read as `parent.<name>` in expressions; `parent` is reserved. Editing one dirties exactly the
  child nodes whose expressions mention `parent`, so the rest of the child stays cached. Subnets nest, and
  `parent.x` may itself be an expression that reads the next level's `parent`.
- A child network shares its owner's kernel cache and thread pool (so identical kernels in different instances
  compile once) and follows its time and fps. `dependsOnTime` looks into children, and `setTime` only dirties the
  subnets that actually depend on time. Editing anything inside a child dirties the owning subnet node
  (`Network::setPropagateToParent` is switched off only while the subnet updates its own child during a cook, so no
  worker thread touches the outer network).
- Saving: a subnet stores its child graph inline (`"graph"`, same shape as the root: fps, display, nodes) and its
  `"spare"` parameter names. Profiles (`profileReport`, `resetProfile`) descend into children, indented.
- A user guide for assets (writing, using, editor workflow, updating) is in [assets.md](assets.md).
- Assets: `saveAsset(net, subnetId, "typeName")` turns a subnet into a definition (`format: "jitnodes-asset"`: type,
  number of inputs, `promote` names with their default values, and the child graph). `registerAsset(text)` /
  `registerAssetFromFile` add it to `NodeRegistry` as a node type with that name (re-registering replaces the
  definition for new instances only; a name that belongs to a built-in type is rejected, and a definition is
  validated by building one instance first). Instances are `AssetNode`s: they copy the definition at creation, do
  not store their child graph in files, and keep independent parameters and caches.
- Graph files embed the definition of every asset they use (`"assets"`, transitive), so a file is self-contained.
  On load, an embedded definition is registered if that type is not registered yet or the embedded `assetVersion`
  is higher than the registered one (otherwise the registered definition wins); registrations are global and are not
  undone if the load later fails. `jitnodesCli --asset
  FILE` preloads library files; `examples/assets/wobble.asset.json` and `examples/graphs/assetDemo.json` show the
  workflow.
- Versions and updates: a definition carries an integer `assetVersion` (`saveAsset(..., assetVersion)`, default 1;
  `NodeRegistry::assetVersion`). Every change of the registered text also bumps an internal revision, and instances
  remember the revision they were built from. `updateAssetInstances(net, typeName = {})` rebuilds the outdated
  instances in place, recursing into subnets and assets: it creates a fresh instance from the current definition,
  copies over every parameter that still exists (base value, expression or keyframes, so values users set are kept
  even when the new default differs), drops parameters that are gone, adds new ones at their defaults, and swaps it in
  with `Network::replaceNode` (same id and name; wiring that still fits is kept, the instance and its consumers
  recook). `outdatedAssetInstances` counts them. Updating is explicit, so a graph never changes under the user; it is
  not undoable and must not run while the network cooks.
- Migration hooks: instances remember the `assetVersion` they were built from (`AssetNode::version`) and save it
  as `"assetVersion"` in graph files. `NodeRegistry::setAssetMigration(type, fn)` installs a hook (an empty function
  removes it; it is independent of `registerAsset`) that receives an `AssetUpgrade`: `fromVersion`, `toVersion`, the
  `old` node (saved parameters only, no child graph), the `fresh` node and the wiring `inputs`. When an instance is
  brought up to date, `fresh` first receives every parameter that exists under the same name, then the hook runs and
  can rename or rescale parameters, set new ones, or reorder `inputs` (padded or trimmed to the new input count).
  The hook runs only when the versions differ, once per instance, in two places: `updateAssetInstances`, and
  `loadNetwork` for an instance saved with an older version than the registered definition. Without a hook
  such a load is lenient: parameters that no longer exist are dropped and a changed input count is padded or
  trimmed (instances saved with the current version still fail on unknown parameters). An exception from the hook
  aborts `updateAssetInstances` with `asset migration of '<name>' (<type>): ...`, leaving that instance and any later
  ones untouched. `loadNetwork` loads the file twice (a validation pass, then the real one), so hooks must be pure
  functions of their `AssetUpgrade`. Chaining steps (1 to 2 to 3) is up to the hook, which switches on `fromVersion`;
  only the latest definition exists, so `old` is never rebuilt from an intermediate version. `History` should be
  recreated after updating assets, since snapshots of nodes with renamed parameters no longer fit.
- Unlocking: `unlockAssetInstance(net, id)` replaces an instance by an ordinary `subnet0`..`subnet4` (same id, name,
  position and wiring) holding a copy of the asset's contents and promoted parameters with the instance's values, so the
  contents become editable and are saved with the graph again. `NodeRegistry::assetNames()` lists the registered assets.
- Not done: per-instance overrides of child nodes, string/int promoted parameters, and `parent.` references to
  string parameters.

## Platform support
- Instruction-set tiers (`jit/isa.h`): `sse2` (4 lanes, the x86-64 baseline, so the JIT works on every x64 CPU),
  `avx2` (8 lanes) and `avx512` (16 lanes, needs AVX512F+DQ). `KernelCompiler` defaults to AVX2 (or SSE2 without
  it); `setIsa()`, `isaSupported()` and `supportedIsas()` select and query tiers, `jitnodesCli --isa` exposes it,
  and the tier is part of the kernel cache key. AVX-512 stays opt-in: on this CPU it is 1.2-1.5x faster than AVX2
  only for compute-heavy kernels, and some Intel parts lower their clock under AVX-512.
- Code structure: `VecEmitter` (`src/jit/vecEmitter.*`, internal) hides the differences between tiers and emits one IR
  op at a time into fresh virtual registers; `Kernel::compileJit` is tier-agnostic. SSE2 uses legacy two-operand
  encodings (copy + op), emulates `blendvps` with and/andn/or, `cmpps` with swapped operands for `>`/`>=`, and
  `floor/ceil` via an int32 round trip (no SSE4.1). AVX-512 builds masks with `vcmpps` + `vpmovm2d` and selects with
  `vpternlogd`, so every tier works with the same all-ones/all-zeros vector masks.
- Padding is 16 floats and alignment 64 bytes for every array, which fits all three tiers and parallel chunking.
- Conditions are real masks: the wrangle parser turns any non-comparison condition (`x ? a : b`, `a && b` on plain
  values) into `value != 0`, so the interpreter and every tier agree (NaN counts as true).
- Vector math (Cephes single-precision polynomials, the same on all tiers; the interpreter uses the C library):
  `sin/cos` are accurate to about 5e-6 absolute for |x| up to a few hundred (error grows with |x|; Cephes range
  reduction is meant for |x| below about 8000); `exp` has about 1e-5 relative error and saturates its argument at
  +-88.37 (results stay finite, no inf); `log` is accurate to about 1e-5 and returns -inf for 0 and NaN for negative
  input; `pow(a, b)` is `exp(b * log(a))`, defined for a > 0 only (a <= 0 gives NaN or 0 where the C library would
  give a real result for integer exponents). Parameter expressions still use the exact C library functions.
- Non-x86-64 hosts: `JitBackend` only enables the JIT on x86-64, so other hosts (including AArch64) run kernels on the
  interpreter. That path is plain C++ but has not been built or run on an ARM machine.
- AArch64/NEON is not implemented. asmjit has an A64 backend and the work is a second `VecEmitter` (4 lanes, NEON has
  native `fmin/fmax/fcm*/bsl`, `frintm/frintp`, `fsqrt` and integer shifts), but there is no ARM hardware or emulator
  available in this environment to validate it, and untested numeric code generation is not worth shipping.

## Persistence and tooling
- Graph files (`graph/serialize.h`): `saveNetwork` writes strict JSON through the vendored json5 library
  (`3rdparty/json5`, P-i-N/json5, MIT, header-only; the same library used by newui). `loadNetwork` accepts full
  JSON5, so hand-edited files may contain comments, trailing commas and unquoted keys
  (`examples/graphs/wobble.json`). The file stores `format`, `version` (currently 1), `fps`, optional `time`,
  `display`, and per node: `id`, `type`, `name`, `floats` (base values), `strings`, optional `expressions` and
  `keyframes`, and `inputs` (node ids, 0 = unconnected). Floats are written as the shortest text that round-trips,
  so save -> load -> save is byte-identical. Cooked geometry is never saved.
- Loading is atomic: the file is first loaded into a scratch network, and the real network is only replaced if that
  succeeds. Errors name the node (`node #2 ('b', null): ...`) and cover unknown types and parameters, duplicate or
  missing ids, cycles, bad expressions, unsupported versions and a missing display node. Node ids are preserved
  (`Network::addNodeWithId`), which keeps references from editors stable.
- json5 quirk found while integrating: `json5::value()` is a zeroed bit pattern, which reads as the number 0, so a
  missing key is not null. The loader therefore checks key presence through `object_view::find` and never inspects
  the value returned by `operator[]` for a missing key.
- Undo/redo (`graph/history.h`): `History::commit(label)` after an edit, `undo()/redo()` with labels, a size limit,
  and redo truncation on a new edit. A state is a `GraphSnapshot` (`serialize.h`): per node its JSON text (without
  wiring), type, name, spare parameter names and input ids, plus the display node and fps; the playhead and asset
  definitions are not included. Unchanged nodes share their text with the previous state (`shared_ptr`), so 30 edits
  of a 60-node graph store about one graph plus 30 node texts (`History::storedBytes`).
  `restoreSnapshot` compares the live graph (always re-snapshotted, so uncommitted edits are discarded) with the target
  and only touches differences: nodes missing from the target are removed; nodes whose type, name or spare set changed
  are removed and created again (`rebuilt`); nodes whose text changed are updated in place (animation cleared, then
  the saved parameters and child graph applied, `updated`); new nodes are added; then wiring is diffed (disconnect
  all differences first, then connect, so intermediate states cannot form cycles), then display node and fps. Every
  node that is not touched, and not downstream of a touched one, keeps its cooked result. Limits: a changed subnet
  reloads its whole child graph (its inner caches are lost), and a failure halfway through a restore, which cannot
  happen for snapshots taken from a live network unless a node type was removed, leaves the network partly restored.
- Plugin registration: custom node types derive from `Node` and are registered with
  `NodeRegistry::instance().registerType(name, creator)`; they then work in networks, fusion planning (if
  point-wise), profiling and save/load (tested with an `offsetX` node). A dynamically loaded plugin ABI is not done.
- Tooling: `geometryToSvg` (`geo/svgExport.h`) draws polylines/polygons with +y up, and `jitnodesCli` loads a graph,
  optionally sets the time/threads, cooks the display node (or `--node NAME`), prints a summary, writes SVG
  (`--svg`, `--points`), re-saves the graph (`--save`), prints the profile (`--profile`) and lists node types
  (`--types`).

## Binary geometry files and other follow-ups from the format research
Source: `best binary format for storing a node graph.docx` (a chat transcript, `D:\code\curvy\docs`). It compares FlatBuffers,
Protocol Buffers, MessagePack and a custom memory-mapped layout, and concludes that for a C++ SIMD engine a custom flat
layout wins: a header, offset tables instead of pointers, string interning, one contiguous float pool per attribute, 64-byte
alignment and an XXHash64 checksum. The rest of the transcript drifts into a runtime arena, a stage scheduler, a profiler and
AVX-512 masks. Its sample code has the same kind of mistakes as the earlier `jit-node-exec.docx` (camelCase asmjit calls, an
invented "XXHash style" function), so the ideas are used and the code is not.

### What the research gives us, and what it does not
| Idea in the research | Verdict | Why |
|---|---|---|
| Custom flat binary layout (header, offset tables, aligned float blocks) | **Adopt, for geometry** | Cooked geometry is the only large float data and it is never saved today, so every slow simulation (boids, reactionDiffusion, a 2M-point `generate`) is recooked at every load. |
| Binary format for the *graph* itself | **Measure first, probably skip** | A graph file holds parameters, not points. It is a few KB to a few hundred KB, hand-editable and diffable as JSON5, and `saveNetwork` is byte-stable. A binary graph would lose all that for no measurable gain unless the 1000-node stress graph shows parse time that matters (step 0). |
| FlatBuffers / Protobuf / MessagePack | **Skip** | New third-party dependency (the project vendors only asmjit and json5), a schema compiler in the build, and none of them gives 64-byte-aligned padded SoA blocks, which is what `AttribArray` already is. The research itself ends with the custom layout. |
| String table, offsets instead of pointers, per-section sizes | **Adopt** | Needed for a relocatable file anyway. |
| XXHash64 checksum and bounds-checked header | **Adopt** | A truncated or corrupt cache must be rejected before any array is trusted. |
| mmap zero-copy loading | **Later, only if measured** | Copying 100M floats takes tens of milliseconds, and zero-copy needs a non-owning `AlignedArray` and a copy-on-write detach. Not worth it before the plain loader exists and is measured. |
| Append-only runtime arena and thread-local arenas | **Skip** | Already decided above ("Scratch-arena temporaries were dropped"): no cook path allocates large temporaries, and attributes are copy-on-write. |
| Kahn-layered parallel cooking | **Already done** (`Network::cook`, level by level) | A refinement is in step 5. |
| Chrome-tracing profiler, per-thread timelines | **Adopt (small)** | We keep per-node totals only; a timeline shows idle gaps in uneven graphs. Step 4. |
| Parameter block, branchless `vcmpps` masks, AVX-512 opmask merge | **Already done** | `Kernel` parameter block; `select` is `vblendvps` on AVX2 and a k-register blend on AVX-512 (`vecEmitter.cpp`). |
| Interned string ids (hashed strings) for string attributes | **Adopt later** | String attribute types are still open under "Attributes"; a per-geometry string table of int ids fits the file format's string table. Step 6. |

### Implementation plan
**Step 0, measure (before any code).** Time `loadNetwork` on the stress graph and on `nebula.json`, and time a cold cook of
`reactionDiffusion` 512 x 512 at t=30, `boids` (900, 14 s) and `generate` 2M points. This decides how much a cache saves and
whether a binary graph is worth discussing; record the numbers in `docs/performance.md`.
*Done 2026-10-26* ("Load and cook" table there): a 2646-node graph loads in 31 ms, so no binary graph format; cold cooks are
`reactionDiffusion` 3.5 s, `boids` 0.24 s, `nebula` 0.18 s, `generate` 2M only 6 ms, so the cache only pays for the simulations.

**Step 1, the file format (`include/jitnodes/geo/geoFile.h`, `src/geo/geoFile.cpp`).** One little-endian file per geometry,
extension `.jngeo`, no dependency:
```
Header (64 bytes): magic "JNGEO1\0\0", u32 formatVersion, u32 endianMarker 0x01020304, u32 flags, u32 sectionCount,
                   u64 fileSize, u64 xxh64 of everything after the header, u64 reserved
Section table:     {u32 id, u32 flags, u64 offset (64-byte aligned), u64 byteSize} per section
Sections:          1 strings   u32 count, then u32 offsets + UTF-8 bytes
                   2 counts    u64 pointCount, u64 primitiveCount
                   3 prims     {u32 start, u32 count, u8 closed, u8 kind, pad} per primitive
                   4 attribs   directory {u32 nameIndex, u8 class, u8 type, u64 count, u64 dataOffset, u64 dataBytes}
                   5 details   {u32 nameIndex, f32 value}
                   6 payload   each attribute exactly as stored in memory: padToLanes(count) elements, zero padded, 64-byte aligned
```
*Done 2026-10-26* as written, with one payload section (directory offsets relative to it); exact layout and checks in
`geometryFiles.md`. `xxh64` is the public header `core/xxhash.h` so tests can recompute the checksum.
Rules: every offset and size is checked against `fileSize` (and against hard caps for counts) before use; unknown section ids are
skipped, an unknown `formatVersion` major is an error with a clear message; floats are copied bit for bit (NaN payloads and
-0 included). `saveGeometry(const Geometry&) -> std::vector<uint8_t>` and `loadGeometry(const uint8_t*, size_t) -> Geometry`,
plus `...File` variants. The loader fills `AlignedArray` storage with one `memcpy` per attribute, so attribute loading runs at
memory bandwidth. XXH64 is implemented in `src/core/xxhash.h` (about 60 lines, checked against the published test vectors).
Tests: round trip of attributes of both classes and types, details, closed and cubic primitives and an empty geometry; the
result must be bit-identical (`sameGeometry`); a corpus of mutated files (flipped bytes, truncations, huge counts, offsets past
the end) must all throw `std::runtime_error` and never read out of bounds.

**Step 2, nodes and tools.** *Done 2026-10-26* (see `geometryFiles.md`), with two deviations: `--geo-in FILE` renders a file without a graph
(`--svg`/`--png`) instead of replacing a node's input, and the node skips its input through a new `Node::needsInputs()` hook that
`buildTask` consults. `fileCache` also accepts `$F` (it shares the expansion with `fileRead`), which step 3 builds on. GUI tests
for the two new palette buttons are in the every-palette-button test; the "Save geometry..." menu item opens a native file dialog and
has no GUI test.
- `fileRead` (no input): `path`; loads the file. A `$F` / `$F4` token in the path is replaced by the frame number
  (`round(time * fps)`), so a baked sequence plays back. It depends on the playhead only when the path has a token. A missing
  or corrupt file is a cook error naming the path.
- `fileCache` (one input): `path`, `mode` = `write` (cook the input and write it), `read` (ignore the input and read) or `auto`
  (read when the file exists and is valid, else cook the input and write it). Passes the geometry through unchanged. In `read`
  mode the input is not cooked at all, which is the point: an expensive simulation upstream is skipped. The file does not record
  what produced it, so a stale cache is the user's to delete or re-write; the docs say so plainly.
- `jitnodesCli --geo-out FILE` writes the cooked display node as `.jngeo`, and `--geo-in FILE` replaces the input of a named
  node for testing. The editor's viewport context menu (today "Export..." for images, which picks PNG or JPEG by extension) gets a
  "Save geometry..." item.
- Palette buttons (the mockup, the converter, `editorWindow.cpp`, one GUI test entry each) and a docs section in `fx.md` or a new
  `geometryFiles.md`.

**Step 3, baked sequences and trail history.** `fileCache` in `record` mode writes `$F` frames as the playhead visits them, and a
`fileRead` of earlier frames gives any node a history without cooking inputs at other times. A `trail` that reads frames
`t - k * step` from a sequence is then a real recorded trail. This is the cheap alternative to "cook obstacle inputs at other
times" for boids obstacles and trails; it needs the sequence to be baked once, so it suits finished sims, not interactive ones.

**Step 4, trace export.** Record `{node, thread, start, end}` for each task in a pre-sized per-thread buffer (no lock on the
hot path), `Network::writeTrace(path)` emits Chrome trace JSON (`chrome://tracing`, Perfetto), and `jitnodesCli --trace FILE`
writes it. The profile table in the editor stays as it is. Test: the trace parses as JSON, every task appears once, and
recording does not change any result.

**Step 5, scheduler refinement (only if step 4 shows barrier gaps).** `Network::cook` runs the tasks one level at a time, so a
slow node in a level holds back unrelated branches in the next. Replace the level barriers by a dependency counter per task: a
task becomes runnable when its last input finishes. Results do not depend on scheduling, so the existing thread-count tests
keep working; compare traces before and after on an uneven graph (one slow `reactionDiffusion` branch next to many cheap nodes).

**Step 6, string attributes (separate project).** A string attribute as `int32` ids into a per-geometry string table, written to
section 1 of the file and readable in wrangles through hashed comparison. Needs a new `AttribType`, so it touches the
kernel cache key, `merge` (table union and remapping) and `attribCreate`; plan it on its own.

**Order and effort.** 0 and 1 first (a day, mostly tests), then 2 (a day with palette and docs), 4 (half a day), 3 (half a day on
top of 2), 5 and 6 only on evidence. Nothing here changes the JSON graph format, existing graphs or existing node behaviour.

**Risks.** A cache that does not know its input changed gives stale pictures (mitigation: `auto` is opt-in, the node shows the
file's age in its status text, and the docs warn); files from a newer build are rejected, never half-read; `.jngeo` carries no
asset or node information, so it is a data file, not a replacement for graph files.

## Threading and profiling
- `ThreadPool::parallelFor(count, fn)`: the caller takes part and idle workers help, so it can be called from inside a
  pool task (nested parallelism) and with zero workers (runs inline). The first exception is rethrown once all indices
  have finished. `Network::setThreadCount(n)` sets total threads (1 = caller only; default is hardware threads - 1,
  capped at 8).
- Data parallelism: `Kernel::run(..., pool)` splits ranges of at least 32768 padded elements into chunks that are
  multiples of 16 lanes (so every chunk stays 64-byte aligned) and runs the same compiled function on shifted channel
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
- Measurements, including JIT off vs on, are in `docs/performance.md`. In short: kernels are memory-bound with the
  JIT, so threads help little there; before the copy work below, copying large geometries and merging dominated.
- Copy-on-write attributes: `AttribArray` shares its storage between copies and clones it on the first non-const
  access (`floats()`, `ints()`, `data()`, `copyFrom`, `resize` of a shared array). Copying a `Geometry` is therefore
  O(number of attributes), and a kernel node only clones the channels it stores to (`Kernel::run` fetches read-only
  channels through the const accessors). Cloning happens on the cooking thread of the node that writes, so shared
  arrays are only ever read by other threads; `sharesStorageWith` exposes the sharing for tests.
- N-ary merge: `Geometry::concat(parts, pool)` builds the result with one allocation per attribute and runs the
  per-attribute copies on the pool. `Network::planCook` flattens a tree of `merge` nodes (dirty, single consumer, not
  the display node, fusion enabled) into one task over the leaf inputs, so a balanced or left-deep tree costs one
  concat. The folded merge nodes are not cooked or cached (like interior nodes of a fused chain; profile note
  `fused into <tail>`). Leaf order equals the tree's left-to-right order, so results are identical.
- `Network::renameNode` validates the name (identifier, unique) and rewrites `old.param` references in expressions of
  the same network; `uniqueName` picks the smallest free `<letters><n>`. `copyNode`/`pasteNode` (serialize.h) carry a node
  as its JSON text plus the names of its inputs, so a paste reconnects by name and works across graphs. `pasteNodes`
  pastes a set: wires and `old.param` expression references between the copied nodes follow their new names
  (`Network::rewriteReferences` applies all renames at once), and a failure removes the nodes already created.
- The editor selection is a set (`EditorModel::selection`); `selected()` is the node only when exactly one is selected.
  `GraphController` drags the whole selection, shift/ctrl-click toggles, and an empty-space drag draws a selection box.
- `merge` takes any number of inputs (`Node::hasVariableInputs`): the network keeps exactly one unconnected input after
  the last connected one (at least two), `connect` may name any index, unconnected inputs are skipped when cooking, and a
  merge with nothing connected is an error. Files list every slot, with `0` for an empty one; undo restores by slot.
- Cancellation: `Network::cancel()` (any thread) makes the cook in progress throw `CookCancelled` before it starts its
  next task; finished nodes keep their results and unfinished ones stay dirty. Subnets check the root network's flag.
  A cancel while no cook is running is ignored. Granularity is one node: a running kernel or node is not interrupted.
- Scratch-arena temporaries were dropped from the plan: after the copy-on-write change no cook path allocates large
  temporaries (what remains are small per-task vectors), so there is nothing for an arena to recycle.
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
- `sin/cos/exp/log/pow/floor/ceil` are JIT-compiled on every instruction set (see "Platform support").
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
- Int attributes: a value an earlier node of the chain writes into an int attribute is run through the store's truncation
  (`composeKernels` gets the channel types of the chain's input geometry) before a later node reads it, so fused and
  unfused cooking agree (found by `jitnodesStress`).
- The kernel cache key (`KernelSpec::signature`) includes the channel names: a compiled `Kernel` binds channels by the names of
  the spec it was built from, so programs that differ only in attribute names must not share one (also found by `jitnodesStress`).

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
6. **Execution (done except raster/interruption)**: see "Threading and profiling" below. Still open: 2D tile/band
   splitting for raster work, interrupting a running kernel (cancellation is per node today).
7. **Platform (x86-64 done, AArch64 open)**: see "Platform support" below. Open: an AArch64/NEON backend (needs a device to
   test on), FMA, an N-lane-agnostic remainder strategy for odd widths.
8. **Persistence and tooling (first slice done)**: see "Persistence and tooling" below. Subnets and assets: see "Subnets and
   assets". Done: diff-based undo, asset versions, `updateAssetInstances` and migration hooks. Still open: a loadable
   plugin ABI, the editor front end (newui, see "Editor UI"), Blend2D output.
9. **Binary geometry files (planned)**: see "Binary geometry files and other follow-ups from the format research" above:
   measure, `.jngeo` format with checksum, `fileRead` / `fileCache` nodes and CLI, baked sequences, trace export, then a
   dependency-counting scheduler and string attributes if the evidence asks for them.

## Editor UI
- Decision: the editor is built on [newui](https://github.com/jimcrafton/newui) (checkout `D:\code\newui`; Win32,
  Blend2D, own `View` tree and reflection), not on ImGui. The core library stays free of UI dependencies; the editor
  is a separate executable target that links `jitnodes` and `newui` (not added to CMake yet).
- Screens are designed as HTML mockups, converted once to `.newui` documents with the `html-mockup-to-newui` skill
  (`.claude/skills/html-mockup-to-newui`: `html2newui.py` converter, `examples/parameterPanel.html`), checked with
  `validateNewui.py` (`.claude/skills/newui-view-json5`) against newui's generated reflection data, and loaded at
  run time with `newui::Bundle::loadFrame()`/`loadView()`. The editor code finds widgets by `name` and wires events
  (`Button::onClick`, `Slider::onValueChanged`, ...) to `Network`/`History` calls.
- What a `.newui` file cannot express is done in code: the node-graph canvas (a named placeholder `SubView` drawn
  with Blend2D), `DropDownList`/`ListView`/`TreeView` models, slider ranges, image paths.
- Planned panels: node-graph canvas with wiring, parameter panel (floats, expressions, keyframes), 2D viewport
  (geometry through the SVG/Blend2D path), profile panel (`profileReport`), undo/redo from `History`, asset library.
- The first screen is `editor/mockups/mainWindow.html` (1280 x 800: toolbar, node palette, graph canvas, parameter
  panel, viewport, profile list, status bar) converted to `editor/ui/mainWindow.newui`; `editor/README.md` lists every
  named view and what it is wired to.
- Build: `JITNODES_BUILD_EDITOR` (off by default, Windows, single-config generators) adds `editor/` and builds newui
  in its own `ExternalProject` tree (`JITNODES_NEWUI_DIR`, default `../newui`); the imported `newui::newui` target carries
  its include paths, `BL_STATIC` and system libraries. Blend2D embeds its own asmjit under the `abi_bl` namespace, so it
  does not clash with jitnodes' `v1_21` one. The editor target compiles with `/permissive` because newui's headers rely on
  MSVC's lenient two-phase lookup (`/std:c++20` otherwise implies `/permissive-`). `jitnodesEditor --check` loads
  `Resources/mainWindow.newui` and verifies every view the code looks up; see `editor/README.md`.
- Wiring: `EditorModel` (UI-free, tested) plus `EditorWindow` (newui views by name) cover the toolbar and palette; see
  `editor/README.md`. The graph canvas is `GraphCanvas` (Blend2D drawing, newui mouse/key events) over a UI-free
  `GraphController`. Nodes carry an optional editor position (`Node::setPosition`, saved as `"pos": [x, y]`, part of the node
  text in history snapshots so moves undo); it never dirties a node. The parameter panel (`ParameterPanel` over `EditorModel::parameters/setParameter/
  toggleExpression/keyframeNow`) builds rows per selected node in a `ScrollView`; edits are single undo steps. The viewport
  (`ViewportController` + `ViewportCanvas`) draws the display node's geometry (kept by `EditorModel::cook`) with a decimating
  stride for huge point counts. The profile list
  (`EditorModel::profile` over `NodeStats`, `ProfileController` + `ProfileCanvas`) shows cooks, last time and fusion notes
  per node, subnet contents indented. Keyframes are edited in a modal dialog
  (`KeyframeDialog` over a `KeyframeEditor` working copy, built from `keyframeDialog.newui`) opened from each float row;
  OK replaces the keys in one undo step. The subnet view
  (`EditorModel::enter/up`, `network()` = the network being edited, double-click or Enter to dive, Up or Backspace to
  leave, Add parameter for promoted parameters) keeps one root-level history; child networks follow the parent's fusion
  setting (`Network::fusionEnabled`). Assets are created from a subnet (`EditorModel::createAsset`: definition file, registration, the subnet replaced by an
  instance, outdated instances updated), instanced from a palette list, loaded from files or an `Assets` folder, and
  unlocked back into a subnet (`unlockAssetInstance`). `Network::replaceNode` keeps a node's editor position. The viewport colours by any point or primitive
  attribute, or the length of a vector attribute (`ViewportController` computes the choices, values, range and colour map;
  `ViewportCanvas` draws segments, dots, legend and a hover read-out). Open: an asset library browser and per-asset editing in place, dragging keys on a curve, and
  the remaining screens.

## Conventions
See `CLAUDE.md`: camelCase functions/variables/files, PascalCase types, `_` member postfix, minimal comments.
