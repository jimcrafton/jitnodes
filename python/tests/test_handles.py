import gc
import threading
import unittest

import jitnodes as jn


def line_graph(threads=1):
    net = jn.Network(threads)
    line = net.add_node("line", "line")
    wrangle = net.add_node("wrangle", "wrangle")
    out = net.add_node("null", "out")
    line.count = 8
    line.length = 7
    wrangle.code = "x = x * 2;"
    line >> wrangle >> out
    net.set_display_node(out)
    return net, line, wrangle, out


class NetworkTest(unittest.TestCase):
    def test_add_node_returns_a_node_handle(self):
        net = jn.Network()
        node = net.add_node("line", "first")
        self.assertIsInstance(node, jn.Node)
        self.assertEqual(node.name(), "first")
        self.assertEqual(node.type_name(), "line")
        self.assertGreater(node.id, 0)
        self.assertEqual(len(net), 1)

    def test_nodes_are_found_by_name_and_id(self):
        net, line, wrangle, out = line_graph()
        self.assertEqual(net["wrangle"], wrangle)
        self.assertEqual(net[line.id], line)
        self.assertEqual(net.node("out"), out)
        self.assertEqual(net.find_node("wrangle"), wrangle)
        self.assertIsNone(net.find_node("missing"))
        self.assertIn("line", net)
        self.assertIn(line, net)
        self.assertNotIn("missing", net)
        self.assertEqual([n.name() for n in net], ["line", "wrangle", "out"])
        with self.assertRaises(KeyError):
            net["missing"]

    def test_handles_compare_and_hash_by_node(self):
        net, line, _wrangle, _out = line_graph()
        self.assertEqual(net["line"], line)
        self.assertEqual(hash(net["line"]), hash(line))
        self.assertEqual(len({net["line"], line}), 1)
        self.assertNotEqual(net["line"], net["out"])

    def test_engine_methods_take_and_return_handles(self):
        net, line, wrangle, out = line_graph()
        self.assertEqual(net.input_of(wrangle, 0), line)
        self.assertEqual(net.display_node(), out)
        self.assertEqual(net.execution_order(out), [line, wrangle, out])
        self.assertEqual(net.consumers_of(line), [wrangle])
        self.assertEqual(net.dependents_of([line]), {line, wrangle, out})
        self.assertEqual(net.node_ids(), [line, wrangle, out])
        net.disconnect(out, 0)
        self.assertIsNone(net.input_of(out, 0))

    def test_cooking_gives_geometry(self):
        net, _line, wrangle, out = line_graph()
        geometry = net.cook(out)
        self.assertEqual(geometry.point_count(), 8)
        self.assertEqual(net.cook_display().point_count(), 8)
        wrangle.code = "x = x * 2; y = y + 1;"
        self.assertEqual(net.cook(wrangle).point_count(), 8)

    def test_connecting_across_networks_is_refused(self):
        _net, line, _wrangle, _out = line_graph()
        other = jn.Network().add_node("null")
        with self.assertRaises(ValueError):
            line >> other

    def test_engine_errors_are_runtime_errors(self):
        net, line, _wrangle, out = line_graph()
        with self.assertRaises(RuntimeError):
            out >> line

    def test_an_unknown_member_is_an_attribute_error(self):
        net = jn.Network()
        with self.assertRaises(AttributeError):
            net.no_such_method
        self.assertFalse(hasattr(net, "no_such_method"))


class NodeTest(unittest.TestCase):
    def test_parameters_read_and_write_like_attributes_and_items(self):
        net = jn.Network()
        line = net.add_node("line")
        line.count = 5
        self.assertEqual(line.count, 5.0)
        line["length"] = 3.5
        self.assertEqual(line["length"], 3.5)
        self.assertIn("count", line)
        self.assertNotIn("nonsense", line)
        self.assertEqual(net[line.name()].count, 5.0)

    def test_string_parameters(self):
        net = jn.Network()
        wrangle = net.add_node("wrangle")
        wrangle.code = "x = x + 1;"
        self.assertEqual(wrangle.code, "x = x + 1;")
        self.assertEqual(wrangle["code"], "x = x + 1;")

    def test_an_unknown_parameter_is_an_error_not_a_new_attribute(self):
        net = jn.Network()
        line = net.add_node("line")
        with self.assertRaises(AttributeError):
            line.cuont = 5
        with self.assertRaises(AttributeError):
            line.cuont
        with self.assertRaises(KeyError):
            line["cuont"]

    def test_engine_methods_are_forwarded(self):
        net = jn.Network()
        line = net.add_node("line")
        line.set_float("count", 6.0)
        self.assertEqual(line.get_float("count"), 6.0)
        self.assertEqual(line.input_count(), 0)
        line.add_tag("layer")
        self.assertTrue(line.has_tag("layer"))
        self.assertEqual(line.tags(), ["layer"])

    def test_the_wire_operator_returns_the_target_so_it_chains(self):
        net = jn.Network()
        a, b, c = net.add_node("line"), net.add_node("wrangle"), net.add_node("null")
        self.assertEqual(a >> b, b)
        b.connect(c)
        self.assertEqual(net.input_of(c, 0), b)
        self.assertEqual(net.input_of(b, 0), a)
        c.disconnect()
        self.assertIsNone(net.input_of(c, 0))

    def test_a_node_knows_its_network(self):
        net = jn.Network()
        node = net.add_node("null")
        self.assertTrue(node.network.valid)
        self.assertEqual(node.network.node(node.name()), node)

    def test_repr(self):
        net = jn.Network()
        node = net.add_node("line", "first")
        self.assertIn("'first'", repr(node))
        self.assertIn("type=line", repr(node))


