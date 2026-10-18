# The stress test

`jitnodesStress` (`tests/stressTest.cpp`) pushes the node execution engine far beyond what the unit tests do: **thousands
of nodes, 10,000 or more points per node, up to millions of points moving through a graph**, with every result checked
rather than just timed. It is a separate executable so the unit tests stay fast; `ctest` runs its quick size.

How the engine works is described in [architecture.md](architecture.md).

## Running it

```
cmake --preset x64-release
cmake --build --preset x64-release --target jitnodesStress
out\build\x64-release\jitnodesStress.exe             # full run, about 75 seconds, about 3000 nodes per scenario
out\build\x64-release\jitnodesStress.exe --quick     # a few seconds; this is what `ctest` runs (jitnodesStressQuick)
```

Exit code 0 means every check passed. Output is unbuffered, so progress is visible even if a scenario crashes the process.

| Option | Meaning |
|---|---|
| `--quick` | small sizes (16 branches x 12 nodes, 3,000 points, 400-node DAGs) |
| `--seed N` | seed of every random graph (default 1); a failure is reproduced by the same seed and options |
| `--scenario a,b,...` | run only these: `chains`, `dag`, `dagmath`, `edits`, `subnets`, `time`, `cancel`, `wide`, `scaling` |
| `--branches N --depth N --points N` | size of the chain graphs (defaults 64 x 40 x 10,000) |
| `--dag-nodes N` | nodes of the random DAGs (default 3,000, plus sources and reducers) |
| `--edits N` | length of the edit storms (default 150) |
| `--threads N` | worker threads (default: the hardware count, at most 8) |
| `--wide-attrs N --wide-chain N --wide-points N` | size of the wide-geometry scenario |
| `--no-interpreter` | skip the slow scalar-interpreter comparisons |
| `--verbose` | print every comparison |

## What it checks, and how

There are three kinds of oracle, so that a bug has to fool all of them to go unnoticed:

1. **An independent reference.** For the chain graphs the test carries its own C++ implementation of every node program, with
   the same float operations in the same order, and compares the engine's output with it *bit for bit*.
2. **Another configuration of the same graph.** The graph is saved to text and loaded into a fresh `Network` for each of:
   JIT on SSE2 / AVX2 / AVX-512, fusion on and off, one thread and many, and the scalar interpreter. Everything must be
   bit-identical (NaNs of any payload and `+0`/`-0` compare equal). Every *leaf* result of the random graphs is also
   compared in full, by a 64-bit digest of all its attribute values, not only the final merged output.
3. **A graph rebuilt from scratch.** After edits, the incrementally recooked result must equal what a freshly loaded copy
   of the saved graph cooks to, with a different planner path (no fusion, one thread).

| # | Scenario | Size (full run) | What is exercised |
|---|---|---|---|
| 1 | `chains` | 2,646 nodes: 64 branches x 40 point-wise nodes, 10,000 points each, 4-way merge tree | cold cook, fusion into 64 kernels, every configuration, save/load byte-identity, 150 single-parameter edits (each may recook only a handful of nodes), 30-step undo/redo through `History`, each step compared with the reference |
| 2 | `dag` | about 4,300 nodes: random DAG of `line`, `circle`, `transform`, random `wrangle` programs, `attribCreate`, `null`, nested `subnet`, and merges of 2-5 inputs with heavy fan-out; 991 leaves | exact-arithmetic programs (`+ - * min max abs sqrt floor / ?: && ! ==` ...) against all configurations **including the interpreter**, with int attributes and per-leaf digests |
| 3 | `dagmath` | about 4,300 nodes | the same with `sin cos exp log pow`: all JIT configurations must agree (the interpreter uses the C library and legitimately differs in the last digits) |
| 4 | `edits` | about 4,300 nodes, 150 edits in 30 batches | parameter edits, new program text (new kernels), time-dependent expressions with cross-node references, rewiring to any earlier node, adding nodes to a 1,000-input-style merge, removing nodes, playhead changes; 11 comparisons against a from-scratch rebuild, then undo and redo of every batch reproducing every earlier state exactly (graph text and geometry) |
| 5 | `subnets` | 300 asset instances in a chain plus 8 nested subnet levels, 10,000 points | promoted parameters, `parent.` forwarding through 8 levels, a reference result, editing a promoted parameter never recompiles, all instances share a handful of kernels, save/load with the embedded asset |
| 6 | `time` | 1,365 nodes, half animated with `time`, `frame` and references to other nodes, 60 frames | only the animated nodes are time-dependent and recooked; static branches are never touched; every 15th frame equals a from-scratch cook |
| 7 | `cancel` | 1,920 nodes on the interpreter | `Network::cancel()` from another thread aborts a slow cook (`CookCancelled`), the next cook is unaffected and matches the reference; a syntax error in the middle of a parallel cook, a missing input, a cycle through 40 nodes and a bad expression are each reported and recoverable |
| 8 | `wide` | 100,000 points x 200 attributes, then a fused chain of 400 wranglers writing 20 of them | copy-on-write: the 180 untouched attributes still share storage with the input, the 20 written ones were cloned once each, a replay of the writes matches; one kernel for the whole chain |
| 9 | `scaling` | the chain graph | full and single-edit recooks for 1/2/4/8 threads, fusion on and off (a table, not a pass/fail measurement) |

