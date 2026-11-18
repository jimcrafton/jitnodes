#include "pyPythonNode.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include "jitnodes/geo/geometry.h"
#include "jitnodes/graph/network.h"

namespace py = pybind11;

namespace jitnodes::python {

namespace {

std::mutex policyMutex;
std::shared_ptr<const PythonNodePolicy> currentPolicy;

py::module_ importNumpy() {
    try {
        return py::module_::import("numpy");
    } catch (const py::error_already_set& error) {
        if (error.matches(PyExc_ImportError)) {
            throw py::import_error("NumPy is required for attribute arrays (pip install numpy)");
        }
        throw;
    }
}

}  // namespace

void setPythonNodePolicy(PythonNodePolicy policy) {
    std::lock_guard<std::mutex> lock(policyMutex);
    currentPolicy = policy ? std::make_shared<const PythonNodePolicy>(std::move(policy)) : nullptr;
}

bool pythonNodeAllowed(const Node& node) {
    std::shared_ptr<const PythonNodePolicy> policy;
    {
        std::lock_guard<std::mutex> lock(policyMutex);
        policy = currentPolicy;
    }
    return policy != nullptr && (*policy)(node);
}

namespace {

// The geometry a python node edits: a copy of its first input (the attribute storage is shared with the input until the first write, which
// copies it), handed to the script for the length of one cook. Writable NumPy views of its attributes follow these rules:
//  - a view is writable only while the cook runs; finish() and close() clear the flag of every view that is still alive;
//  - resize_points() is refused while a view is alive, because the storage would move under it;
//  - a view that the script kept (a global, a slice) must not reach the cached result, so finish() hands out an unshared copy then.
class MutableGeometry {
public:
    explicit MutableGeometry(std::shared_ptr<Geometry> geometry) : geometry_(std::move(geometry)) {}

    Geometry& geometry() {
        requireOpen();
        return *geometry_;
    }

    bool isOpen() const { return open_; }

    py::object view(AttribClass cls, const std::string& name) {
        Geometry& geo = geometry();
        const AttribArray* found = geo.findAttrib(cls, name);
        if (found == nullptr) {
            throw py::key_error("no " + std::string(cls == AttribClass::point ? "point" : "primitive") + " attribute '" + name + "'");
        }
        AttribArray& attrib = geo.attrib(cls, name, found->type());
        // data() detaches storage shared with the input (or with the cache), so a write never reaches another geometry.
        void* data = attrib.data();
        py::module_ numpy = importNumpy();
        py::capsule owner(new std::shared_ptr<Geometry>(geometry_), [](void* p) { delete static_cast<std::shared_ptr<Geometry>*>(p); });
        const py::dtype dtype(attrib.type() == AttribType::float32 ? "float32" : "int32");
        py::array array(dtype, {static_cast<py::ssize_t>(attrib.size())}, {static_cast<py::ssize_t>(sizeof(float))}, data, owner);
        dropDeadViews();
        views_.push_back({py::weakref(array), cls});
        return std::move(array);
    }

    void resizePoints(size_t count) {
        Geometry& geo = geometry();
        requireNoViews(AttribClass::point, "resize_points");
        geo.resizePoints(count);
    }

    // Adds a primitive over points that exist (a cubic one must have the point count its layout needs). The primitive attributes grow by one, so
    // an array from prim_attrib may not be alive. A key makes it part of the shape with that key (see Geometry::addShapePrimitive).
    void addPrimitive(uint32_t start, uint32_t count, bool closed, PrimKind kind, const std::string& key) {
        Geometry& geo = geometry();
        Primitive primitive;
        primitive.start = start;
        primitive.count = count;
        primitive.closed = closed;
        primitive.kind = kind;
        checkPrimitive(geo, primitive, "add_primitive");
        requireNoViews(AttribClass::primitive, "add_primitive");
        geo.addShapePrimitive(primitive, key);
    }

    AttribArray& stringAttrib(AttribClass cls, const std::string& name) {
        Geometry& geo = geometry();
        const AttribArray* found = geo.findAttrib(cls, name);
        if (found == nullptr) {
            throw py::key_error("no " + std::string(cls == AttribClass::point ? "point" : "primitive") + " attribute '" + name + "'");
        }
        if (found->type() != AttribType::string) {
            throw py::type_error("'" + name + "' is not a string attribute");
        }
        return geo.attrib(cls, name, AttribType::string);
    }