class StaleHandleTest(unittest.TestCase):
    def test_a_removed_node_raises_and_reports_invalid(self):
        net = jn.Network()
        node = net.add_node("line", "gone")
        self.assertTrue(node.valid)
        net.remove_node(node)
        self.assertFalse(node.valid)
        self.assertIn("(gone)", repr(node))
        with self.assertRaises(jn.StaleHandleError):
            node.count
        with self.assertRaises(jn.StaleHandleError):
            node.count = 1
        with self.assertRaises(jn.StaleHandleError):
            node.name()
        with self.assertRaises(jn.StaleHandleError):
            net.cook(node)

    def test_stale_handle_error_is_a_reference_error(self):
        self.assertTrue(issubclass(jn.StaleHandleError, ReferenceError))

    def test_a_bound_method_kept_in_a_variable_is_checked_when_called(self):
        net = jn.Network()
        node = net.add_node("line")
        set_float = node.set_float
        set_float("count", 3.0)
        net.remove_node(node)
        with self.assertRaises(jn.StaleHandleError):
            set_float("count", 4.0)

    def test_clear_makes_old_handles_stale_even_when_the_id_is_taken_again(self):
        net = jn.Network()
        old = net.add_node("line", "old")
        net.clear()
        new = net.add_node("line", "new")
        self.assertEqual(old.id, new.id)
        self.assertFalse(old.valid)
        self.assertTrue(new.valid)
        with self.assertRaises(jn.StaleHandleError):
            old.count = 5
        with self.assertRaises(jn.StaleHandleError):
            old.name()
        self.assertEqual(new.name(), "new")
        self.assertNotEqual(old, new)
        self.assertEqual(len({old, new}), 2)
        self.assertNotIn(old, net)
        self.assertIn(new, net)
        with self.assertRaises(jn.StaleHandleError):
            net.cook(old)

    def test_a_node_made_after_a_clear_is_equal_to_itself_found_again(self):
        net = jn.Network()
        net.clear()
        node = net.add_node("line", "kept")
        self.assertEqual(net["kept"], node)

    def test_undo_removes_a_node_made_after_the_commit(self):
        net = jn.Network()
        history = jn.History(net)
        node = net.add_node("line", "temp")
        self.assertTrue(history.commit("add"))
        self.assertTrue(history.undo())
        self.assertFalse(node.valid)
        with self.assertRaises(jn.StaleHandleError):
            node.count
        self.assertTrue(history.redo())
        self.assertEqual(net.find_node("temp").type_name(), "line")

    def test_a_handle_follows_its_id_when_undo_restores_the_node(self):
        net = jn.Network()
        node = net.add_node("line")
        node.count = 4
        history = jn.History(net)
        node.count = 9
        history.commit("edit")
        history.undo()
        self.assertEqual(node.count, 4.0)


class HistoryTest(unittest.TestCase):
    def test_commit_undo_redo(self):
        net = jn.Network()
        line = net.add_node("line")
        line.count = 8
        history = jn.History(net)
        line.count = 4
        self.assertTrue(history.commit("shorter"))
        self.assertTrue(history.can_undo())
        self.assertEqual(history.undo_label(), "shorter")
        self.assertTrue(history.undo())
        self.assertEqual(line.count, 8.0)
        self.assertTrue(history.can_redo())
        self.assertTrue(history.redo())
        self.assertEqual(line.count, 4.0)

    def test_unknown_member(self):
        history = jn.History(jn.Network())
        with self.assertRaises(AttributeError):
            history.no_such_method


class ValueTypesTest(unittest.TestCase):
    def test_enums_and_keyframes_are_plain_values(self):
        self.assertNotEqual(jn.AttribClass.point, jn.AttribClass.primitive)
        key = jn.Keyframe()
        key.time = 1.5
        key.value = 2.0
        key.interp = jn.KeyInterp.smooth
        self.assertEqual((key.time, key.value, key.interp), (1.5, 2.0, jn.KeyInterp.smooth))

    def test_keyframes_drive_a_parameter(self):
        net = jn.Network()
        line = net.add_node("line")
        first, last = jn.Keyframe(), jn.Keyframe()
        last.time, last.value = 1.0, 10.0
        line.set_keyframes("count", [first, last])
        self.assertTrue(line.is_animated("count"))