## Results

Full run on an AMD Ryzen 9 7900X (12 cores / 24 threads), Windows 11, MSVC Release, 2026-10-17, seed 1:
**1,472 checks, 0 failed, 74.6 s**. Peak working set about 1.6 GB (the engine caches every node's result, and the interpreter and unfused configurations hold thousands of geometries at once).

| Measurement | Result |
|---|---|
| Cold cook of 2,646 nodes (640,000 points out, 64 fused kernels) | 50 ms, of which 11 ms compiling |
| Same graph, JIT off (interpreter) | 105 ms fused, 95 ms unfused |
| Edit one parameter, recook | 2.4 ms on average; at most 3 nodes recook |
| Save + load a 2,646-node graph | 836 KB of JSON, 259 ms to load, byte-identical re-save |
| Commit one undo step on a 2,646-node graph | 21 ms (the graph is snapshotted) |
| Random DAG, 4,309 nodes, 19 million leaf points | cold cook 160 ms, 1,444 kernels in 86 ms |
| Same, unfused / 1 thread / interpreter | 145 ms / 363 ms / 2,496 ms (unfused with the interpreter) |
| Random DAG with `sin cos exp log pow` | cold cook 253 ms, 1,436 kernels in 176 ms |
| 300 asset instances + 8 nested levels, 10,000 points | 6 ms cold; 2 kernels compiled in all |
| 1,365-node animated graph, per frame | 8.9 ms, 33 node cooks |
| One fused kernel from a 400-node chain on 100,000 points | 58 ms cold including the compile (1.1 ms); 1,500 nodes: 0.4 s; 3,000 nodes: 1.6 s |

Thread scaling of a full recook of the 2,646-node graph (10,000 points per source, median of 7):

| threads | fusion on | fusion off |
|---:|---:|---:|
| 1 | 72.6 ms | 44.8 ms |
| 2 | 48.9 ms | 49.2 ms |
| 4 | 39.9 ms | 43.9 ms |
| 8 | 37.4 ms | 39.9 ms |

and of recooking after editing one node: 2.4-2.5 ms fused, 1.2-1.4 ms unfused. With only 10,000 points per source the kernels
are tiny, so what is measured is the engine's own overhead (planning, composing kernels, cache lookups, task scheduling), and
unfused is as fast or faster. Fusion pays when there are many points per node (see the 1M-point numbers in
[architecture.md](architecture.md#performance)). The remaining cost of fusing is `composeKernels`, which re-copies the
accumulated graph for every member of a chain, so composing a chain of *k* nodes is O(k^2) work on every recook of that chain; caching the
composed kernel of a chain, or composing incrementally, would remove it.

## What the stress test found

The first runs of this test found five problems in the engine. All are fixed, and each has a regression test in
`tests/testMain.cpp` or is covered by the stress test itself.

1. **Programs that differ only in attribute names shared one kernel (wrong results).** `KernelSpec::signature()`, the kernel
   cache key, left out the channel names, but a cached `Kernel` binds its channels by the names of the spec it was
   built from. `x = x * 2;` in one wrangle and `y = y * 2;` in another compiled to *one* kernel, and the second node ran the
   first node's program: `y` was never doubled. Seen with fusion off, and with fusion on whenever two nodes' composed
   programs had the same shape. The channel names are now part of the key (test
   `programsThatDifferOnlyInAttributeNamesDoNotShareAKernel`).
2. **Fused and unfused cooking disagreed for int attributes.** An int attribute truncates toward zero when stored, but inside a
   fused chain the next node read the untruncated float the previous node had computed. `composeKernels` now applies the
   same truncation (including `INT_MIN` for NaN and out-of-range values) when a later node reads an int channel an
   earlier node wrote (test `fusionTruncatesIntegerAttributesBetweenNodesLikeUnfusedCooking`).
3. **A crash on pool worker threads for very large fused kernels.** A kernel fused from hundreds of nodes has thousands of
   runtime parameters; they were all broadcast before the loop and spilled to a stack frame of tens of kilobytes. Generated
   code does not probe the stack, and on Windows the stack grows through its guard page, so a frame larger than a page
   skips the guard page on a thread whose stack has not grown yet: an access violation, only with two or more threads and
   100,000 points or more (the size at which `Kernel::run` splits a range across the pool). Fixed twice over: beyond 32
   invariants each is broadcast where it is used instead of being hoisted, and each thread commits 256 KB of stack before
   it first runs generated code.
4. **Quadratic-to-cubic cost for long fused chains.** `ExprGraph::intern` scanned every existing node on each insertion, and
   fusing a chain re-copies the growing graph for each member. A cold cook of a 3,000-node chain took 57 seconds; with a
   hash index it takes 1.6 seconds (1,500 nodes: 7.3 s to 0.4 s; 400 nodes: 180 ms to 56 ms).
5. **Differences in the sign of zero between instruction sets.** SSE2 `floor`/`ceil` (an int32 round trip) return `+0` for
   `-0.0` where AVX2 and AVX-512 return `-0.0`. Not fixed, since the values compare equal; the stress test treats
   `+0` and `-0` as equal and the behaviour is documented in [architecture.md](architecture.md#the-jit-from-wrangle-text-to-machine-code).

A sixth finding was a test bug rather than an engine bug, but worth knowing: **`History` does not restore the playhead**
(documented), and `History::commit` does nothing when the graph did not change, so an edit that sets a parameter to its
current value creates no undo step.

## Design notes of the test

- **Reproducible.** All graphs come from a seeded generator (`--seed`); the generator, the reference model and the oracles
  are in one file with no dependencies beyond the library.
- **Bounded memory.** The engine caches the result of every node, so a graph of thousands of nodes holds thousands of
  geometries. The random DAG caps every merge at four sources' worth of points, reduces each leaf to 50 points with a
  `resample` node before the final merge (so everything still cooks and contributes to the display node), and compares
  the full leaf results through digests instead of keeping copies.
- **Int attributes everywhere.** Every source is followed by two `attribCreate` nodes that make `i1` and `i2` int, so every
  geometry in the graph has them as int and random programs may freely read and write them; an int attribute merged with a float one of the same
  name is an error by design, and the generator avoids creating that situation.
- **Exact-arithmetic and transcendental programs are separate graphs,** because only the former can be compared against the
  interpreter (and the reference model) bit for bit.
- **Edits are applied through the public API** (`setFloat`, `setString`, `setExpression`, `connect`, `addNode`,
  `removeNode`, `History`), so the test drives the same paths as the editor.

## Extending it

A new scenario is a function `void scenarioFoo(const Options&, Report&)` registered in the table in `main`. Use `rep.check(ok,
message)` for assertions, `sameGeometry` / `digest` to compare results, `cloneWith(net, config)` to get the same graph in another
configuration, and the `Rng` for reproducible randomness. If a random graph fails, rerun with the same `--seed` and options
and reduce `--dag-nodes` (or `--branches`/`--depth`) until it is small; the scratch-fuzzing approach that found
finding 1 (random two- and three-node chains compared fused against unfused) is a good way to get a minimal case.
