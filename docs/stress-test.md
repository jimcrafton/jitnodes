# The stress test

`jitnodesStress` (`tests/stressTest.cpp`) pushes the node execution engine far beyond what the unit tests do: **thousands
of nodes, 10,000 or more points per node, up to millions of points moving through a graph**, with every result checked
rather than just timed. It is a separate executable so the unit tests stay fast; `ctest` runs its quick size.

How the engine works is described in [architecture.md](architecture.md).

## Running it

```
cmake --preset x64-release
cmake --build --preset x64-release --target jitnodesStress
out\build\x64-release\jitnodesStress.exe             # full run, about 80 seconds, about 3000 nodes per scenario
out\build\x64-release\jitnodesStress.exe --quick     # a few seconds; this is what `ctest` runs (jitnodesStressQuick)
```

Exit code 0 means every check passed. Output is unbuffered, so progress is visible even if a scenario crashes the process.

| Option | Meaning |
|---|---|
| `--quick` | small sizes (16 branches x 12 nodes, 3,000 points, 400-node DAGs) |
| `--seed N` | seed of every random graph (default 1); a failure is reproduced by the same seed and options |
| `--scenario a,b,...` | run only these: `chains`, `dag`, `dagmath`, `edits`, `subnets`, `time`, `cancel`, `wide`, `scaling`, `fuzz`, `concurrent`, `soak`, `shapes`, `langfuzz`, `pathological`, `faults` |
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

### More scenarios

These look for a different kind of problem than size: odd inputs, concurrency, long runs, hostile graphs and failures.

| # | Scenario | What is exercised |
|---|---|---|
| 10 | `fuzz` | Many small random graphs (40-200 nodes, seeds derived from `--seed` and the iteration number), each compared across ISAs, fusion, thread counts and sometimes the interpreter, with leaf digests, save/load identity and the consumer-index check. `--duration SECONDS` runs for a time instead of a count (nightly use); a failure prints the iteration, reproduce with `--fuzz-first N --fuzz-iterations 1`. |
| 11 | `concurrent` | Independent networks loaded and cooked on 8 threads at once; one kernel cache hit by 8 threads (each distinct kernel is compiled exactly once and runs correctly from all of them); one thread pool shared by 8 threads with nested `parallelFor` and throwing tasks; `cancel()` hammered from a second thread while another keeps editing and cooking. |
| 12 | `soak` | Thousands of edit / commit / undo / redo / cook cycles (4,000 in the full run) watching history size, kernel-cache size, working set and the reverse indexes, then the result against a fresh rebuild; then 6,000 unique programs on one node to exercise the kernel-cache cap. |
| 13 | `shapes` | Point counts 0, 1, 2, 3 ... around every lane width, the 32,768 chunk size and 100,001 (each against the reference on 4-5 configurations); 12 million points through a fused chain; 4,000 sources of 1-3 points merged. |
| 14 | `langfuzz` | Random programs on NaN, infinity, denormal, +-0 and int-extreme data (all ISAs agree, the exact subset also matches the interpreter); mutated program text (parse or a clean error, never a crash); programs nested or chained up to 50,000 deep. |
| 15 | `pathological` | A merge of 10,000 inputs; a ladder of merges with exponentially many paths (cycle detection); one node feeding 10,000 consumers; chains of 30,000 nodes fused and unfused; a left-leaning chain of 5,000 merges; chained parameter references; subnets nested to the limit; save/load of those. Every case must succeed or fail with a message. |
| 16 | `faults` | Parallel cooks where up to 8 branches throw `runtime_error`, `bad_alloc` or a non-`std` exception (or sleep); cancel while branches sleep; a `bad_alloc` injected into the *n*-th allocation of a cook, of JIT compilation and of undo/redo, for a sweep of *n* (the test replaces `operator new`). The next cook must always be correct. A watchdog aborts the process if anything hangs. |

Nightly fuzzing: `powershell tools/nightlyFuzz.ps1 -Seconds 1800` runs the `fuzz` scenario for that long with the date (`yyyymmdd`) as the seed, writes
`out/fuzzLogs/fuzz-<seed>.log`, prints the tail and exits non-zero on a failure (the log has the iteration to reproduce with `--fuzz-first`).
Schedule it with Windows Task Scheduler or any CI runner; build `x64-release` first.

Options for these: `--fuzz-iterations N`, `--fuzz-first N`, `--duration SECONDS`, `--soak-cycles N`, and `--write-graph FILE` (saves the chain graph with node positions and exits; the editor GUI load test uses it).

## Other tools

- **AddressSanitizer.** `cmake --preset x64-asan && cmake --build --preset x64-asan --target jitnodesTests jitnodesStress`, then run both. All of the above pass (a few minutes for the quick sizes). The JIT-generated code itself is not instrumented, and ASan frames are several times larger than release frames, which is how it found the stack-depth problems listed below. On Linux the same option adds UBSan.
- **Debug builds.** `x64-debug` and `x64-debug-editor` build and pass everything (93 unit tests, all 16 stress scenarios, the editor model tests, and the 66 + 5 GUI tests against the debug editor). The allocation-failure sweeps of `faults` are skipped there: the MSVC debug standard library allocates inside `noexcept` moves, so an injected `bad_alloc` can only terminate. On Windows the stress test installs a `std::terminate` handler that prints a symbolised stack, which is how that was diagnosed.
- **ThreadSanitizer** is not available for MSVC or clang-cl on Windows. On Linux, configure with `-DCMAKE_CXX_FLAGS=-fsanitize=thread` and run `jitnodesStress --scenario concurrent,cancel,fuzz`.
- **Editor GUI load test.** `editor/tests/gui/test_editor_gui_load.py` opens the 2,646-node graph in the real editor and drives select all, nudge, copy/paste, cut and undo with real input (it takes over mouse and keyboard, so run it on an idle desktop):
  `python editor\tests\gui\test_editor_gui_load.py --exe out\build\x64-release-editor\editor\jitnodesEditor.exe`.

