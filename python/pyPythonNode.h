#pragma once

#include <chrono>
#include <functional>

#include <pybind11/pybind11.h>

#include "jitnodes/graph/node.h"

namespace jitnodes::python {

// A "python" node runs the code in its `code` parameter when it cooks. Code that arrives in a file is code somebody else wrote, so no
// python node cooks until the application says so: the policy is asked on every cook (from any thread, so it must be thread safe) and,
// with none set or when it answers false, the node fails with a message instead of running. There is no filtering or sandbox of what the
// code does; allowing a node means trusting its code.
using PythonNodePolicy = std::function<bool(const Node&)>;

// What the cook fails with while the policy refuses a node: an editor recognises a refusal by this text (it starts the message) and can offer to
// allow the code.
inline constexpr const char* pythonNodeNotAllowedMessage = "Python nodes are not allowed here: the application has not trusted this code";

// An empty policy allows nothing (the default).
void setPythonNodePolicy(PythonNodePolicy policy);
bool pythonNodeAllowed(const Node& node);

// A python node that runs longer than the limit is stopped: a watchdog thread raises jitnodes.NodeInterrupted (a BaseException, so `except
// Exception` does not swallow it) in the node's thread, and the cook fails with a message. Python raises it between bytecodes, so a long call
// that does not return to Python (time.sleep, one huge NumPy operation) is stopped when it returns. The limit counts from the moment the node
// holds the interpreter, not from the moment it was queued. Zero means no limit; the default is 10 seconds.
void setPythonNodeTimeLimit(std::chrono::milliseconds limit);
std::chrono::milliseconds pythonNodeTimeLimit();

// The same stop for the host: polled by the watchdog (every 20 ms, from its own thread, so it must be thread safe and quick) while a python
// node runs; true stops the running node as the time limit does. curvy answers true while Esc is held.
void setPythonNodeInterruptCheck(std::function<bool()> check);

// Network::cancel() stops a running python node too (the cook then ends as cancelled, not as a failure).
//
// A node that was stopped (limit or host) does not run again until its code, a parameter or an input changes, so a repaint does not restart a
// runaway script.

// Registers the node type "python" and the class jitnodes.MutableGeometry, and installs the cook thread hooks that release the interpreter
// lock while workers cook. Called by registerModule.
void registerPythonNode(pybind11::module_& module);

}  // namespace jitnodes::python
