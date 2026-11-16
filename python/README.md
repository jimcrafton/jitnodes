# jitnodes Python module

The Python module (`import jitnodes`): hand-written handles in `pyHandles.cpp` over bindings that newui's scriptgen
generates from the `// @scriptable` marks in the headers.

Everything is documented in [`../docs/python.md`](../docs/python.md): how the binding is built, what was added to jitnodes,
object lifetime (why scripts get handles and what makes a handle stale), the Python API, embedding in an application, building
and testing, and what is not done. The generator itself is documented in `newui/tools/scriptgen/README.md`.

Build with `-DJITNODES_BUILD_PYTHON=ON` (see `CMakeLists.txt` in this folder); tests are in `tests/`.