class OwnershipTest(unittest.TestCase):
    def test_a_node_keeps_the_network_a_script_made_alive(self):
        node = jn.Network().add_node("line", "kept")
        gc.collect()
        self.assertTrue(node.valid)
        self.assertTrue(node.network.valid)
        node.count = 3
        self.assertEqual(node.count, 3.0)
        self.assertEqual(node.network["kept"], node)

    def test_a_bound_method_keeps_the_network_alive(self):
        add_node = jn.Network().add_node
        gc.collect()
        self.assertEqual(add_node("line", "made").name(), "made")

    def test_a_history_keeps_the_network_alive(self):
        history = jn.History(jn.Network())
        gc.collect()
        self.assertFalse(history.can_undo())

    def test_geometry_outlives_its_network_and_handles(self):
        net, _line, _wrangle, out = line_graph()
        geometry = net.cook(out)
        del net, out, _line, _wrangle
        gc.collect()
        self.assertEqual(geometry.point_count(), 8)
        self.assertGreater(geometry.primitive_count() + geometry.point_count(), 0)

    def test_a_script_cannot_change_a_cooked_geometry_the_cache_still_holds(self):
        net, _line, _wrangle, out = line_graph()
        geometry = net.cook(out)
        for mutator in ("resize_points", "append", "set_detail", "set_shape_key", "add_vector_attrib"):
            self.assertFalse(hasattr(geometry, mutator), mutator)
        self.assertTrue(hasattr(geometry, "point_count"))
        self.assertEqual(net.cook(out).point_count(), 8)

    def test_handles_to_two_networks_do_not_mix(self):
        a, b = jn.Network(), jn.Network()
        node_a, node_b = a.add_node("line", "same"), b.add_node("line", "same")
        self.assertEqual(node_a.id, node_b.id)
        self.assertNotEqual(node_a, node_b)
        self.assertIn(node_a, a)
        self.assertNotIn(node_a, b)
        with self.assertRaises(ValueError):
            a.connect(node_a, node_b)


class CookingTest(unittest.TestCase):
    def test_progress_callbacks_run_while_cook_has_released_the_gil(self):
        net, _line, _wrangle, out = line_graph(threads=4)
        calls = []
        net.set_progress_callback(lambda progress: calls.append(threading.get_ident()))
        result = []
        worker = threading.Thread(target=lambda: result.append(net.cook(out).point_count()))
        worker.start()
        worker.join(timeout=30)
        self.assertFalse(worker.is_alive(), "cook did not finish: the callback is waiting for the GIL")
        self.assertEqual(result, [8])
        self.assertGreater(len(calls), 0)

    def test_another_thread_runs_while_a_cook_is_in_progress(self):
        net, _line, wrangle, out = line_graph()
        wrangle.code = "x = x * 2;"
        started = threading.Event()
        net.set_progress_callback(lambda progress: started.set())
        worker = threading.Thread(target=lambda: net.cook(out))
        worker.start()
        self.assertTrue(started.wait(timeout=30))
        worker.join(timeout=30)
        self.assertFalse(worker.is_alive())