    // The ids of a string attribute stay in the array a view shows, so an array from attrib() sees the change.
    void setString(AttribClass cls, const std::string& name, size_t index, const std::string& text) {
        AttribArray& attrib = stringAttrib(cls, name);
        if (index >= attrib.size()) {
            throw py::index_error("element " + std::to_string(index) + " of '" + name + "' does not exist");
        }
        const int32_t id = attrib.intern(text);
        attrib.ints()[index] = id;
    }

    void setStringValues(AttribClass cls, const std::string& name, const std::vector<std::string>& values) {
        AttribArray& attrib = stringAttrib(cls, name);
        if (values.size() != attrib.size()) {
            throw py::value_error("'" + name + "' has " + std::to_string(attrib.size()) + " elements, " + std::to_string(values.size()) + " given");
        }
        std::vector<int32_t> ids;
        ids.reserve(values.size());
        for (const std::string& text : values) {
            ids.push_back(attrib.intern(text));
        }
        int32_t* target = attrib.ints().data();
        for (size_t i = 0; i < ids.size(); ++i) {
            target[i] = ids[i];
        }
    }

    // The cook is over: the views stop being writable. Returns true when one of them is still referenced by the script.
    bool freezeViews() {
        bool survivors = false;
        for (View& entry : views_) {
            py::object array = entry.ref();
            if (!array.is_none()) {
                survivors = true;
                array.attr("flags").attr("writeable") = false;
            }
        }
        views_.clear();
        open_ = false;
        return survivors;
    }

    GeometryHandle finish() {
        requireOpen();
        const bool survivors = freezeViews();
        geometry_->checkStrings();
        geometry_->checkShapes();
        for (const Primitive& primitive : geometry_->primitives()) {
            checkPrimitive(*geometry_, primitive, "the result");   // points may have been removed after the primitive was added
        }
        if (!survivors) {
            return geometry_;
        }
        auto copy = std::make_shared<Geometry>(*geometry_);
        for (AttribClass cls : {AttribClass::point, AttribClass::primitive}) {
            for (const auto& entry : copy->attribs(cls)) {
                copy->attrib(cls, entry.first, entry.second.type()).data();
            }
        }
        return copy;
    }

private:
    void requireOpen() const {
        if (!open_) {
            throw std::runtime_error("this geometry belongs to a cook that has finished");
        }
    }

    static void checkPrimitive(const Geometry& geo, const Primitive& primitive, const char* what) {
        if (primitive.count == 0 || static_cast<size_t>(primitive.start) + primitive.count > geo.pointCount()) {
            throw std::runtime_error(std::string(what) + ": a primitive of " + std::to_string(primitive.count) + " points from point " +
                                     std::to_string(primitive.start) + " does not fit the " + std::to_string(geo.pointCount()) + " points");
        }
        if (primitive.kind == PrimKind::cubic && cubicSegments(primitive) == 0) {
            throw std::runtime_error(std::string(what) + ": a cubic primitive needs 3n+1 points (open) or 3n (closed), not " + std::to_string(primitive.count));
        }
    }

    // A change of length moves the storage of every attribute of the class, so no array of it may be alive.
    void requireNoViews(AttribClass cls, const char* what) {
        dropDeadViews();
        for (const View& entry : views_) {
            if (entry.cls == cls) {
                throw std::runtime_error(std::string(what) + ": an array from " + (cls == AttribClass::point ? "point_attrib" : "prim_attrib") +
                                         "/attrib is still alive; del it (and any slice of it) first");
            }
        }
    }

    void dropDeadViews() {
        std::vector<View> alive;
        for (View& entry : views_) {
            if (!entry.ref().is_none()) {
                alive.push_back(std::move(entry));
            }
        }
        views_ = std::move(alive);
    }

    struct View {
        py::weakref ref;
        AttribClass cls;
    };

