#include "pyHandles.h"

#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include "jitnodes/core/threadPool.h"
#include "jitnodes/geo/geometry.h"
#include "jitnodes/geo/raster.h"

namespace py = pybind11;

namespace jitnodes::python {

namespace {

using GeometryPtr = std::shared_ptr<Geometry>;

// NumPy is only needed when an array is asked for, so the module imports without it.
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

const AttribArray& requireAttrib(const Geometry& geometry, AttribClass cls, const std::string& name) {
    const AttribArray* attrib = geometry.findAttrib(cls, name);
    if (attrib == nullptr) {
        throw py::key_error("no " + std::string(cls == AttribClass::point ? "point" : "primitive") + " attribute '" + name + "'");
    }
    return *attrib;
}

// A view of the attribute's storage. The geometry is const while a script holds it (it is shared with the node cache), so the storage
// neither moves nor changes; the capsule keeps the geometry, and with it the storage, alive for as long as the array is.
py::object attribView(const GeometryPtr& geometry, AttribClass cls, const std::string& name) {
    const AttribArray& attrib = requireAttrib(*geometry, cls, name);
    py::module_ numpy = importNumpy();

    py::capsule owner(new std::shared_ptr<const Geometry>(geometry), [](void* p) {
        delete static_cast<std::shared_ptr<const Geometry>*>(p);
    });

    const bool isFloat = attrib.type() == AttribType::float32;
    const py::dtype dtype(isFloat ? "float32" : "int32");
    // Ints and string ids both live in an int32 array.
    const void* data = attrib.data();
    py::array array(dtype, {static_cast<py::ssize_t>(attrib.size())}, {static_cast<py::ssize_t>(sizeof(float))}, data, owner);
    array.attr("flags").attr("writeable") = false;
    return std::move(array);
}

}  // namespace

void registerGeometryArrays(py::module_& core) {
    py::class_<Geometry, GeometryPtr> cls = py::reinterpret_borrow<py::class_<Geometry, GeometryPtr>>(core.attr("Geometry"));

    cls.def("attrib", [](const GeometryPtr& self, AttribClass attribClass, const std::string& name) {
            return attribView(self, attribClass, name);
        }, py::arg("attrib_class"), py::arg("name"),
        "A read-only NumPy view of an attribute (float32 or int32; a string attribute gives its ids, see strings()).")
        .def("point_attrib", [](const GeometryPtr& self, const std::string& name) {
            return attribView(self, AttribClass::point, name);
        }, py::arg("name"), "Read-only NumPy view of a point attribute, such as 'x' or 'y'.")
        .def("prim_attrib", [](const GeometryPtr& self, const std::string& name) {
            return attribView(self, AttribClass::primitive, name);
        }, py::arg("name"), "Read-only NumPy view of a primitive attribute.")
        .def("attrib_names", [](const GeometryPtr& self, AttribClass attribClass) {
            std::vector<std::string> names;
            for (const auto& entry : self->attribs(attribClass)) {
                names.push_back(entry.first);
            }
            return names;
        }, py::arg("attrib_class"))
        .def("attrib_type", [](const GeometryPtr& self, AttribClass attribClass, const std::string& name) {
            return requireAttrib(*self, attribClass, name).type();
        }, py::arg("attrib_class"), py::arg("name"))
        .def("strings", [](const GeometryPtr& self, AttribClass attribClass, const std::string& name) {
            const AttribArray& attrib = requireAttrib(*self, attribClass, name);
            if (attrib.type() != AttribType::string) {
                throw py::type_error("'" + name + "' is not a string attribute");
            }
            return attrib.table().strings;
        }, py::arg("attrib_class"), py::arg("name"), "The string table of a string attribute: element i of the array is strings[ids[i]].")
        .def("string_values", [](const GeometryPtr& self, AttribClass attribClass, const std::string& name) {
            const AttribArray& attrib = requireAttrib(*self, attribClass, name);
            if (attrib.type() != AttribType::string) {
                throw py::type_error("'" + name + "' is not a string attribute");
            }
            std::vector<std::string> values;
            values.reserve(attrib.size());
            const int32_t* ids = attrib.ints().data();
            const std::vector<std::string>& table = attrib.table().strings;
            for (size_t i = 0; i < attrib.size(); ++i) {
                if (ids[i] < 0 || static_cast<size_t>(ids[i]) >= table.size()) {
                    throw std::runtime_error("string attribute id is outside its table");
                }
                values.push_back(table[static_cast<size_t>(ids[i])]);
            }
            return values;
        }, py::arg("attrib_class"), py::arg("name"), "The text of every element of a string attribute.")
        .def("render_png", [](const GeometryPtr& self, const std::string& path, int width, int height) {
            // Same as `jitnodesCli --png`: the settings come from a render node in the chain, width and height 0 take its size.
            const RenderSettings settings = renderSettingsFromDetails(*self, width, height);
            const size_t hardware = std::thread::hardware_concurrency();
            ThreadPool pool(hardware > 1 ? hardware - 1 : 0);
            py::gil_scoped_release release;
            writePng(path, renderSplats(*self, settings, &pool));
            return std::make_pair(settings.width, settings.height);
        }, py::arg("path"), py::arg("width") = 0, py::arg("height") = 0,
        "Renders the points that have a radius as soft sprites (settings from a render node) to a PNG; returns (width, height).");
}

}  // namespace jitnodes::python
