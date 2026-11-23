#include "pyHandles.h"

// The standalone module: `import jitnodes` from any Python (a script, a test). An application that embeds
// Python registers the same thing with PYBIND11_EMBEDDED_MODULE(jitnodes, m) { jitnodes::python::registerModule(m); }.
PYBIND11_MODULE(jitnodes, m) {
    m.doc() = "jitnodes: node graphs of vector geometry";
    jitnodes::python::registerModule(m);
}
