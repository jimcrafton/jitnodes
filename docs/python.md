# Python bindings

`import jitnodes` lets a script build and cook node graphs, edit parameters, wire nodes and undo. The same module runs
in a plain Python (headless scripts, tests) and embedded in an application such as curvy (a script then edits the
document's own graph). This page explains how the binding is built, what had to be added to jitnodes for it, the
object-lifetime problems and how they are handled, and the Python API.

Contents: [Overview](#overview) · [How the binding is built](#how-the-binding-is-built) ·
[What was added to jitnodes](#what-was-added-to-jitnodes) · [Object lifetime](#object-lifetime) ·
[The Python API](#the-python-api) · [Embedding in an application](#embedding-in-an-application) ·
[Building and testing](#building-and-testing) · [Limitations and what is not done](#limitations-and-what-is-not-done) ·
[Worked example: a night sky](pythonExample.md)

## Overview

```
 jitnodes headers ──scriptgen (newui)──▶ generated pybind11 code ─▶ jitnodes._core      raw engine classes (do not hold them)
   // @scriptable marks                                                    ▲
                                                                           │ forwards every call, after checking
 python/pyHandles.cpp (hand written) ─────────────────────────────▶ jitnodes.Network / Node / History   handles (use these)
                                                                    jitnodes.Geometry, Keyframe, enums   plain values
```

Two layers, on purpose:

* **Generated, `jitnodes._core`.** newui's scriptgen reads the `// @scriptable` marks in the jitnodes headers and writes
  pybind11 bindings for `Network`, `Node`, `Geometry`, `History`, `Keyframe` and the enums (see
  `newui/tools/scriptgen/README.md`). New public methods show up in Python on the next build without anyone editing a
  binding list. These classes wrap the engine's objects directly, so they are raw references.
* **Hand written, `python/pyHandles.cpp`.** The classes scripts actually use are *handles* (`Network`, `Node`, `History`).
  A handle is safe to keep: it checks on every call that what it points at still exists, and otherwise raises a clear error
  instead of crashing. Everything it does not define itself it forwards by name to the generated class.

## How the binding is built

1. **Marking.** The public surface is chosen in the headers, with comments scriptgen reads:
   * `// @scriptable` on `Network`, `Node`, `Geometry`, `History`, `Keyframe` and the enums `AttribClass`, `AttribType`,
     `PrimKind`, `KeyInterp`. `Node` and `Geometry` use `holder=shared`. `Geometry` is also `readonly`: a script gets the
     very object the node cache holds (`cook` returns a `shared_ptr<const Geometry>`), and Python has no const, so scriptgen
     binds only its const methods. Before this was added, a script calling `geometry.resize_points(3)` changed the cached
     result and the next `cook` returned the corrupted geometry; a test now checks that no mutator exists.
   * `// @scriptable ignore=true` on the internal types `AudioStep`, `ThreadPool`, `AttribArray`, `Primitive`, `ShapeRange`,
     `CookContext`, `NodeStats`, `PointKernel`, `NodeWriter`, `NodeReader`, `RestoreStats` and `KernelCompiler`. Every method that
     names one of them is left out of Python automatically.
   * `ignore=true` on `Geometry::pointAttrib/findPointAttrib/x/y` (raw pointers to point data; they will become NumPy views),
     `Network::replaceNode`, `Network::touchedSince`, and the two members added for the bindings (`lifetime`, `generation`).
   * `// @scriptable release_gil` on `Network::cook`, `cookDisplay` and `freezeNode`.
2. **Generation.** scriptgen runs over `include/jitnodes` with `-std=c++20 --strict` at build time. The result is one `.cpp`
   (`registerJitnodesScripting(py::module_&)`) and a type-stub file `_core.pyi`. Names are snake_case (`setFloat` is
   `set_float`, `typeName` is `type_name` as a keyword), overloads, default arguments, `std::vector`/`map`/`set`/`optional`/
   `function` conversions and the enums come out as described in the scriptgen README.
3. **Hand-written layer.** `registerModule()` (in `pyHandles.cpp`) creates the submodule `jitnodes._core`, calls the generated
   function on it, remembers the generated `Network`, `Node` and `History` classes, re-exports the safe value types
   (`Geometry`, `Keyframe`, the enums) in the main module, and defines the handle classes and `StaleHandleError`.
4. **Forwarding.** A handle's `__getattr__` finds the method on the generated class and returns a callable that, *when called*:
   checks the handle, translates the arguments, calls the generated method, translates the result.

   | Direction | Translation |
   |---|---|
   | argument that is a node handle, to a `Network` method | its node id (and it must belong to that network, otherwise `ValueError`) |
   | argument that is a node handle, to a `Node` method | the raw node |
   | argument that is a `Network` handle | the raw network |
   | list, tuple or set of node handles, to a `Network` method | the same container of ids |
   | result of `add_node`, `add_node_with_id`, `find_node`, `input_of`, `display_node` | a node handle (`None` for "no node") |
   | result of `node_ids`, `consumers_of`, `execution_order` | a list of node handles |
   | result of `dependents_of` | a set of node handles |
   | result that is a raw `Node` (from `node(...)`) or `Network` (from `parent()`) | a handle |

   Because the check happens when the call is made, `f = node.set_float` followed later by `f(...)` is safe too.
5. **Module entry.** `pyModule.cpp` is the standalone module (`PYBIND11_MODULE(jitnodes, m)`, built as `jitnodes.pyd`). An
   application embeds the same thing with `PYBIND11_EMBEDDED_MODULE(jitnodes, m) { jitnodes::python::registerModule(m); }`.

## What was added to jitnodes

Core library (small, comments and two members; nothing on the cook path):

| Where | Change |
|---|---|
| `graph/network.h` | `std::weak_ptr<void> Network::lifetime() const`: expires when the network is destroyed (backed by a `shared_ptr<void>` member created with the network). A subnet's inner network has its own, so it expires with its subnet node. |
| `graph/network.h`, `src/graph/network.cpp` | `std::uint64_t Network::generation() const`: incremented by `clear()` (a load clears first). Needed because `clear()` restarts both node ids and node uids, see [Object lifetime](#object-lifetime). |
| `graph/network.h` | `// @scriptable release_gil` on `cook`, `cookDisplay`, `freezeNode`. |
| many headers | `// @scriptable` and `// @scriptable ignore=true` comments (see above). No behaviour change. |
| `tests/testMain.cpp` | tests for the lifetime token (top level and nested) and the generation counter. |

New, outside the library, under `python/`:

| File | Purpose |
|---|---|
| `pyHandles.h`, `pyHandles.cpp` | `NetworkRef`, `PyNetwork`, `PyNode`, `PyHistory`, `StaleHandleError`, `registerModule()`, `wrapNetwork()` |
| `pyModule.cpp` | the standalone `jitnodes` module |
| `CMakeLists.txt` | targets `jitnodesPython` (static library for an embedding application), `pyjitnodes` (the module), tests; option `JITNODES_BUILD_PYTHON` in the top-level `CMakeLists.txt` |
| `tests/test_handles.py`, `tests/runScripts.py` | Python tests, run against the built module |
| `tests/embeddedTests.cpp` | C++ tests with an embedded interpreter |

The engine itself did not change: no Python include, no extra work per call, no change to cooking, fusion or undo. A build
without `JITNODES_BUILD_PYTHON` is the same library as before.

## Object lifetime

### The problem

The engine hands out raw pointers and references: `Network::node(id)` returns a `Node&` that lives inside the network's
own storage. In C++ that is fine, because the caller knows when it is valid. A script does not: it can keep a variable
for as long as it likes. These things invalidate a `Node&`, and all of them are easy to do from a script:

* `remove_node`, or the node being removed by `clear()` or by loading a file;
* **undo and redo**: `History::undo` rebuilds only the nodes that differ, so it *deletes and recreates node objects*. A
  script holding a node across `undo()` would crash, even though the network is alive. The same goes for rolling back
  a transaction that created nodes;
* the whole network going away: an application closing a document while a script still has its network or nodes;
* **id reuse**: `clear()` (and a load) restart the ids and uids from 1, so a stale reference to "node 3" can silently
  become a different node 3, and a freed network's address can be handed to a new network;
* the interpreter shutting down while objects still exist.

If the bindings exposed the raw classes, each of these would be a crash or, worse, silent corruption. Simply sharing
ownership of the `Network` (`shared_ptr`) would not help: nodes are still replaced by undo, and the engine's
internal back-pointers (a node to its network, a subnet to its parent) are raw.

### The design: handles that check on every call

A script never holds a pointer into the engine. It holds a handle:

| Handle | Holds | Valid while |
|---|---|---|
| `Network` | a `NetworkRef`: raw pointer, the network's `lifetime()` token, and (for a network the script created) a `shared_ptr` that owns it | the token has not expired |
| `Node` | its `NetworkRef`, the node id, the network's `generation` when the handle was made | the network is alive, the generation is unchanged, and the id still exists |
| `History` | its `NetworkRef` and the `History` object | the network is alive |

Every operation goes through `get()`, which throws `StaleHandleError` when the check fails, and then looks the node up by id
*at that moment*. So:

* A removed node, a node undone away, a cleared network, a destroyed network: the handle raises
  `jitnodes.StaleHandleError` (a subclass of `ReferenceError`, like Blender's removed-object error). `valid` is `False` and
  `repr` shows `(gone)`. Nothing crashes.
* A handle **follows its id**: after `undo()` recreates node 3, the handle to node 3 works again. After `redo()` removes it,
  it is stale again.
* The token is a `weak_ptr`, not an address, so a new network at the same address does not revive an old handle (the
  embedded tests create 300 networks, most at the previous one's address, and check that no old handle becomes valid).
* The **generation** catches what ids and tokens cannot: `clear()` keeps the network alive and restarts the ids, so after
  `net.clear(); net.add_node(...)` an old handle to node 1 would otherwise point at the new node 1. The old handle is
  stale; the new one is valid and compares unequal to it.
* Handles compare and hash by (network, id, generation), so `net["a"] == net["a"]` holds and a node from another
  network never compares equal.

### Who owns what

| Case | Owner | What the handle does |
|---|---|---|
| A script creates the network (`jitnodes.Network()`) | the handles themselves: each holds a `shared_ptr`. The network is destroyed when the last handle, history or bound method that refers to it is dropped | keeps it alive, so `jitnodes.Network().add_node("line")` is safe |
| An application owns the network (curvy's document) and gives it to Python with `wrapNetwork()` | the application | holds only the weak token: it never delays closing the document, and every handle goes stale when it is destroyed |
| `Geometry` returned by `cook` | `shared_ptr<const Geometry>`, shared with the node cache; only its const methods are bound, so a script cannot change it | outlives the network and the handles |

Bound methods count as handles: `add_node = jitnodes.Network().add_node` keeps the network alive, and calling it after the
network has gone (application-owned case) raises `StaleHandleError`.

### What is checked, what is not

Verified by the tests (`python/tests`, and `tests/testMain.cpp`):

* a removed node: read, write, method call, `cook(node)` and `in net` all behave;
* undo removing a node, redo bringing it back, and undo restoring a value;
* `clear()` followed by a new node with the same id;
* the token and generation in C++, including a subnet's inner network when its node is removed;
* an application-owned network destroyed while handles exist: network, node, `History`, `len`, lookups, methods and `repr`
  all go stale; the handles are still alive during interpreter shutdown, after the network, without a crash;
* a script-created network lives while a node, a bound method or a `History` refers to it, and is destroyed as soon as the
  last one goes (a node type that counts its destruction proves this), including 200 create-and-drop rounds with 1000 nodes;
* `Geometry` still works after its network and every handle are gone, and has no method that could change what the cache holds;
* a node from another network is refused by `connect` and similar calls, and two networks' handles never mix.

Not protected, by design:

* **Threads.** The engine is not thread safe for edits. A handle's check and the call that follows are not atomic: do not
  destroy or edit a network from another thread while a script uses it or while it cooks. `cook` releases the GIL (so
  progress callbacks, which run on worker threads, can call Python) but that does not make concurrent edits safe.
* **Explicit id reuse.** `add_node_with_id(i, ...)` after `remove_node(i)` makes a handle to the old node `i` valid again,
  exactly as undo does. That is the point of "a handle follows its id"; it is only a surprise if you reuse ids yourself.
* **Raw `_core` objects.** Anything you take from `jitnodes._core` directly is a raw reference with none of the above.
  Use the handles.

## The Python API

### Quick tour

```python
import jitnodes as jn

net = jn.Network()                       # a network the script owns; Network(threads=4) sets the cooking threads
line = net.add_node("line", "line")      # a Node handle (the name is optional)
wrangle = net.add_node("wrangle")
out = net.add_node("null")

line.count = 8                           # parameters read and write like attributes ...
line["length"] = 7.0                     # ... or like items
wrangle.code = "x = x * 2;"
line >> wrangle >> out                   # wire to input 0; returns the target, so it chains
net.set_display_node(out)

geometry = net.cook(out)                 # a Geometry; cooking releases the GIL
print(geometry.point_count())

history = jn.History(net)                # undo and redo
line.count = 4
history.commit("shorter")
history.undo()
print(line.count)                        # 8.0 again
```

### `jitnodes.Network`

| | |
|---|---|
| `Network(threads=0)` | create a network the script owns (0 picks a default from the hardware) |
| `net.valid` | `False` once the network is gone (application-owned networks) |
| `len(net)`, `for n in net`, `net.nodes()` | node count; handles in id order |
| `net["name"]`, `net[id]`, `net.node(key)` | the node with that name or id; `KeyError` if there is none |
| `"name" in net`, `node in net`, `id in net` | membership (a stale handle is not in it) |
| `net.add_node(type_name, name="")` | create a node, returns a handle; `net.add_node_with_id(id, type_name, name)` |
| `net.remove_node(node)`, `net.clear()` | remove one node; remove all (handles made before are stale afterwards) |
| `net.connect(a, b, input_index=0)`, `net.disconnect(b, input_index)` | wire `a` to an input of `b`; a cycle or a bad input raises `RuntimeError` |
| `net.find_node(name)`, `net.input_of(node, i)`, `net.display_node()` | handle, or `None` |
| `net.set_display_node(node)` | choose the node `cook_display()` cooks |
| `net.cook(node)`, `net.cook_display()`, `net.freeze_node(node)` | cook (releases the GIL); returns a `Geometry` |
| `net.node_ids()`, `net.execution_order(node)`, `net.consumers_of(node)`, `net.dependents_of([nodes])` | lists/sets of handles |
| `net.set_time(t)`, `net.time()`, `net.set_fps(f)`, `net.fps()` | animation time |
| `net.set_progress_callback(fn)`, `net.cancel()` | `fn(progress)` runs on worker threads while a cook is in progress |
| `net.rename_node(node, name)`, `net.unique_name(base)`, `net.profile_report()`, ... | everything else the engine's `Network` offers, in snake_case; see `_core.pyi` |

### `jitnodes.Node`

| | |
|---|---|
| `node.id`, `node.valid`, `node.network` | the id, whether it is still there, its network handle |
| `node.name()`, `node.type_name()`, `node.input_count()` | engine methods (forwarded) |
| `node.count`, `node["count"]` | read a float, string or geometry parameter; reading a float gives its value at the current time |
| `node.count = 8`, `node["count"] = 8` | write one; an unknown name is an `AttributeError` / `KeyError`, never a new attribute |
| `"count" in node` | whether the parameter exists |
| `a >> b` | wire `a` to the first input of `b`, returns `b` |
| `a.connect(b, input=0)`, `a.disconnect(input=0)` | wire to a given input of `b`; unwire an input of `a` |
| `node.set_float(name, v)`, `get_float`, `set_string`, `set_expression`, `set_keyframes`, `add_tag`, `set_bypassed`, ... | the engine's methods, forwarded (see `_core.pyi`) |

A name that is both an engine method and a parameter resolves to the method; the item form (`node["name"]`) always means
the parameter. Parameters of a node type are the ones that type declares; spare parameters are reached the same way.

### `jitnodes.History`

`History(network, max_states=100)` records undo states of a network. Then, forwarded from the engine:
`commit(label="")` (returns whether anything changed), `undo()`, `redo()`, `can_undo()`, `can_redo()`, `undo_label()`,
`redo_label()`, `size()`. Handles to nodes survive undo as described above.

### Values: `Geometry`, `Keyframe`, enums

* `Geometry` is what `cook` returns: a read-only view of the node cache's result, safe to keep after the network is gone. Only the
  const methods are bound (`readonly`, see below): `point_count()`, `primitive_count()`, `count(cls)`, `has_attrib(cls, name)`,
  `has_point_attrib(name)`, `has_shape_keys()`, `shape_key(prim)`, `shape_of_primitive()`, `in_group(...)`, `detail(name)`,
  `details()`. `Geometry()` makes an empty one. The attribute arrays are described under Limitations. `geometry.render_png(path, width=0, height=0)`
  renders the points that have a `radius` as soft sprites with the settings of a `render` node in the chain (like `jitnodesCli --png`) and returns
  the size; the GIL is released while it draws.
* Example: `examples/python/nightSky.py <folder with the module> [out.png]` builds a 38-node night sky (field stars, Milky Way, a spiral galaxy,
  the Big Dipper driven by a table, a shooting star, a crescent moon) and renders it to `nightSky.png` (about 0.3 s). It is explained, with
  the exact command to run it and the image, in [pythonExample.md](pythonExample.md).
* `Keyframe()` has `time`, `value`, `interp`; `node.set_keyframes("count", [k1, k2])` animates a parameter.
* Enums: `AttribClass`, `AttribType`, `PrimKind`, `KeyInterp` (for example `KeyInterp.smooth`).

### Errors

| Exception | When |
|---|---|
| `jitnodes.StaleHandleError` (a `ReferenceError`) | a handle's network, node or history is gone |
| `KeyError` | `net["missing"]`, `node["missing"]` |
| `AttributeError` | an unknown member or parameter name |
| `ValueError` | a node from another network was passed to a network method |
| `RuntimeError` | the engine refused: a cycle, a bad input index, a parse error in an expression, ... |
| `TypeError` | a key that is not a node, an id or a name |

## Embedding in an application

```cpp
#include <pybind11/embed.h>
#include "pyHandles.h"

PYBIND11_EMBEDDED_MODULE(jitnodes, m) { jitnodes::python::registerModule(m); }

void runScript(jitnodes::Network& network, const std::string& code) {
    pybind11::dict scope;
    scope["net"] = jitnodes::python::wrapNetwork(network);   // a weak handle: it never keeps the network alive
    pybind11::exec(code, scope);
}
```

* Link `jitnodesPython` (it brings `jitnodes`, the generated bindings and pybind11) and `pybind11::embed`.
* `wrapNetwork` imports the `jitnodes` module itself, so the embedded module only has to be defined.
* Hand an application's network to Python with `wrapNetwork`, never by giving a script a raw pointer or a shared owner:
  that is what lets the document close while a script variable still exists.
* Read a Python error's message (`error_already_set::what()`) while the interpreter is still alive, not after
  `scoped_interpreter` has been destroyed.
* **`PythonRuntime`** (`python/pyRuntime.h`) is the supported way to start the interpreter; it replaces `py::scoped_interpreter` and the
  `PYBIND11_EMBEDDED_MODULE` line above (it registers `jitnodes` itself):

  ```cpp
  jitnodes::python::PythonRuntime runtime({pythonHome, {scriptsFolder, userPackagesFolder}});
  pybind11::dict scope = runtime.newScope();       // __builtins__ and jitnodes imported
  scope["net"] = jitnodes::python::wrapNetwork(network);
  pybind11::exec(code, scope);
  ```

  It is isolated (`PyConfig_InitIsolatedConfig`): only the bundled `home` is used (standard library, `DLLs`, `site-packages`, then the extra
  search paths), so `PYTHONHOME`, `PYTHONPATH`, a system Python, the user site folder and the registry are ignored; `site` is not imported,
  signal handlers are not installed (the host keeps Ctrl-C), and `sys.executable` is the bundled `python.exe` so `multiprocessing` cannot
  start the application again. A folder that is not a Python home throws `std::runtime_error` with the reason.
* Thread model (chosen with the runtime, option D): create and destroy the runtime on the thread that runs scripts, normally the main thread.
  That thread keeps the GIL; cooking releases it while worker threads run, and progress callbacks take it back. Scripts use a network from one
  thread at a time. Destroy every `py::object` the application holds before the runtime (it finalizes the interpreter). Only one runtime can
  exist in a process, ever: a second one throws.
* The application still needs `python313.dll` next to the executable or on the path.

## Building and testing

```
cmake -S . -B out/jitpy -G Ninja -DCMAKE_BUILD_TYPE=Release -DJITNODES_BUILD_PYTHON=ON
cmake --build out/jitpy
ctest --test-dir out/jitpy
```

Run from a Visual Studio developer prompt on Windows. It needs:

* the **full pybind11 source tree** (default `../../pybind11`, option `JITNODES_PYBIND11_DIR`);
* a **Python 3.13 x64 SDK** with `include/` and `libs/` (default `../../python`, option `JITNODES_PYTHON_DIR`), for example
  python-build-standalone. The CMake file sets `PYBIND11_FINDPYTHON` and the `Python_*` find options so it uses that one and not
  the system Python;
* **newui's scriptgen** (`JITNODES_NEWUI_DIR`, default `../newui`) with the reflectgen venv, which generates the bindings.

Tests (all registered with `ctest`):

| Test | What it covers |
|---|---|
| `jitnodesTests` | the engine suite, including the lifetime token and generation counter |
| `jitnodesPythonScripts` | `python/tests/test_handles.py`: handles, parameters, wiring, stale handles after remove / undo / clear, ownership (nodes, bound methods and histories keep a script's network alive; geometry outlives it), history, keyframes, cooking from another thread with a progress callback |
| `jitnodesPythonEmbedded` | `python/tests/embeddedTests.cpp`: an application-owned network (handles do not keep it alive, all go stale), address reuse, script-created networks really destroyed when the last handle goes, shutdown with live handles |

### Running under AddressSanitizer

```
cmake -S . -B out/jitpy-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DJITNODES_BUILD_PYTHON=ON -DJITNODES_SANITIZE=ON ^
      -DJITNODES_BUILD_EXAMPLES=OFF -DJITNODES_BUILD_TOOLS=OFF
cmake --build out/jitpy-asan
```

In a Visual Studio developer prompt (so `clang_rt.asan_dynamic-x86_64.dll` is on `PATH`) with `PYTHONHOME` set to the Python folder:

* `out/jitpy-asan/python/jitnodesPythonEmbeddedTests.exe`: the strong check, because the whole program, including the embedded
  interpreter's callers, is instrumented.
* `out/jitpy-asan/jitnodesTests.exe`: the engine suite.
* The Python tests against the instrumented module: Python 3.8+ does not search `PATH` for an extension's DLLs, so call
  `os.add_dll_directory(<folder of the asan dll>)` before importing `jitnodes` (a small wrapper script that then runs
  `tests/runScripts.py`). Only the module is instrumented, not `python.exe`, so this covers less than the embedded run.

Notes: Debug needs `/RTC` off for ASAN, so use RelWithDebInfo; the generated bindings need `/bigobj` when instrumented (set in
`python/CMakeLists.txt`); MSVC's ASAN has no leak detector (the counting node in the embedded tests is what proves objects are
freed); ASAN delays reuse of freed memory, so the "new network at the same address" test sees almost no reuse there (it sees
about 296 of 300 in a normal build), which is why that test runs in the normal build too.

Result (2026-11-15): embedded tests 27 checks, engine tests 233, Python tests 34, all with no ASAN report.

The output of the build also contains `jitnodes.<abi>.pyd` (import it with the folder on `sys.path`) and `python/jitnodes/_core.pyi`.

## Limitations and what is not done

* **Stubs for the handles.** The build writes `python/jitnodes/__init__.pyi` (next to `_core.pyi`) with `genHandleStubs.py`, which derives the
  `Network`, `Node`, `History` and `Geometry` stubs from `_core.pyi` and applies the translations `pyHandles.cpp` makes: node ids become `Node`
  handles in results (`add_node`, `node_ids`, `find_node` -> `Node | None`, ...), `Node | int` is accepted where the engine takes an id, and
  parameters are `Any` through `__getattr__`/`__setattr__`. The `Geometry` stub includes the NumPy methods. If a result or argument translation
  is added to `pyHandles.cpp`, add the name to the tables at the top of the script (`test_stubs.py` checks that every declared member exists).
  Parameter names of a node type are not known to the stub, so `node.count` is `Any`.
* **Attribute arrays are read-only.** `geometry.point_attrib("x")`, `prim_attrib(name)` and `attrib(AttribClass, name)` return zero-copy, non-writeable NumPy views (float32, or int32 for int and string attributes; `strings()` and `string_values()` decode string ids). Each view keeps its geometry alive through a capsule, so it stays valid after the network is gone. NumPy is only needed when a view is requested (the module imports without it; the call raises ImportError). Writable arrays are not done: they would dangle when the point count changes. Detail values come from `details()`/`detail()`.
* **The surface is wide.** Everything public on `Network` and `Node` is forwarded, including engine internals such as
  `note_touched` or `take_uid`. Mark more members `ignore=true` to hide them.
* **Subnets** are reachable only as nodes. `SubnetNode` and `AssetNode` are not bound, so a script cannot edit a subnet's inner
  network yet.
* **No Python-authored nodes** and no undo transactions (`with doc.transaction(): ...`) yet; see
  `curvy/docs/py_scripting_plan.md` for the plan.
* **Not thread safe**, see above. Free-threaded Python builds are not supported or tested.
* **Windows x64 only** so far (Python SDK, venv path and the MSVC build were set up and tested there).
