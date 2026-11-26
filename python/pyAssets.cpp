#include "pyAssets.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include <pybind11/functional.h>
#include <pybind11/stl.h>

#include "jitnodes/graph/assetDefinition.h"
#include "jitnodes/graph/assetLibrary.h"
#include "jitnodes/graph/serialize.h"
#include "pyGuard.h"
#include "pyHandles.h"

namespace py = pybind11;

namespace jitnodes::python {

namespace {

std::string validateScripts(const AssetDefinition& definition) {
    if (definition.scripts.empty() || !Py_IsInitialized()) {
        return {};
    }
    try {
        const std::vector<ScriptProblem> problems = checkAssetScripts(definition.scripts);
        if (problems.empty()) {
            return {};
        }
        const ScriptProblem& first = problems.front();
        return first.script + " " + std::to_string(first.line) + ":" + std::to_string(first.column) + ": " + first.message;
    } catch (const py::error_already_set& e) {
        return std::string("the script check failed: ") + e.what();
    }
}

// What a `with jitnodes.assets.restricted(node, event):` block holds; a no-op for events with no restriction.
class RestrictedBlock {
public:
    RestrictedBlock(const PyNode& node, std::string event) : node_(node), event_(std::move(event)) {}

    void enter() {
        node_.get();
        EventRestriction::Mode mode;
        if (event_ == "onFrameChanged") {
            mode = EventRestriction::Mode::instanceAndInputs;
        } else if (event_ == "onVersionUpdated") {
            mode = EventRestriction::Mode::onlyThisNode;
        } else {
            return;
        }
        restriction_ = std::make_unique<EventRestriction>(node_.network().get(), node_.id(), event_, mode);
    }

    void exit() { restriction_.reset(); }

private:
    PyNode node_;
    std::string event_;
    std::unique_ptr<EventRestriction> restriction_;
};

// A parameter as Python sees it: a value of its own, or one of a definition's, found again by serial number on every use because adding,
// removing or reordering moves the list.
class PyAssetParam {
public:
    PyAssetParam() = default;

    PyAssetParam(py::object definition, AssetParam& param) : definition_(std::move(definition)) {
        static std::atomic<std::uint64_t> lastSerial{0};
        if (param.serial == 0) {
            param.serial = ++lastSerial;
        }
        serial_ = param.serial;
    }

    AssetParam* find() const {
        if (!definition_) {
            return &own_;
        }
        for (AssetParam& p : definition_.cast<AssetDefinition&>().params) {
            if (p.serial == serial_) {
                return &p;
            }
        }
        return nullptr;
    }