    std::shared_ptr<Geometry> geometry_;
    std::vector<View> views_;
    bool open_ = true;
};

std::atomic<long long> timeLimitMilliseconds{10000};
std::mutex interruptMutex;
std::shared_ptr<const std::function<bool()>> interruptCheck;

// Never destroyed: it is made while Python runs and handed to the watchdog thread, which may outlive the interpreter's last use.
py::object* interruptType = nullptr;

enum class StopReason { none, timeLimit, cancelled, interrupted };

// One running python node, seen by the watchdog.
struct Run {
    unsigned long thread = 0;
    std::chrono::steady_clock::time_point started;
    const Network* network = nullptr;
    std::atomic<bool> active{true};
    std::atomic<int> reason{static_cast<int>(StopReason::none)};
    std::chrono::steady_clock::time_point lastRaised;
};

// One thread for every python node that runs. While a node runs it looks, every 20 ms, at the time limit, the network's cancel flag and the
// host's check, and raises NodeInterrupted in the node's thread. It sleeps while nothing runs.
class Watchdog {
public:
    static Watchdog& instance() {
        static Watchdog watchdog;
        return watchdog;
    }

    void add(const std::shared_ptr<Run>& run) {
        std::lock_guard<std::mutex> lock(mutex_);
        runs_.push_back(run);
        if (!thread_.joinable()) {
            thread_ = std::thread([this]() { loop(); });
        }
        wake_.notify_all();
    }

