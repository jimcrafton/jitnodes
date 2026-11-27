// Python embedded in an application. Checks what only an embedding shows: a network the application owns (handles must not
// keep it alive and must go stale when it is destroyed), when a network a script made is really destroyed, and that nothing
// is left behind or reused wrongly after many networks come and go.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <pybind11/embed.h>

#include "jitnodes/jitnodes.h"
#include "pyHandles.h"
#include "pyRuntime.h"

namespace py = pybind11;

namespace {

int failures = 0;
std::atomic<int> probesAlive{0};

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("[FAIL] %s\n", what.c_str());
        ++failures;
    } else {
        std::printf("[ ok ] %s\n", what.c_str());
    }
}

// A node that counts the instances alive, so a test can tell when the engine really freed a network. Alive, not destroyed: undo history
// holds clones of the nodes, which come and go on their own.
class ProbeNode : public jitnodes::Node {
public:
    ProbeNode() : Node("probe", 0) { ++probesAlive; }
    ~ProbeNode() override { --probesAlive; }
    jitnodes::GeometryHandle cook(jitnodes::CookContext&, const std::vector<jitnodes::GeometryHandle>&) override {
        return std::make_shared<jitnodes::Geometry>();
    }
};

void run(py::dict& scope, const char* code) {
    py::exec(code, scope);
    py::exec("import gc; gc.collect()", scope);
}

bool flag(py::dict& scope, const char* name) {
    return scope[name].cast<bool>();
}

void applicationOwnedNetwork() {
    auto network = std::make_unique<jitnodes::Network>(1);
    const std::weak_ptr<void> lifetime = network->lifetime();
    network->addNode("line", "first");

    py::dict scope;
    scope["net"] = jitnodes::python::wrapNetwork(*network);
    run(scope, R"py(
import jitnodes
node = net["first"]
node.count = 3
history = jitnodes.History(net)
history.commit("start")
ok = net.valid and node.valid and len(net) == 1 and node.count == 3.0
)py");
    check(flag(scope, "ok"), "a script reads and edits the application's node");
    check(network->node(network->findNode("first")).getFloat("count") == 3.0f, "the edit reached the application's node");
    check(!lifetime.expired(), "the network is alive while the handles exist");

    run(scope, R"py(
def is_stale(action):
    try:
        action()
    except jitnodes.StaleHandleError:
        return True
    return False
)py");

    network.reset();
    check(lifetime.expired(), "handles do not keep the application's network alive");

    run(scope, R"py(
results = {
    "network invalid": not net.valid,
    "node invalid": not node.valid,
    "node read": is_stale(lambda: node.count),
    "node write": is_stale(lambda: setattr(node, "count", 1)),
    "node method": is_stale(lambda: node.name()),
    "network len": is_stale(lambda: len(net)),
    "network method": is_stale(lambda: net.node_count()),
    "network lookup": is_stale(lambda: net["first"]),
    "history method": is_stale(lambda: history.can_undo()),
    "history commit": is_stale(lambda: history.commit("after")),
    "repr": "(gone)" in repr(net) and "(gone)" in repr(node),
}
)py");
    for (const auto item : scope["results"].cast<py::dict>()) {
        check(item.second.cast<bool>(), "after the network is destroyed: " + item.first.cast<std::string>());
    }
    // node, net and history stay alive until the interpreter shuts down, after the network: that must not crash.
}

void aNewNetworkAtTheSameAddressDoesNotRevive() {
    py::dict scope;
    run(scope, "import jitnodes\nolds = []\n");
    int reused = 0;
    const void* previous = nullptr;
    constexpr int rounds = 300;
    std::unique_ptr<jitnodes::Network> network;
    for (int i = 0; i < rounds; ++i) {
        network = std::make_unique<jitnodes::Network>(1);
        if (network.get() == previous) {
            ++reused;
        }
        previous = network.get();
        network->addNode("line", "first");
        scope["net"] = jitnodes::python::wrapNetwork(*network);
        run(scope, "olds.append(net); olds.append(net['first'])");
        network.reset();
    }
    // One more network, alive, possibly at the address an old one had.
    network = std::make_unique<jitnodes::Network>(1);
    network->addNode("line", "first");
    scope["net"] = jitnodes::python::wrapNetwork(*network);
    run(scope, "fresh = net['first']\nstale = [h for h in olds if h.valid]\nfresh_ok = fresh.valid and net.valid");
    check(scope["stale"].cast<py::list>().size() == 0, "no handle to a destroyed network comes back (" + std::to_string(reused) + " of " +
                                                          std::to_string(rounds) + " networks reused the previous address)");
    check(flag(scope, "fresh_ok"), "the handles to the new network are valid");
}