class AttribArrayTest(unittest.TestCase):
    def setUp(self):
        self.np = __import__("numpy")

    def test_point_attrib_is_a_read_only_float32_view(self):
        net, _line, _wrangle, out = line_graph()
        geometry = net.cook(out)
        x = geometry.point_attrib("x")
        self.assertEqual(x.dtype, self.np.float32)
        self.assertEqual(x.shape, (8,))
        self.assertFalse(x.flags.writeable)
        self.assertEqual(list(x), [2.0 * i for i in range(8)])
        with self.assertRaises(ValueError):
            x[0] = 1.0

    def test_the_view_shares_the_geometry_storage(self):
        net, _line, _wrangle, out = line_graph()
        geometry = net.cook(out)
        first = geometry.point_attrib("x")
        second = geometry.attrib(jn.AttribClass.point, "x")
        self.assertEqual(first.ctypes.data, second.ctypes.data)

    def test_the_array_outlives_the_geometry_and_the_network(self):
        net, _line, _wrangle, out = line_graph()
        x = net.cook(out).point_attrib("x")
        del net, _line, _wrangle, out
        gc.collect()
        self.assertEqual(float(x.sum()), 2.0 * sum(range(8)))

    def attrib_graph(self, **settings):
        net = jn.Network()
        line = net.add_node("line", "line")
        line.count = 5
        create = net.add_node("attribCreate", "create")
        for name, value in settings.items():
            setattr(create, name, value)
        line >> create
        return net, create

    def test_primitive_attributes_by_type(self):
        _net, create = self.attrib_graph(name="weight", **{"class": "primitive"}, type="float", value=2.5)
        geometry = _net.cook(create)
        weight = geometry.prim_attrib("weight")
        self.assertEqual(weight.dtype, self.np.float32)
        self.assertEqual(weight.shape, (geometry.primitive_count(),))
        self.assertTrue(self.np.all(weight == 2.5))
        self.assertFalse(weight.flags.writeable)

        _net, create = self.attrib_graph(name="id", **{"class": "primitive"}, type="int", value=7)
        ids = _net.cook(create).prim_attrib("id")
        self.assertEqual(ids.dtype, self.np.int32)
        self.assertTrue(self.np.all(ids == 7))

    def test_int_point_attribute(self):
        net, create = self.attrib_graph(name="tag", type="int", value=3)
        tag = net.cook(create).point_attrib("tag")
        self.assertEqual(tag.dtype, self.np.int32)
        self.assertEqual(tag.shape, (5,))
        self.assertTrue(self.np.all(tag == 3))

    def test_string_attributes_give_ids_and_a_table(self):
        net, create = self.attrib_graph(name="label", type="string", text="hello")
        geometry = net.cook(create)
        ids = geometry.point_attrib("label")
        self.assertEqual(ids.dtype, self.np.int32)
        table = geometry.strings(jn.AttribClass.point, "label")
        self.assertEqual(table[0], "")
        self.assertEqual({table[i] for i in ids}, {"hello"})
        self.assertEqual(geometry.string_values(jn.AttribClass.point, "label"), ["hello"] * 5)
        self.assertEqual(geometry.attrib_type(jn.AttribClass.point, "label"), jn.AttribType.string)

    def test_string_attribute_with_a_different_text_per_element(self):
        net = jn.Network()
        line = net.add_node("line", "line")
        line.count = 4
        label = net.add_node("attribString", "label")
        label.name = "label"
        label.text = "pt$P"
        line >> label
        geometry = net.cook(label)
        values = geometry.string_values(jn.AttribClass.point, "label")
        self.assertEqual(len(values), 4)
        self.assertEqual(len(set(values)), 4)
        ids = geometry.point_attrib("label")
        table = geometry.strings(jn.AttribClass.point, "label")
        self.assertEqual([table[i] for i in ids], values)

    def test_detail_values_come_from_the_geometry(self):
        net = jn.Network()
        line = net.add_node("line", "line")
        render = net.add_node("render", "render")
        line >> render
        details = net.cook(render).details()
        self.assertEqual(details["render.enabled"], 1.0)

    def test_render_png_writes_an_image_of_the_splats(self):
        import os
        import tempfile
        import zlib

        net = jn.Network()
        stars = net.add_node("generate", "stars")
        stars.count = 200
        look = net.add_node("wrangle", "look")
        look.code = "radius = 0.5; alpha = 1; softness = 1;"
        image = net.add_node("render", "image")
        image.width = 64
        image.height = 48
        stars >> look >> image
        geometry = net.cook(image)
        folder = tempfile.mkdtemp()
        path = os.path.join(folder, "sky.png")
        try:
            self.assertEqual(geometry.render_png(path), (64, 48))
            self.assertEqual(geometry.render_png(path, 32, 24), (32, 24))
            with open(path, "rb") as stream:
                data = stream.read()
            self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(int.from_bytes(data[16:20], "big"), 32)
            idat = data.index(b"IDAT")
            length = int.from_bytes(data[idat - 4:idat], "big")
            pixels = zlib.decompress(data[idat + 4:idat + 4 + length])
            self.assertTrue(any(pixels), "the image is not black")
        finally:
            os.remove(path)
            os.rmdir(folder)

    def test_the_string_table_outlives_the_network(self):
        net, create = self.attrib_graph(name="label", type="string", text="kept")
        geometry = net.cook(create)
        ids = geometry.point_attrib("label")
        del net, create
        gc.collect()
        self.assertEqual(geometry.string_values(jn.AttribClass.point, "label")[0], "kept")
        self.assertEqual(int(ids[0]), 1)

    def test_names_and_missing_attributes(self):
        net, _line, _wrangle, out = line_graph()
        geometry = net.cook(out)
        self.assertIn("x", geometry.attrib_names(jn.AttribClass.point))
        self.assertEqual(geometry.attrib_type(jn.AttribClass.point, "x"), jn.AttribType.float32)
        with self.assertRaises(KeyError):
            geometry.point_attrib("nope")
        with self.assertRaises(KeyError):
            geometry.prim_attrib("nope")
        with self.assertRaises(TypeError):
            geometry.strings(jn.AttribClass.point, "x")


if __name__ == "__main__":
    unittest.main()