    void remove(const std::shared_ptr<Run>& run) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < runs_.size(); ++i) {
            if (runs_[i] == run) {
                runs_.erase(runs_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
    }

    ~Watchdog() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    static StopReason reasonToStop(const Run& run) {
        const long long limit = timeLimitMilliseconds.load();
        if (limit > 0 && std::chrono::steady_clock::now() - run.started >= std::chrono::milliseconds(limit)) {
            return StopReason::timeLimit;
        }
        if (run.network != nullptr && run.network->isCancelRequested()) {
            return StopReason::cancelled;
        }
        std::shared_ptr<const std::function<bool()>> check;
        {
            std::lock_guard<std::mutex> lock(interruptMutex);
            check = interruptCheck;
        }
        if (check != nullptr && (*check)()) {
            return StopReason::interrupted;
        }
        return StopReason::none;
    }

    void loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            if (runs_.empty()) {
                wake_.wait(lock);
                continue;
            }
            wake_.wait_for(lock, std::chrono::milliseconds(20));
            const std::vector<std::shared_ptr<Run>> snapshot = runs_;
            lock.unlock();
            for (const std::shared_ptr<Run>& run : snapshot) {
                if (!run->active.load()) {
                    continue;
                }
                const auto now = std::chrono::steady_clock::now();
                const bool raisedBefore = run->reason.load() != static_cast<int>(StopReason::none);
                // A script that catches the exception is told again, but not on every look.
                if (raisedBefore && now - run->lastRaised < std::chrono::milliseconds(250)) {
                    continue;
                }
                const StopReason reason = raisedBefore ? static_cast<StopReason>(run->reason.load()) : reasonToStop(*run);
                if (reason == StopReason::none) {
                    continue;
                }
                // The node's thread gives the lock up every few milliseconds while it runs Python code. By the time this thread has it the
                // node may be over (the run's thread would then be running something else), so the flag is looked at again.
                py::gil_scoped_acquire gil;
                if (run->active.load()) {
                    run->reason = static_cast<int>(reason);
                    run->lastRaised = now;
                    PyThreadState_SetAsyncExc(run->thread, interruptType->ptr());
                }
            }
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<std::shared_ptr<Run>> runs_;
    std::thread thread_;
    bool stop_ = false;
};

// Registers the run with the watchdog while a node's code executes, and takes it out (with any exception that was raised too late to land) when
// it ends. Made and destroyed with the interpreter lock held.
class RunGuard {
public:
    RunGuard(const Network* network) : run_(std::make_shared<Run>()) {
        run_->thread = PyThread_get_thread_ident();
        run_->started = std::chrono::steady_clock::now();
        run_->network = network;
        Watchdog::instance().add(run_);
    }

    ~RunGuard() {
        run_->active = false;
        Watchdog::instance().remove(run_);
        // An exception raised just before the end stays pending for the thread's next Python code, which is not ours.
        PyThreadState_SetAsyncExc(run_->thread, nullptr);
    }

    StopReason reason() const { return static_cast<StopReason>(run_->reason.load()); }

private:
    std::shared_ptr<Run> run_;
};

std::string seconds(long long milliseconds) {
    const double value = static_cast<double>(milliseconds) / 1000.0;
    std::string text = std::to_string(value);
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

class PythonNode : public Node {
public:
    PythonNode() : Node("python", 1) {
        declareString("code", "");
        for (const char* name : {"a", "b", "c", "d"}) {
            declareFloat(name, 0.0f);
        }
    }

    bool hasVariableInputs() const override { return true; }
    bool inputsOptional() const override { return true; }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& inputs) override {
        if (!pythonNodeAllowed(*this)) {
            throw std::runtime_error("Python nodes are not allowed here: the application has not trusted this code");
        }
        const std::string signature = runSignature(inputs);
        if (!stoppedSignature_.empty() && signature == stoppedSignature_) {
            throw std::runtime_error(stoppedMessage_ + " (change the code, a parameter or an input to run it again)");
        }
        py::gil_scoped_acquire gil;
        RunGuard guard(ctx.network);
        MutableGeometry* mine = nullptr;
        py::object mineObject;
        try {
            py::module_ builtins = py::module_::import("builtins");
            py::dict scope;
            scope["__builtins__"] = builtins;

            py::list inputList;
            for (const GeometryHandle& input : inputs) {
                if (input != nullptr) {
                    inputList.append(py::cast(std::const_pointer_cast<Geometry>(input)));
                }
            }
            const GeometryHandle first = inputs.empty() ? nullptr : inputs.front();
            mineObject = py::cast(MutableGeometry(first != nullptr ? std::make_shared<Geometry>(*first) : std::make_shared<Geometry>()));
            mine = mineObject.cast<MutableGeometry*>();

            py::dict floats;
            for (const std::string& name : floatNames()) {
                floats[py::str(name)] = getFloat(name);
            }
            py::dict strings;
            for (const std::string& name : stringNames()) {
                if (name != "code") {
                    strings[py::str(name)] = getString(name);
                }
            }
            scope["geo"] = mineObject;
            scope["inputs"] = inputList;
            scope["params"] = floats;
            scope["strings"] = strings;

            py::object compiled = builtins.attr("compile")(getString("code"), "<python node " + name() + ">", "exec");
            builtins.attr("exec")(compiled, scope);

            py::object result = scope["geo"];
            MutableGeometry* out = result.cast<MutableGeometry*>();
            GeometryHandle handle = out->finish();
            if (out != mine && mine->isOpen()) {
                mine->freezeViews();
            }
            return handle;
        } catch (const py::error_already_set& error) {
            if (mine != nullptr && mine->isOpen()) {
                mine->freezeViews();
            }
            if (interruptType != nullptr && error.matches(*interruptType)) {
                const StopReason reason = guard.reason();
                if (reason == StopReason::cancelled) {
                    throw CookCancelled();
                }
                stoppedSignature_ = signature;
                stoppedMessage_ = reason == StopReason::timeLimit
                    ? "Python node stopped: it ran longer than " + seconds(timeLimitMilliseconds.load()) + " seconds"
                    : "Python node stopped";
                throw std::runtime_error(stoppedMessage_);
            }
            throw std::runtime_error(error.what());
        } catch (const py::cast_error&) {
            if (mine != nullptr && mine->isOpen()) {
                mine->freezeViews();
            }
            throw std::runtime_error("the python node's script must leave a jitnodes.MutableGeometry in `geo`");
        } catch (...) {
            if (mine != nullptr && mine->isOpen()) {
                mine->freezeViews();
            }
            throw;
        }
    }

private:
    // What decides whether a stopped node may run again: its code, parameters and the identity of its inputs.
    std::string runSignature(const std::vector<GeometryHandle>& inputs) const {
        std::string text = getString("code") + '\0' + std::to_string(timeLimitMilliseconds.load());
        for (const std::string& name : floatNames()) {
            text += '\0' + name + '=' + std::to_string(getFloat(name));
        }
        for (const std::string& name : stringNames()) {
            if (name != "code") {
                text += '\0' + name + '=' + getString(name);
            }
        }
        for (const GeometryHandle& input : inputs) {
            text += '\0' + std::to_string(reinterpret_cast<std::uintptr_t>(input.get()));
        }
        return text;
    }

    std::string stoppedSignature_;
    std::string stoppedMessage_;
};

void* leaveInterpreter() {
    if (Py_IsInitialized() && PyGILState_Check()) {
        return PyEval_SaveThread();
    }
    return nullptr;
}

void enterInterpreter(void* token) {
    if (token != nullptr) {
        PyEval_RestoreThread(static_cast<PyThreadState*>(token));
    }
}

}  // namespace

void setPythonNodeTimeLimit(std::chrono::milliseconds limit) {
    timeLimitMilliseconds = limit.count() > 0 ? limit.count() : 0;
}

std::chrono::milliseconds pythonNodeTimeLimit() {
    return std::chrono::milliseconds(timeLimitMilliseconds.load());
}

void setPythonNodeInterruptCheck(std::function<bool()> check) {
    std::lock_guard<std::mutex> lock(interruptMutex);
    interruptCheck = check ? std::make_shared<const std::function<bool()>>(std::move(check)) : nullptr;
}

void registerPythonNode(py::module_& module) {
    if (interruptType == nullptr) {
        PyObject* type = PyErr_NewException("jitnodes.NodeInterrupted", PyExc_BaseException, nullptr);
        interruptType = new py::object(py::reinterpret_steal<py::object>(type));
    }
    module.attr("NodeInterrupted") = *interruptType;
    module.def("set_python_node_time_limit", [](double seconds) {
        setPythonNodeTimeLimit(std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0)));
    }, py::arg("seconds"), "How long a python node may run before it is stopped; 0 for no limit (the default is 10 seconds).");
    module.def("python_node_time_limit", []() { return static_cast<double>(pythonNodeTimeLimit().count()) / 1000.0; });