void scriptOwnedNetworks() {
    jitnodes::NodeRegistry::instance().registerType("probe", [] { return std::make_unique<ProbeNode>(); });
    py::dict scope;
    run(scope, "import jitnodes\n");

    run(scope, "node = jitnodes.Network().add_node('probe')");
    check(probesAlive == 1, "a node handle keeps the network a script made alive after the Network variable is gone");
    run(scope, "alive_then = node.valid and node.network.valid\ndel node");
    check(flag(scope, "alive_then"), "and the node and its network answer");
    check(probesAlive == 0, "dropping the last handle destroys the network");

    run(scope, "bound = jitnodes.Network().add_node('probe').name");
    check(probesAlive == 1, "a bound method keeps the network alive");
    run(scope, "del bound");
    check(probesAlive == 0, "dropping the bound method destroys the network");

    run(scope, "net = jitnodes.Network()\nnet.add_node('probe')\nhistory = jitnodes.History(net)\ndel net");
    check(probesAlive >= 1, "a history keeps the network alive");
    run(scope, "answers = not history.can_undo()\ndel history");
    check(flag(scope, "answers") && probesAlive == 0, "and dropping it destroys the network and the history's copies");

    run(scope, "net = jitnodes.Network()\nfirst = net.add_node('probe')\nnet.remove_node(first)");
    check(probesAlive == 0, "a removed node is destroyed at once, even while its stale handle exists");
    run(scope, "del net, first");

    run(scope, "net = jitnodes.Network()\nnet.add_node('probe')\nnet.clear()");
    check(probesAlive == 0, "clear destroys the nodes");
    run(scope, "del net");

    constexpr int rounds = 200;
    constexpr int perNetwork = 5;
    run(scope, R"py(
for _ in range(200):
    net = jitnodes.Network()
    nodes = [net.add_node('probe') for _ in range(5)]
    net.cook(nodes[0])
    history = jitnodes.History(net)
del net, nodes, history
)py");
    check(probesAlive == 0, "no node of " + std::to_string(rounds) + " create-and-drop rounds of " + std::to_string(perNetwork) + " nodes is left alive (" +
                                std::to_string(probesAlive.load()) + " are)");
}

void runtimeIsIsolated(jitnodes::python::PythonRuntime& runtime, const std::filesystem::path& extraFolder) {
    py::dict scope = runtime.newScope();
    py::exec(R"py(
import sys, os, json
flags_isolated = sys.flags.isolated == 1
no_site = 'site' not in sys.modules
no_env_path = not any('bogus_pythonpath' in p for p in sys.path)
no_system = not any('Program Files' in p and 'Python' in p for p in sys.path) and not any('AppData' in p for p in sys.path)
stdlib_works = json.loads('[1]') == [1]
exe_is_bundled = os.path.basename(sys.executable).lower().startswith('python')
)py", scope);
    check(flag(scope, "flags_isolated"), "the interpreter is isolated");
    check(flag(scope, "no_site"), "site is not imported");
    check(flag(scope, "no_env_path"), "PYTHONPATH is ignored");
    check(flag(scope, "no_system"), "no system or user folder is on sys.path");
    check(flag(scope, "stdlib_works"), "the bundled standard library imports");
    check(flag(scope, "exe_is_bundled"), "sys.executable is the bundled interpreter");

    py::exec("import numpy\nnumpy_ok = numpy.arange(3).sum() == 3\nimport jitnodes\nhas_module = hasattr(jitnodes, 'Network')", scope);
    check(flag(scope, "numpy_ok"), "NumPy imports from the bundled site-packages");
    check(flag(scope, "has_module"), "the jitnodes module is built in");

    runtime.addSearchPath(extraFolder);
    py::exec("import runtime_probe\nprobe_ok = runtime_probe.VALUE == 42", scope);
    check(flag(scope, "probe_ok"), "a module in an added search path imports");
    check(runtime.version().rfind("3.", 0) == 0, "version() reports " + runtime.version());
}

void secondRuntimeIsRefused(const std::filesystem::path& home) {
    bool refused = false;
    try {
        jitnodes::python::PythonRuntime again({home, {}});
    } catch (const std::runtime_error&) {
        refused = true;
    }
    check(refused, "a second runtime is refused");
}

void badHomeIsRefused() {
    bool refused = false;
    std::string message;
    try {
        jitnodes::python::PythonRuntime runtime({std::filesystem::temp_directory_path() / "no_such_python_home", {}});
    } catch (const std::runtime_error& e) {
        refused = true;
        message = e.what();
    }
    check(refused && message.find("no Python standard library") != std::string::npos, "a folder without Python is refused with a reason");
}

int runAll(const std::filesystem::path& home) {
    badHomeIsRefused();

    const std::filesystem::path extra = std::filesystem::temp_directory_path() / "jitnodes_runtime_probe";
    std::filesystem::create_directories(extra);
    {
        std::ofstream(extra / "runtime_probe.py") << "VALUE = 42\n";
    }
#ifdef _WIN32
    _putenv_s("PYTHONPATH", "bogus_pythonpath");
#else
    setenv("PYTHONPATH", "bogus_pythonpath", 1);
#endif

    {
        jitnodes::python::PythonRuntime runtime({home, {}});
        try {
            runtimeIsIsolated(runtime, extra);
            secondRuntimeIsRefused(home);
            scriptOwnedNetworks();
            aNewNetworkAtTheSameAddressDoesNotRevive();
            applicationOwnedNetwork();
        } catch (const py::error_already_set& e) {
            // Read here, while the interpreter is still alive.
            std::printf("[FAIL] python error: %s\n", e.what());
            ++failures;
        }
    }
    std::printf("%d failed\n", failures);
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: jitnodesPythonEmbeddedTests <python home>\n");
        return 2;
    }
    try {
        return runAll(argv[1]);
    } catch (const std::exception& e) {
        std::printf("[FAIL] exception: %s\n", e.what());
        return 2;
    }
}