    AssetParam& get() const {
        AssetParam* p = find();
        if (p == nullptr) {
            throw StaleHandleError("the parameter is no longer part of its asset definition");
        }
        return *p;
    }

private:
    py::object definition_;
    std::uint64_t serial_ = 0;
    mutable AssetParam own_;
};

py::list liveParams(py::object self) {
    AssetDefinition& definition = self.cast<AssetDefinition&>();
    py::list out;
    for (AssetParam& p : definition.params) {
        out.append(py::cast(PyAssetParam(self, p)));
    }
    return out;
}

template <typename T>
void bindField(py::class_<PyAssetParam>& cls, const char* name, T AssetParam::*member, const char* doc = "") {
    cls.def_property(name, [member](const PyAssetParam& p) { return p.get().*member; },
                     [member](const PyAssetParam& p, T value) { p.get().*member = std::move(value); }, doc);
}

void bindParam(py::module_& m) {
    py::class_<PyAssetParam> cls(m, "AssetParam", "One promoted parameter of an asset: its default and what an editor shows for it.");
    cls.def(py::init<>())
        .def("__repr__", [](const PyAssetParam& p) {
            const AssetParam* param = p.find();
            return param != nullptr ? "<AssetParam " + param->name + ">" : std::string("<AssetParam (removed)>");
        });
    bindField(cls, "name", &AssetParam::name);
    bindField(cls, "label", &AssetParam::label);
    bindField(cls, "default", &AssetParam::defaultValue);
    bindField(cls, "minimum", &AssetParam::minimum);
    bindField(cls, "maximum", &AssetParam::maximum);
    bindField(cls, "step", &AssetParam::step);
    bindField(cls, "group", &AssetParam::group);
    bindField(cls, "tooltip", &AssetParam::tooltip);
    bindField(cls, "ramp", &AssetParam::ramp, "True for a ramp parameter; default_ramp is then its default.");
    bindField(cls, "default_ramp", &AssetParam::defaultRamp);
}

void bindDefinition(py::module_& m) {
    py::class_<AssetDefinition>(m, "AssetDef",
        "An asset definition as a value: a copy that does not touch the library until apply() or save(). Parameters are live objects of this\n"
        "definition (d.params[0].label = 'x' edits it).")
        .def(py::init<>())
        .def_readwrite("name", &AssetDefinition::name, "The node type name.")
        .def_readwrite("label", &AssetDefinition::label)
        .def_readwrite("category", &AssetDefinition::category)
        .def_readwrite("description", &AssetDefinition::description)
        .def_readwrite("author", &AssetDefinition::author)
        .def_readwrite("icon", &AssetDefinition::icon)
        .def_readwrite("help", &AssetDefinition::help)
        .def_readwrite("version", &AssetDefinition::version, "The asset's own version; save() writes one more than the registered one.")
        .def_readwrite("inputs", &AssetDefinition::inputs)
        .def_readonly("source", &AssetDefinition::source, "The file it was read from or saved to; empty when it only exists in memory.")
        .def_readonly("graph_text", &AssetDefinition::graphText, "The contents, as the JSON of a graph.")
        .def_property("input_labels", [](const AssetDefinition& d) { return d.inputLabels; },
                      [](AssetDefinition& d, std::vector<std::string> labels) { d.inputLabels = std::move(labels); })
        .def_property_readonly("params", [](py::object self) { return liveParams(std::move(self)); }, "The promoted parameters, in order.")
        .def_property("renames", [](const AssetDefinition& d) { return d.renames; },
                      [](AssetDefinition& d, std::map<std::string, std::string> map) { d.renames = std::move(map); },
                      "Old parameter name -> new name. Assign a whole dict, or use set_rename.")
        .def_property("scripts", [](const AssetDefinition& d) { return d.scripts; },
                      [](AssetDefinition& d, std::map<std::string, std::string> map) { d.scripts = std::move(map); },
                      "Script name -> source. Assign a whole dict, or use set_script.")
        .def_property("files", [](const AssetDefinition& d) { return d.files; },
                      [](AssetDefinition& d, std::map<std::string, std::string> map) { d.files = std::move(map); },
                      "Extra files: name -> text.")
        .def("set_rename", [](AssetDefinition& d, const std::string& from, const std::string& to) { d.renames[from] = to; })
        .def("set_script", [](AssetDefinition& d, const std::string& name, const std::string& source) { d.scripts[name] = source; })
        .def("remove_script", [](AssetDefinition& d, const std::string& name) { return d.scripts.erase(name) != 0; })
        .def("set_file", [](AssetDefinition& d, const std::string& name, const std::string& text) { d.files[name] = text; })
        .def("remove_file", [](AssetDefinition& d, const std::string& name) { return d.files.erase(name) != 0; })
        .def("add_param", [](py::object self, const std::string& name, float defaultValue, const std::string& label, std::optional<float> minimum,
                             std::optional<float> maximum, float step, const std::string& group, const std::string& tooltip,
                             std::optional<std::string> ramp) {
                 AssetDefinition& d = self.cast<AssetDefinition&>();
                 AssetParam p;
                 p.ramp = ramp.has_value();
                 p.defaultRamp = ramp.value_or("");
                 p.name = name;
                 p.defaultValue = defaultValue;
                 p.label = label;
                 p.minimum = minimum;
                 p.maximum = maximum;
                 p.step = step;
                 p.group = group;
                 p.tooltip = tooltip;
                 d.params.push_back(std::move(p));
                 return PyAssetParam(self, d.params.back());
             },
             py::arg("name"), py::arg("default") = 0.0f, py::arg("label") = "", py::arg("minimum") = py::none(), py::arg("maximum") = py::none(),
             py::arg("step") = 0.0f, py::arg("group") = "", py::arg("tooltip") = "", py::arg("ramp") = py::none(),
             "Adds a promoted parameter and returns it. With ramp (a ramp's text) it is a ramp parameter.")
        .def("param", [](py::object self, const std::string& name) -> py::object {
            AssetDefinition& d = self.cast<AssetDefinition&>();
            for (AssetParam& p : d.params) {
                if (p.name == name) {
                    return py::cast(PyAssetParam(self, p));
                }
            }
            return py::none();
        }, py::arg("name"), "The parameter with this name, or None.")
        .def("remove_param", [](AssetDefinition& d, const std::string& name) {
            const auto it = std::find_if(d.params.begin(), d.params.end(), [&](const AssetParam& p) { return p.name == name; });
            if (it == d.params.end()) {
                return false;
            }
            d.params.erase(it);
            return true;
        }, py::arg("name"))
        .def("rename_param", [](AssetDefinition& d, const std::string& from, const std::string& to, bool record) {
            return d.renameParam(from, to, record);
        }, py::arg("old"), py::arg("new"), py::arg("record") = true,
           "Renames a parameter and rewrites parent.<old> in the contents; returns how many expressions changed. With record (the default) the old "
           "name goes into `renames`, so instances of older versions keep their values; pass False for a parameter no registered version has had.")
        .def("validate", [](const AssetDefinition& d) { return d.validate(); },
             "An empty string when the definition can be registered, else the first problem (including a script that breaks the rules).")
        .def("to_text", &AssetDefinition::toText, "The definition file's text.")
        .def_static("from_text", &AssetDefinition::fromText, py::arg("text"))
        .def("apply", [](const AssetDefinition& d) {
            EventRestriction::checkLibrary("apply");
            AssetLibrary::instance().apply(d);
        }, "Registers it in memory (instances then count as outdated). Raises when validate() reports a problem.")
        .def("save", [](AssetDefinition& d, std::optional<std::string> path) {
            EventRestriction::checkLibrary("save");
            if (!path && d.source.empty()) {
                throw py::value_error("this definition has no file yet: give a path");
            }
            AssetLibrary::instance().save(d, path ? *path : d.source);
        }, py::arg("path") = py::none(), "Writes the file (one version up) and registers it.")
        .def("__repr__", [](const AssetDefinition& d) { return "<AssetDef " + d.name + " v" + std::to_string(d.version) + ">"; });
}

void bindLibrary(py::module_& m) {
    py::class_<AssetEntry>(m, "AssetInfo", "What the library knows about one asset.")
        .def_readonly("name", &AssetEntry::name)
        .def_readonly("label", &AssetEntry::label)
        .def_readonly("category", &AssetEntry::category)
        .def_readonly("version", &AssetEntry::version)
        .def_readonly("inputs", &AssetEntry::inputs)
        .def_readonly("params", &AssetEntry::params)
        .def_readonly("source", &AssetEntry::source)
        .def_readonly("embedded", &AssetEntry::embedded)
        .def_readonly("instances", &AssetEntry::instances)
        .def_readonly("outdated", &AssetEntry::outdated)
        .def("__repr__", [](const AssetEntry& e) { return "<AssetInfo " + e.name + " v" + std::to_string(e.version) + ">"; });

    m.def("list", [](const std::optional<PyNetwork>& network) {
        return AssetLibrary::instance().list(network ? &network->ref().get() : nullptr);
    }, py::arg("network") = py::none(), "The registered assets, by name; with a network, how many instances of each it holds.");
    m.def("has", [](const std::string& name) { return AssetLibrary::instance().has(name); }, py::arg("name"));
    m.def("get", [](const std::string& name) { return AssetLibrary::instance().definition(name); }, py::arg("name"),
          "A copy of the registered definition, to edit and then apply() or save().");
    m.def("install", [](const std::string& path) {
        EventRestriction::checkLibrary("install");
        AssetLibrary::instance().installFile(path);
    }, py::arg("path"), "Registers the definition in a .asset.json file.");
    m.def("install_directory", [](const std::string& directory) {
        EventRestriction::checkLibrary("install_directory");
        size_t installed = 0;
        const std::string error = AssetLibrary::instance().installDirectory(directory, &installed);
        return py::make_tuple(installed, error);
    }, py::arg("directory"), "Registers every *.asset.json file of a folder; returns (how many, the first error or '').");
    m.def("reload", [](const std::string& name) {
        EventRestriction::checkLibrary("reload");
        AssetLibrary::instance().reload(name);
    }, py::arg("name"), "Reads the asset's file again.");
    m.def("uninstall", [](const std::string& name, const std::optional<PyNetwork>& network) {
        EventRestriction::checkLibrary("uninstall");
        AssetLibrary::instance().uninstall(name, network ? &network->ref().get() : nullptr);
    }, py::arg("name"), py::arg("network") = py::none(), "Removes the asset from the library (the file stays). With a network, refuses while it has instances.");
    m.def("update_instances", [](const PyNetwork& network, const std::optional<std::string>& name) {
        EventRestriction::checkLibrary("update_instances");
        return AssetLibrary::instance().updateInstances(network.ref().get(), name ? *name : std::string());
    }, py::arg("network"), py::arg("name") = py::none(),
          "Rebuilds the instances built from an older version of their asset (all assets, or one); returns how many. Parameters keep their values.");
    m.def("create", [](const PyNode& node, const std::optional<std::string>& name, const std::optional<std::string>& path) {
        node.get();
        AssetDefinition d = AssetDefinition::fromNode(node.network().get(), node.id(), name ? *name : node.get().name(), 1);
        if (path) {
            EventRestriction::checkLibrary("create");
            AssetLibrary::instance().save(d, *path);
        }
        return d;
    }, py::arg("node"), py::arg("name") = py::none(), py::arg("path") = py::none(),
          "A definition from a subnet. With a path it is also saved and registered; the subnet itself is not replaced.");
    m.def("unlock", [](const PyNode& node) {
        EventRestriction::checkNode(node.network(), node.id(), "unlock");
        node.get();
        unlockAssetInstance(node.network().get(), node.id());
        return py::cast(PyNode(node.network(), node.id()));
    }, py::arg("node"), "Turns an asset instance back into an ordinary subnet with a copy of the contents; returns the node.");
    m.def("rename_parameter", [](const PyNode& node, const std::string& from, const std::string& to) {
        EventRestriction::checkNode(node.network(), node.id(), "rename_parameter");
        node.get();
        return renameSpareParameter(node.network().get(), node.id(), from, to);
    }, py::arg("node"), py::arg("old"), py::arg("new"),
          "Renames a promoted parameter of a subnet and rewrites parent.<old> in the expressions inside it; returns how many were rewritten.");
    m.def("check_scripts", [](const std::map<std::string, std::string>& scripts) {
        py::list out;
        for (const ScriptProblem& p : checkAssetScripts(scripts)) {
            out.append(py::make_tuple(p.script, p.line, p.column, p.message));
        }
        return out;
    }, py::arg("scripts"),
          "The problems in a dict of asset scripts (name -> source) as (script, line, column, message); empty when the rules are kept.");

    py::class_<RestrictedBlock>(m, "_Restricted")
        .def("__enter__", [](py::object self) {
            self.cast<RestrictedBlock&>().enter();
            return self;
        })
        .def("__exit__", [](RestrictedBlock& self, const py::object&, const py::object&, const py::object&) { self.exit(); });
    m.def("restricted", [](const PyNode& node, const std::string& event) { return RestrictedBlock(node, event); },
          py::arg("node"), py::arg("event"),
          "For `with`: while it runs, the calls the event may not make raise PermissionError (onFrameChanged: change the node, what is inside it "
          "or what it reads from; onVersionUpdated: change anything). Other events: no restriction. An application wraps each event it runs in this.");
}

}  // namespace

void registerAssets(py::module_& module) {
    py::module_ m = module.def_submodule("assets", "Asset definitions, the asset library and the rules for asset scripts.");
    bindParam(m);
    bindDefinition(m);
    bindLibrary(m);
    AssetDefinition::setScriptValidator(validateScripts);
}

}  // namespace jitnodes::python