    py::class_<MutableGeometry>(module, "MutableGeometry",
        "The geometry a python node edits: a copy of its first input. Valid for the cook only; arrays from point_attrib/attrib are writable "
        "views of its storage and become read-only when the cook ends.")
        .def("point_count", [](MutableGeometry& self) { return self.geometry().pointCount(); })
        .def("primitive_count", [](MutableGeometry& self) { return self.geometry().primitiveCount(); })
        .def("resize_points", [](MutableGeometry& self, size_t count) { self.resizePoints(count); }, py::arg("count"),
            "Changes the number of points (new points are zero). Refused while an array from point_attrib/attrib is alive.")
        .def("add_attrib", [](MutableGeometry& self, AttribClass cls, const std::string& name, AttribType type) {
                self.geometry().attrib(cls, name, type);
            }, py::arg("attrib_class"), py::arg("name"), py::arg("attrib_type") = AttribType::float32,
            "Creates a zero filled attribute when it is missing; raises when it exists with another type.")
        .def("has_attrib", [](MutableGeometry& self, AttribClass cls, const std::string& name) { return self.geometry().hasAttrib(cls, name); },
            py::arg("attrib_class"), py::arg("name"))
        .def("attrib_names", [](MutableGeometry& self, AttribClass cls) {
                std::vector<std::string> names;
                for (const auto& entry : self.geometry().attribs(cls)) {
                    names.push_back(entry.first);
                }
                return names;
            }, py::arg("attrib_class"))
        .def("attrib", [](MutableGeometry& self, AttribClass cls, const std::string& name) { return self.view(cls, name); },
            py::arg("attrib_class"), py::arg("name"), "A writable NumPy view of an attribute (float32, or int32 for int and string attributes).")
        .def("point_attrib", [](MutableGeometry& self, const std::string& name) { return self.view(AttribClass::point, name); }, py::arg("name"),
            "A writable NumPy view of a point attribute, such as 'x' or 'y'.")
        .def("prim_attrib", [](MutableGeometry& self, const std::string& name) { return self.view(AttribClass::primitive, name); }, py::arg("name"),
            "A writable NumPy view of a primitive attribute.")
        .def("add_primitive", [](MutableGeometry& self, uint32_t start, uint32_t count, bool closed, PrimKind kind, const std::string& key) {
                self.addPrimitive(start, count, closed, kind, key);
            }, py::arg("start"), py::arg("count"), py::arg("closed") = false, py::arg("kind") = PrimKind::polyline, py::arg("key") = std::string(),
            "Adds a primitive over `count` points from point `start` (a cubic one needs 3n+1 points, or 3n when closed). A non-empty key makes it part "
            "of the shape with that key; primitives of one shape must be added one after the other. Refused while an array from prim_attrib is alive.")
        .def("primitive", [](MutableGeometry& self, size_t index) {
                const std::vector<Primitive>& all = self.geometry().primitives();
                if (index >= all.size()) {
                    throw py::index_error("primitive " + std::to_string(index) + " does not exist");
                }
                const Primitive& p = all[index];
                return py::make_tuple(p.start, p.count, p.closed, p.kind);
            }, py::arg("index"), "(start, count, closed, kind) of a primitive.")
        .def("primitives", [](MutableGeometry& self) {
                py::list result;
                for (const Primitive& p : self.geometry().primitives()) {
                    result.append(py::make_tuple(p.start, p.count, p.closed, p.kind));
                }
                return result;
            }, "(start, count, closed, kind) of every primitive.")
        .def("shape_key", [](MutableGeometry& self, size_t index) { return self.geometry().shapeKey(index); }, py::arg("index"),
            "The key of the shape a primitive belongs to ('' for an anonymous one).")
        .def("set_shape_key", [](MutableGeometry& self, size_t index, const std::string& key) { self.geometry().setShapeKey(index, key); },
            py::arg("index"), py::arg("key"), "Gives a primitive a shape key; the shapes are checked when the cook ends.")
        .def("strings", [](MutableGeometry& self, AttribClass cls, const std::string& name) { return self.stringAttrib(cls, name).table().strings; },
            py::arg("attrib_class"), py::arg("name"), "The string table of a string attribute: element i of the ids is strings[ids[i]].")
        .def("string_values", [](MutableGeometry& self, AttribClass cls, const std::string& name) {
                AttribArray& attrib = self.stringAttrib(cls, name);
                std::vector<std::string> values;
                values.reserve(attrib.size());
                for (size_t i = 0; i < attrib.size(); ++i) {
                    values.push_back(attrib.stringAt(i));
                }
                return values;
            }, py::arg("attrib_class"), py::arg("name"), "The text of every element of a string attribute.")
        .def("set_string", [](MutableGeometry& self, AttribClass cls, const std::string& name, size_t index, const std::string& text) {
                self.setString(cls, name, index, text);
            }, py::arg("attrib_class"), py::arg("name"), py::arg("index"), py::arg("text"),
            "Sets one element of a string attribute (create it with add_attrib(cls, name, AttribType.string)).")
        .def("set_string_values", [](MutableGeometry& self, AttribClass cls, const std::string& name, const std::vector<std::string>& values) {
                self.setStringValues(cls, name, values);
            }, py::arg("attrib_class"), py::arg("name"), py::arg("values"), "Sets every element of a string attribute; one text per element.")
        .def("set_detail", [](MutableGeometry& self, const std::string& name, float value) { self.geometry().setDetail(name, value); },
            py::arg("name"), py::arg("value"))
        .def("detail", [](MutableGeometry& self, const std::string& name, float fallback) { return self.geometry().detail(name, fallback); },
            py::arg("name"), py::arg("fallback") = 0.0f)
        .def("details", [](MutableGeometry& self) { return self.geometry().details(); });

    module.def("allow_python_nodes", [](bool allowed) {
        if (allowed) {
            setPythonNodePolicy([](const Node&) { return true; });
        } else {
            setPythonNodePolicy(nullptr);
        }
    }, py::arg("allowed"),
    "Lets python nodes run (True) or stops them (False, the default). Code in a file is somebody else's code: allow it only for files you trust.");

    NodeRegistry::instance().registerType("python", [] { return std::make_unique<PythonNode>(); });
    Network::setCookThreadHooks({leaveInterpreter, enterInterpreter});
}

}  // namespace jitnodes::python
