#include "pyRuntime.h"

#include <stdexcept>

#include <pybind11/embed.h>

#include "pyHandles.h"

namespace py = pybind11;
namespace fs = std::filesystem;

// Registered with the interpreter's table of built-in modules when this file is linked in. The standalone jitnodes.pyd does not
// reference PythonRuntime, so the linker leaves this file out of it.
PYBIND11_EMBEDDED_MODULE(jitnodes, m) {
    jitnodes::python::registerModule(m);
}

namespace jitnodes::python {

namespace {

bool runtimeCreated = false;

void check(PyConfig& config, PyStatus status) {
    if (PyStatus_Exception(status) != 0) {
        const std::string message = PyStatus_IsError(status) != 0 && status.err_msg != nullptr ? status.err_msg : "out of memory";
        PyConfig_Clear(&config);
        throw std::runtime_error("Python configuration failed: " + message);
    }
}

void setString(PyConfig& config, wchar_t** field, const fs::path& value) {
    check(config, PyConfig_SetString(&config, field, value.wstring().c_str()));
}

void appendPath(PyConfig& config, const fs::path& folder) {
    check(config, PyWideStringList_Append(&config.module_search_paths, folder.wstring().c_str()));
}

struct Layout {
    fs::path executable;
    fs::path library;
    std::vector<fs::path> folders;   // the standard library, extension modules and site-packages, in search order
};

Layout layoutOf(const fs::path& home) {
    Layout layout;
#ifdef _WIN32
    layout.executable = home / "python.exe";
    layout.library = home / "Lib";
    layout.folders = {home / "Lib", home / "DLLs", home / "Lib" / "site-packages"};
#else
    const fs::path lib = home / "lib" / ("python" + std::to_string(PY_MAJOR_VERSION) + "." + std::to_string(PY_MINOR_VERSION));
    layout.executable = home / "bin" / "python3";
    layout.library = lib;
    layout.folders = {lib, lib / "lib-dynload", lib / "site-packages"};
#endif
    return layout;
}

}  // namespace

PythonRuntime::PythonRuntime(const PythonRuntimeOptions& options) {
    if (runtimeCreated) {
        throw std::runtime_error("a Python runtime was already created in this process; only one is supported");
    }
    const Layout layout = layoutOf(options.home);
    if (!fs::exists(layout.library / "os.py")) {
        throw std::runtime_error("no Python standard library under '" + options.home.string() + "' (expected " + layout.library.string() + ")");
    }

    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.parse_argv = 0;
    config.install_signal_handlers = 0;
    config.site_import = 0;
    setString(config, &config.home, options.home);
    // multiprocessing starts this file; pointing it at the bundled interpreter keeps it from launching the application again.
    setString(config, &config.executable, layout.executable);
    config.module_search_paths_set = 1;
    for (const fs::path& folder : layout.folders) {
        appendPath(config, folder);
    }
    for (const fs::path& folder : options.searchPaths) {
        appendPath(config, folder);
    }

    runtimeCreated = true;
    py::initialize_interpreter(&config, 0, nullptr, false);
}

PythonRuntime::~PythonRuntime() {
    py::finalize_interpreter();
}

py::dict PythonRuntime::newScope() const {
    py::dict scope;
    scope["__builtins__"] = py::module_::import("builtins");
    scope["jitnodes"] = py::module_::import("jitnodes");
    return scope;
}

void PythonRuntime::addSearchPath(const fs::path& folder) {
    py::module_::import("sys").attr("path").attr("append")(folder.string());
}

std::string PythonRuntime::version() const {
    const std::string full = Py_GetVersion();
    return full.substr(0, full.find(' '));
}

}  // namespace jitnodes::python