## Results

Full run on an AMD Ryzen 9 7900X (12 cores / 24 threads), Windows 11, MSVC Release, 2026-10-17, seed 1:
**the original nine scenarios: 1,472 checks, 0 failed, 74.6 s; with the seven added ones, all 16 scenarios: 32,794 checks, 0 failed, 79 s**. Peak working set about 1.6 GB (the engine caches every node's result, and the interpreter and unfused configurations hold thousands of geometries at once).

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

and of recooking after editing one node: about 2.0 ms fused, 1.3 ms unfused. With only 10,000 points per source the kernels
are tiny, so what is measured is the engine's own overhead (planning, composing kernels, cache lookups, task scheduling), and
unfused is as fast or faster once several threads are available. Fusion pays when there are many points per node (see the 1M-point numbers in
[architecture.md](architecture.md#performance)). A chain is composed with `appendKernel`, which adds each member to the
accumulated graph in place, so composing a chain of *k* nodes is O(k) (it was O(k^2) with the copying `composeKernels`; the
1-thread fused full recook went from 70 ms to 46 ms).

## What the stress test found

The first runs of this test found five problems in the engine, and the scenarios added later found eight more. All are fixed, and each has a regression test in
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

Found by the later scenarios (all fixed, each with a regression test in `tests/testMain.cpp`):

6. **A parser stack overflow.** `parseWrangle` is recursive descent; `(((...(x)...)))` or `-----...x` a few thousand levels deep crashed the process. Nesting is now limited to 256 levels and to the stack that is left, with a normal parse error.
7. **Cycle detection was exponential.** `Network::reaches` walked every *path* upstream, so a ladder of merges that each join the two before them (a few dozen rungs) made `connect` effectively hang. It now walks each node once, and downstream from the node being wired, which is also much shorter.
8. **Planning recursed once per node.** A chain of a few thousand nodes that does not fuse (fusion off, or fan-out between every node) overflowed the stack in `planCook`; so did a left-leaning chain of merges (`collectMergeInputs`) and `executionOrder`. All three are iterative now; a 30,000-node chain cooks in 0.14 s unfused.
9. **Everything that asked "who uses this node" scanned the whole graph.** `markDirty` visited each downstream node and scanned all nodes for each, `consumerCount` scanned for each fusion candidate, and `connect` of a 30,000-node chain took minutes. The network now keeps reverse indexes (consumers per node, nodes that mention a node name in an expression) in step with every edit; building the 30,000-chain went from about 150 s to under a second, a merge of 10,000 inputs from 1.4 s to 40 ms, and the edit-storm scenario got 25% faster. `Network::indexesConsistent()` checks the indexes against a scan and the soak, fuzz and edit scenarios call it.
10. **Parameter expressions that refer to each other recursed without limit.** A chain of 2,000 nodes each referring to the previous node's parameter crashed. Evaluation now stops with an error when the stack headroom (`stackHeadroom()`, new in `core/stackGuard.h`) or 400 levels run out; cook, save and load check the headroom too, and subnets cannot be nested more than 64 levels (`Network::maxSubnetDepth`), because cooking, saving, loading and undo all recurse through them. Counting levels alone is not enough: AddressSanitizer and debug builds use several times more stack per level than release.
11. **The kernel cache never shrank.** Every distinct program ever cooked stayed compiled, about 3-4 KB each; typing code into a wrangle creates one per keystroke. The cache is now capped at 4,096 kernels (`KernelCompiler::maxCachedKernels`), dropping the least recently used quarter when full; kernels in use stay alive through their `shared_ptr`.
12. **Recomposing a fused chain was quadratic** (the item the first version of this page listed as open): `appendKernel` adds each member to the accumulated graph in place, so composing *k* nodes is O(k), and the 1-thread fused recook of the 2,646-node graph went from 70 ms to 46 ms.

13. **Pasting a large selection hung the editor** (found by the GUI load test, not the headless scenarios). `uniqueName` scanned every node for each candidate name, so pasting 2,646 nodes onto themselves was cubic and did not finish in 30 s. Node names are now indexed (`findNode` is a hash lookup) and `uniqueName` remembers the smallest free number per stem; the same paste takes 0.23 s in the editor.

What the later scenarios checked and found *no* problem with: JIT results on NaN, infinity, denormals, signed zeros and int extremes across SSE2 / AVX2 / AVX-512 and the interpreter; point counts around every lane width and chunk boundary; 12 million points; concurrent cooking of independent networks; one kernel cache and one pool used from many threads (including exceptions thrown inside nested `parallelFor`); `cancel()` called thousands of times while cooking; and allocation failures injected into cooks, JIT compilation and undo (every one fails with `bad_alloc` or is absorbed, and the next cook is correct).

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
