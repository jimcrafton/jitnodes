#include "pyAssets.h"

#include <pybind11/eval.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace jitnodes::python {

namespace {

// The analysis is Python's own `ast`, run by the interpreter the scripts will run in, so the check and the run agree on what parses.
const char* checkerSource = R"PY(
import ast

_PREFIXES = ("set_", "add_", "remove_", "clear_", "rename")
_NAMES = frozenset(("connect", "disconnect", "undo", "redo", "clear", "freeze_node", "touch"))
_DYNAMIC = frozenset(("exec", "eval", "compile", "__import__"))
_ATTRIBUTE_BUILTINS = frozenset(("getattr", "setattr", "delattr"))
_DOCUMENT = frozenset(("doc", "network", "selection", "curvy"))


def _mutating(name):
    return name.startswith(_PREFIXES) or name in _NAMES


class _Walker(ast.NodeVisitor):
    def __init__(self, seeds, keys, helpers, report, document):
        self.tainted = set(seeds)
        self.keys = keys
        self.helpers = helpers
        self.report = report
        self.document = document

    def _is_tainted(self, expr):
        for sub in ast.walk(expr):
            if isinstance(sub, ast.Name) and sub.id in self.tainted:
                return True
            if (isinstance(sub, ast.Subscript) and isinstance(sub.value, ast.Name) and sub.value.id == "kwargs"
                    and isinstance(sub.slice, ast.Constant) and sub.slice.value in self.keys):
                return True
            if (isinstance(sub, ast.Call) and isinstance(sub.func, ast.Attribute) and sub.func.attr == "get"
                    and isinstance(sub.func.value, ast.Name) and sub.func.value.id == "kwargs"
                    and sub.args and isinstance(sub.args[0], ast.Constant) and sub.args[0].value in self.keys):
                return True
        return False

    def _taint(self, target):
        if isinstance(target, ast.Name):
            self.tainted.add(target.id)
        elif isinstance(target, (ast.Tuple, ast.List)):
            for item in target.elts:
                self._taint(item)
        elif isinstance(target, ast.Starred):
            self._taint(target.value)

    def _store(self, target):
        if isinstance(target, (ast.Attribute, ast.Subscript)):
            if self._is_tainted(target.value):
                self.report(target, "assigns to it")
        elif isinstance(target, (ast.Tuple, ast.List)):
            for item in target.elts:
                self._store(item)

    def visit_Assign(self, node):
        self.visit(node.value)
        tainted = self._is_tainted(node.value)
        for target in node.targets:
            self._store(target)
            if tainted:
                self._taint(target)

    def visit_AnnAssign(self, node):
        if node.value is not None:
            self.visit(node.value)
            self._store(node.target)
            if self._is_tainted(node.value):
                self._taint(node.target)

    def visit_AugAssign(self, node):
        self.visit(node.value)
        self._store(node.target)

    def visit_NamedExpr(self, node):
        self.visit(node.value)
        if self._is_tainted(node.value):
            self._taint(node.target)

    def visit_Delete(self, node):
        for target in node.targets:
            self._store(target)

    def visit_For(self, node):
        self.visit(node.iter)
        if self._is_tainted(node.iter):
            self._taint(node.target)
        for stmt in node.body + node.orelse:
            self.visit(stmt)

    def visit_With(self, node):
        for item in node.items:
            self.visit(item.context_expr)
            if item.optional_vars is not None and self._is_tainted(item.context_expr):
                self._taint(item.optional_vars)
        for stmt in node.body:
            self.visit(stmt)

    def _comprehension(self, node, elements):
        for gen in node.generators:
            self.visit(gen.iter)
            if self._is_tainted(gen.iter):
                self._taint(gen.target)
            for cond in gen.ifs:
                self.visit(cond)
        for element in elements:
            self.visit(element)

    def visit_ListComp(self, node):
        self._comprehension(node, [node.elt])

    def visit_SetComp(self, node):
        self._comprehension(node, [node.elt])

    def visit_GeneratorExp(self, node):
        self._comprehension(node, [node.elt])

    def visit_DictComp(self, node):
        self._comprehension(node, [node.key, node.value])

    def visit_BinOp(self, node):
        if isinstance(node.op, ast.RShift) and self._is_tainted(node.right):
            self.report(node, "connects it")
        self.generic_visit(node)

    def visit_Name(self, node):
        if isinstance(node.ctx, ast.Load) and node.id in self.document:
            self.report(node, "uses " + node.id)

