import ast
import os
import unittest

import jitnodes as jn


def load_stub():
    path = os.path.join(os.path.dirname(jn.__file__), "jitnodes", "__init__.pyi")
    with open(path, encoding="utf-8") as stream:
        return ast.parse(stream.read())


def stub_classes():
    return {node.name: node for node in load_stub().body if isinstance(node, ast.ClassDef)}


def members(class_def):
    names = set()
    for item in class_def.body:
        if isinstance(item, ast.FunctionDef):
            names.add(item.name)
        elif isinstance(item, ast.AnnAssign):
            names.add(item.target.id)
    return names


def returns(class_def, name):
    for item in class_def.body:
        if isinstance(item, ast.FunctionDef) and item.name == name:
            return ast.unparse(item.returns)
    raise KeyError(name)


class HandleStubTest(unittest.TestCase):
    def setUp(self):
        self.net = jn.Network()
        self.line = self.net.add_node("line", "line")
        self.history = jn.History(self.net)
        self.geometry = self.net.cook(self.line)
        self.classes = stub_classes()

    def test_every_member_the_stub_declares_exists(self):
        instances = {"Network": self.net, "Node": self.line, "History": self.history, "Geometry": self.geometry}
        for class_name, instance in instances.items():
            for name in sorted(members(self.classes[class_name])):
                if name in ("__getattr__", "__setattr__"):
                    continue
                with self.subTest(member=f"{class_name}.{name}"):
                    self.assertTrue(hasattr(instance, name) or hasattr(type(instance), name))

    def test_the_stub_declares_the_handle_classes_and_values(self):
        self.assertTrue({"Network", "Node", "History", "Geometry", "StaleHandleError"} <= set(self.classes))
        self.assertIn("point_attrib", members(self.classes["Geometry"]))

    def test_node_results_are_declared_as_handles(self):
        network = self.classes["Network"]
        self.assertEqual(returns(network, "add_node"), "Node")
        self.assertEqual(returns(network, "node_ids"), "list[Node]")
        self.assertEqual(returns(network, "dependents_of"), "set[Node]")
        self.assertEqual(returns(network, "find_node"), "Node | None")
        self.assertIsInstance(self.net.add_node("null", "out"), jn.Node)
        self.assertTrue(all(isinstance(node, jn.Node) for node in self.net.node_ids()))
        self.assertIsInstance(self.net.dependents_of([self.line]), set)
        self.assertIsNone(self.net.find_node("missing"))

    def test_node_id_arguments_take_handles(self):
        network = self.classes["Network"]
        for item in network.body:
            if isinstance(item, ast.FunctionDef) and item.name == "connect":
                self.assertEqual(ast.unparse(item.args.args[1].annotation), "Node | int")
        out = self.net.add_node("null", "out")
        self.net.connect(self.line, out, 0)
        self.assertEqual(self.net.input_of(out, 0), self.line)

    def test_the_engine_id_method_is_replaced_by_the_property(self):
        self.assertIn("id", members(self.classes["Node"]))
        self.assertIsInstance(self.line.id, int)


if __name__ == "__main__":
    unittest.main()
