#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>

namespace jitnodes::python {

struct PythonRuntimeOptions {
    // The folder of a bundled CPython (python.exe, Lib/, DLLs/ on Windows; lib/python3.x on other systems). Nothing else is looked at:
    // no PYTHONHOME or PYTHONPATH, no system installation, no user site folder, no registry.
    std::filesystem::path home;
    // Searched after the standard library and site-packages: the application's script folder, the user's packages.
    std::vector<std::filesystem::path> searchPaths;
};

// An isolated embedded interpreter with the `jitnodes` module built in. Create it on the thread that will run scripts (normally the
// main thread) and destroy it on the same thread: that thread holds the GIL for the runtime's whole life, and cooking releases it
// while worker threads run (a progress callback takes it back). Every Python object an application keeps (a py::dict scope, a py::object)
// must be released before the runtime is destroyed.
//
// One runtime per process: CPython cannot be fully unloaded, so a second one (also after the first is destroyed) throws.
class PythonRuntime {
public:
    // Throws std::runtime_error with the reason when `home` is not a Python folder or the interpreter does not start.
    explicit PythonRuntime(const PythonRuntimeOptions& options);
    ~PythonRuntime();

    PythonRuntime(const PythonRuntime&) = delete;
    PythonRuntime& operator=(const PythonRuntime&) = delete;

    // A fresh global scope for running code: __builtins__ and the `jitnodes` module imported.
    pybind11::dict newScope() const;

    // Appends a folder to sys.path.
    void addSearchPath(const std::filesystem::path& folder);

    // "3.13.18" and so on.
    std::string version() const;
};

}  // namespace jitnodes::python