    def visit_Import(self, node):
        for alias in node.names:
            if alias.name.split(".")[0] in self.document:
                self.report(node, "imports " + alias.name)

    def visit_ImportFrom(self, node):
        if (node.module or "").split(".")[0] in self.document:
            self.report(node, "imports " + node.module)

    def visit_Call(self, node):
        func = node.func
        if isinstance(func, ast.Attribute):
            if self._is_tainted(func.value):
                if _mutating(func.attr):
                    self.report(node, "calls " + func.attr)
            elif isinstance(func.value, ast.Name) and func.value.id == "module" and func.attr in self.helpers:
                self.report(node, "calls module." + func.attr + ", which edits")
        elif isinstance(func, ast.Name):
            if func.id in _DYNAMIC:
                self.report(node, func.id + "() could hide an edit")
            elif func.id in _ATTRIBUTE_BUILTINS:
                self._attribute_builtin(node, func.id)
            elif func.id in self.helpers:
                self.report(node, "calls " + func.id + ", which edits")
        self.generic_visit(node)

    def _attribute_builtin(self, node, name):
        args = node.args
        named = args[1] if len(args) > 1 else None
        if named is None or not (isinstance(named, ast.Constant) and isinstance(named.value, str)):
            self.report(node, name + "() with a computed name could hide an edit")
        elif args and self._is_tainted(args[0]) and (name != "getattr" or _mutating(named.value)):
            self.report(node, name + "(" + repr(named.value) + ")")


def _editing_helpers(tree):
    functions = {n.name: n for n in tree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))}
    editing = set()
    changed = True
    while changed:
        changed = False
        for name, function in functions.items():
            if name in editing:
                continue
            arguments = function.args
            parameters = {a.arg for a in arguments.posonlyargs + arguments.args + arguments.kwonlyargs}
            hits = []
            walker = _Walker(parameters | {"node", "network", "doc", "kwargs"}, ("node",), editing, lambda n, m: hits.append(m), frozenset())
            for statement in function.body:
                walker.visit(statement)
            if hits:
                editing.add(name)
                changed = True
    return editing


# event: (names in kwargs the event may not change, the document names it may not use, what to say)
_RULES = {
    "onFrameChanged": (("node",), frozenset(), "cannot change this asset"),
    "onVersionUpdated": (("old",), _DOCUMENT, "may only read old and change fresh"),
}


def check(scripts):
    problems = []
    trees = {}
    for event, source in scripts.items():
        try:
            trees[event] = ast.parse(source, filename=event)
        except SyntaxError as error:
            problems.append((event, error.lineno or 1, error.offset or 1, error.msg))
    helpers = _editing_helpers(trees["module"]) if "module" in trees else set()
    for event, (keys, document, saying) in _RULES.items():
        tree = trees.get(event)
        if tree is None:
            continue

        def report(node, detail, event=event, saying=saying):
            problems.append((event, getattr(node, "lineno", 1), getattr(node, "col_offset", 0) + 1, saying + " (" + detail + ")"))

        walker = _Walker(keys, keys, helpers, report, document)
        for statement in tree.body:
            walker.visit(statement)
    for event, tree in trees.items():
        if event in _RULES or event == "module":
            continue

        def report(node, detail, event=event):
            problems.append((event, getattr(node, "lineno", 1), getattr(node, "col_offset", 0) + 1, "cannot do this (" + detail + ")"))

        walker = _Walker((), (), set(), report, frozenset())
        for statement in tree.body:
            walker.visit(statement)
    problems.sort(key=lambda p: (p[0], p[1], p[2]))
    return problems
)PY";

py::object& checkerModule() {
    static py::object* module = new py::object();   // never freed: it must not be released after the interpreter is finalized
    if (!*module) {
        py::object created = py::module_::import("types").attr("ModuleType")("jitnodes_assetcheck");
        py::exec(checkerSource, created.attr("__dict__"));
        *module = created;
    }
    return *module;
}

}  // namespace

std::vector<ScriptProblem> checkAssetScripts(const std::map<std::string, std::string>& scripts) {
    py::gil_scoped_acquire gil;
    std::vector<ScriptProblem> problems;
    const py::list found = checkerModule().attr("check")(scripts);
    for (const py::handle item : found) {
        const py::tuple row = py::reinterpret_borrow<py::tuple>(item);
        problems.push_back({row[0].cast<std::string>(), row[1].cast<int>(), row[2].cast<int>(), row[3].cast<std::string>()});
    }
    return problems;
}

}  // namespace jitnodes::python
