r"""GUI tests for jitnodesEditor, driven with newui's guitest (real SendInput; needs an idle desktop, nothing covering the window):

    python editor\tests\gui\test_editor_gui.py --exe D:\code\jitnodes\out\build\x64-release-editor\editor\jitnodesEditor.exe
"""
import os
import sys

sys.path.insert(0, r"D:\code\newui\tools\guitest")

import newui_guitest
from newui_guitest import AppTestCase

NODE_W, NODE_H, ROW_Y, FIRST_X, FIRST_Y = 120, 36, 86, 60, 60
GRAPHS = r"D:\code\jitnodes\examples\graphs"
VK_SHIFT = 0x10
CF_UNICODETEXT = 13


def _clipboardApi():
    import ctypes
    from ctypes import wintypes
    user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32
    user32.OpenClipboard.argtypes = [wintypes.HWND]
    user32.GetClipboardData.argtypes = [wintypes.UINT]
    user32.GetClipboardData.restype = wintypes.HANDLE
    user32.SetClipboardData.argtypes = [wintypes.UINT, wintypes.HANDLE]
    user32.SetClipboardData.restype = wintypes.HANDLE
    kernel32.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
    kernel32.GlobalAlloc.restype = wintypes.HGLOBAL
    kernel32.GlobalLock.argtypes = [wintypes.HGLOBAL]
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalUnlock.argtypes = [wintypes.HGLOBAL]
    return ctypes, user32, kernel32


def _openClipboard(user32):
    import time
    for _ in range(50):
        if user32.OpenClipboard(None):
            return
        time.sleep(0.02)
    raise RuntimeError("could not open the clipboard")


def clipboardText():
    ctypes, user32, kernel32 = _clipboardApi()
    _openClipboard(user32)
    try:
        handle = user32.GetClipboardData(CF_UNICODETEXT)
        if not handle:
            return ""
        pointer = kernel32.GlobalLock(handle)
        try:
            return ctypes.wstring_at(pointer)
        finally:
            kernel32.GlobalUnlock(handle)
    finally:
        user32.CloseClipboard()


def setClipboardText(text):
    ctypes, user32, kernel32 = _clipboardApi()
    _openClipboard(user32)
    try:
        user32.EmptyClipboard()
        if text:
            data = (text + "\0").encode("utf-16-le")
            handle = kernel32.GlobalAlloc(0x0002, len(data))
            pointer = kernel32.GlobalLock(handle)
            ctypes.memmove(pointer, data, len(data))
            kernel32.GlobalUnlock(handle)
            user32.SetClipboardData(CF_UNICODETEXT, handle)
    finally:
        user32.CloseClipboard()


class EditorCase(AppTestCase):
    def setUp(self):
        setClipboardText("")      # the system clipboard is shared with the editor's copy and paste
        super().setUp()

    def add(self, *buttons):
        for name in buttons:
            self.app.click(name)

    def label(self, name):
        return self.app.prop(name, "text")

    def status(self):
        return self.label("graphStatus")

    def title(self):
        return self.label("nodeTitle")

    def emptyPoint(self):
        r = self.app.find("graphCanvas")
        return (r.x + r.w - 12, r.y + r.h - 12)

    def nodePoint(self, column, row, dx=0, dy=0):
        r = self.app.find("graphCanvas")
        return (r.x + FIRST_X + column * (NODE_W + 30) + NODE_W // 2 + dx, r.y + FIRST_Y + row * ROW_Y + NODE_H // 2 + dy)

    def selectAll(self):
        self.app.click_at(self.emptyPoint())
        self.app.key("ctrl+a")

    def setField(self, name, text):
        self.app.click(name)
        self.app.key("ctrl+a")
        self.app.type(text)
        self.app.key("enter")

    def shiftClick(self, point):
        self.app._send([self.app._key(VK_SHIFT)])
        try:
            self.app.click_at(point)
        finally:
            self.app._send([self.app._key(VK_SHIFT, up=True)])


class StartUp(EditorCase):
    def test_the_editor_starts_and_speaks_the_protocol(self):
        self.assertTrue(self.app.running)
        self.assertGreater(self.app.find("graphCanvas").w, 0)


class Palette(EditorCase):
    def test_palette_buttons_add_nodes(self):
        self.add("addLine", "addTransform")
        self.assertTrue(self.status().startswith("2 nodes"), self.status())

    def test_undo_and_redo(self):
        self.add("addLine", "addTransform", "undoButton")
        self.assertTrue(self.status().startswith("1 node"), self.status())
        self.app.click("redoButton")
        self.assertTrue(self.status().startswith("2 nodes"), self.status())

    def test_adding_a_node_cooks_the_display_node(self):
        self.add("addLine")
        self.assertIn("100 points", self.label("cookInfo"))
        self.assertIn("100 points", self.label("viewportInfo"))


class GraphCanvas(EditorCase):
    def test_clicking_a_node_selects_it(self):
        self.add("addLine", "addTransform")
        self.app.click_at(self.nodePoint(0, 0))
        self.assertEqual("line1", self.title())
        self.app.click_at(self.emptyPoint())
        self.assertEqual("no selection", self.title())

    def test_dragging_a_node_moves_it(self):
        self.add("addLine")
        self.app.drag(self.nodePoint(0, 0), self.nodePoint(0, 0, 240, 0))
        self.assertEqual("line1", self.title())
        self.app.click_at(self.emptyPoint())
        self.app.click_at(self.nodePoint(0, 0))
        self.assertEqual("no selection", self.title(), "the old spot is empty")
        self.app.click_at(self.nodePoint(0, 0, 240, 0))
        self.assertEqual("line1", self.title())

    def test_the_move_is_one_undo_step(self):
        self.add("addLine")
        self.app.drag(self.nodePoint(0, 0), self.nodePoint(0, 0, 240, 0))
        self.app.click("undoButton")
        self.app.click_at(self.emptyPoint())
        self.app.click_at(self.nodePoint(0, 0))
        self.assertEqual("line1", self.title())
        self.assertTrue(self.status().startswith("1 node"), self.status())

    def test_delete_removes_the_selected_node(self):
        self.add("addLine", "addTransform")
        self.app.click_at(self.nodePoint(0, 1))
        self.app.key("delete")
        self.assertTrue(self.status().startswith("1 node"), self.status())


class MultiSelect(EditorCase):
    def test_box_select_selects_every_node(self):
        self.add("addLine", "addTransform", "addNull")
        r = self.app.find("graphCanvas")
        self.app.drag(self.emptyPoint(), (r.x + 4, r.y + 4))
        self.assertEqual("3 nodes selected", self.title())

    def test_a_partial_box_selects_only_what_it_touches(self):
        self.add("addLine", "addTransform", "addNull")
        r = self.app.find("graphCanvas")
        self.app.drag((r.x + 4, r.y + 4), (r.x + FIRST_X + NODE_W + 10, r.y + FIRST_Y + ROW_Y + NODE_H // 2))
        self.assertEqual("2 nodes selected", self.title())

    def test_ctrl_a_selects_all(self):
        self.add("addLine", "addTransform")
        self.selectAll()
        self.assertEqual("2 nodes selected", self.title())

    def test_shift_click_extends_the_selection(self):
        self.add("addLine", "addTransform")
        self.app.click_at(self.nodePoint(0, 0))
        self.shiftClick(self.nodePoint(0, 1))
        self.assertEqual("2 nodes selected", self.title())

    def test_group_drag_moves_every_selected_node(self):
        self.add("addLine", "addTransform")
        self.selectAll()
        self.app.drag(self.nodePoint(0, 0), self.nodePoint(0, 0, 240, 0))
        self.app.click_at(self.emptyPoint())
        for row in (0, 1):
            self.app.click_at(self.nodePoint(0, row))
            self.assertEqual("no selection", self.title())
            self.app.click_at(self.nodePoint(0, row, 240, 0))
            self.assertNotEqual("no selection", self.title())

    def test_copy_and_paste_with_the_keyboard(self):
        self.add("addLine", "addTransform")
        self.selectAll()
        self.app.key("ctrl+c")
        self.app.key("ctrl+v")
        self.assertTrue(self.status().startswith("4 nodes"), self.status())

    def test_cut_removes_the_selection_and_undo_restores_it(self):
        self.add("addLine", "addTransform")
        self.selectAll()
        self.app.key("ctrl+x")
        self.assertTrue(self.status().startswith("0 nodes"), self.status())
        self.app.click("undoButton")
        self.assertTrue(self.status().startswith("2 nodes"), self.status())


class Nudging(EditorCase):
    def test_arrow_keys_move_the_selected_node(self):
        self.add("addLine")
        self.app.click_at(self.nodePoint(0, 0))
        for _ in range(3):
            self.app.key("shift+right")
        self.app.click_at(self.emptyPoint())
        self.app.click_at(self.nodePoint(0, 0))
        self.assertEqual("no selection", self.title(), "the old spot is empty")
        self.app.click_at(self.nodePoint(0, 0, 150, 0))
        self.assertEqual("line1", self.title())

    def test_plain_arrows_move_by_one_step(self):
        self.add("addLine")
        self.app.click_at(self.nodePoint(0, 0))
        for _ in range(10):
            self.app.key("down")
        self.app.click_at(self.emptyPoint())
        self.app.click_at(self.nodePoint(0, 0, 0, 100))
        self.assertEqual("line1", self.title())

    def test_the_whole_selection_moves_and_undo_steps_back(self):
        self.add("addLine", "addTransform")
        self.selectAll()
        for _ in range(3):
            self.app.key("shift+right")
        self.app.click("undoButton")
        self.app.click_at(self.emptyPoint())
        self.app.click_at(self.nodePoint(0, 1, 100, 0))
        self.assertEqual("transform2", self.title(), "two of the three moves remain")

    def test_arrows_do_nothing_without_a_selection(self):
        self.add("addLine")
        self.app.click_at(self.emptyPoint())
        for _ in range(5):
            self.app.key("shift+right")
        self.app.click_at(self.nodePoint(0, 0))
        self.assertEqual("line1", self.title())


class Clipboard(EditorCase):
    def test_paste_is_disabled_until_something_is_copied(self):
        self.add("addLine")
        self.assertFalse(self.app.prop("pasteButton", "enabled"))
        self.app.click("copyButton")
        self.assertTrue(self.app.prop("pasteButton", "enabled"))

    def test_paste_adds_a_uniquely_named_copy(self):
        self.add("addLine", "copyButton", "pasteButton")
        self.assertEqual("line2", self.title())
        self.assertTrue(self.status().startswith("2 nodes"), self.status())

    def test_cut_button_removes_the_node(self):
        self.add("addLine", "cutButton")
        self.assertTrue(self.status().startswith("0 nodes"), self.status())


class SystemClipboard(EditorCase):
    NODES = ('{ format: "jitnodes-nodes", version: 1, nodes: [ { type: "line", name: "fromElsewhere", inputs: [], '
             'body: { type: "line", name: "fromElsewhere", floats: { count: 10, length: 1 } } } ] }')

    def test_copy_puts_the_nodes_on_the_system_clipboard_as_text(self):
        self.add("addLine", "copyButton")
        self.assertIn("jitnodes-nodes", clipboardText())
        self.assertIn("line1", clipboardText())

    def test_cut_does_too(self):
        self.add("addLine", "cutButton")
        self.assertIn("jitnodes-nodes", clipboardText())

    def test_nodes_copied_elsewhere_can_be_pasted(self):
        setClipboardText(self.NODES)
        self.app.click("graphCanvas")
        self.assertTrue(self.app.prop("pasteButton", "enabled"))
        self.app.key("ctrl+v")
        self.assertTrue(self.status().startswith("1 node"), self.status())
        self.assertEqual("fromElsewhere", self.title())

    def test_other_text_is_ignored(self):
        setClipboardText("just some words")
        self.add("addLine")
        self.assertFalse(self.app.prop("pasteButton", "enabled"))
        self.app.click_at(self.emptyPoint())
        self.app.key("ctrl+v")
        self.assertTrue(self.status().startswith("1 node"), self.status())

    def test_a_second_editor_pastes_what_the_first_copied(self):
        self.add("addLine", "addTransform")
        self.selectAll()
        self.app.key("ctrl+c")
        other = self.launch()
        self.addCleanup(other.close)
        other.click_at((int(other.find("graphCanvas").x + 20), int(other.find("graphCanvas").y + 20)))
        other.key("ctrl+v")
        self.assertTrue(other.prop("graphStatus", "text").startswith("2 nodes"), other.prop("graphStatus", "text"))

    def test_the_clipboard_is_still_there_after_the_nodes_are_deleted(self):
        self.add("addLine", "copyButton", "cutButton")
        self.app.click("pasteButton")
        self.assertTrue(self.status().startswith("1 node"), self.status())


class Rename(EditorCase):
    def test_rename_button_renames_the_selected_node(self):
        self.add("addLine")
        self.setField("nodeName", "shifter")
        self.app.click("renameButton")
        self.assertEqual("shifter", self.title())

    def test_an_invalid_name_is_rejected(self):
        self.add("addLine")
        self.setField("nodeName", "bad name")
        self.app.click("renameButton")
        self.assertEqual("line1", self.title())


class MultiRename(EditorCase):
    def test_f2_focuses_the_name_field(self):
        self.add("addLine")
        self.app.click_at(self.nodePoint(0, 0))
        self.app.key("f2")
        self.assertEqual("nodeName", self.app.focused_name())
        self.app.type("shifter")
        self.app.key("enter")
        self.assertEqual("shifter", self.title())

    def test_renaming_several_nodes_numbers_them(self):
        self.add("addLine", "addTransform")
        self.selectAll()
        self.app.key("f2")
        self.app.type("wall")
        self.app.click("renameButton")
        self.app.click_at(self.nodePoint(0, 0))
        self.assertEqual("wall1", self.title())
        self.app.click_at(self.nodePoint(0, 1))
        self.assertEqual("wall2", self.title())

    def test_the_display_toggle_works_with_several_selected(self):
        self.add("addLine", "addTransform")
        self.app.click_at(self.nodePoint(0, 0))
        self.app.click("displayToggle")
        self.shiftClick(self.nodePoint(0, 1))
        self.assertEqual("2 nodes selected", self.title())
        self.assertTrue(self.app.prop("displayToggle", "enabled"))
        self.assertFalse(self.app.prop("displayToggle", "checked"))
        self.app.click("displayToggle")
        self.assertTrue(self.app.prop("displayToggle", "checked"))


class Parameters(EditorCase):
    def test_the_panel_lists_the_selected_nodes_parameters(self):
        self.add("addLine")
        self.assertGreater(self.app.find("param.count").w, 0)
        self.assertGreater(self.app.find("param.length").w, 0)

    def test_selecting_another_node_rebuilds_the_rows(self):
        self.add("addLine", "addTransform")
        self.assertGreater(self.app.find("param.tx").w, 0)
        self.app.click_at(self.nodePoint(0, 0))
        self.app.wait_until(lambda: self.app.find("param.count").w > 0, "the line's rows")

    def test_typing_a_number_sets_the_parameter(self):
        self.add("addLine", "addTransform")
        self.setField("param.tx", "5")
        self.assertEqual("5", self.app.text_of("param.tx"))

    def test_text_that_is_not_a_number_becomes_an_expression(self):
        self.add("addLine", "addTransform")
        self.setField("param.tx", "sin(time) * 2")
        self.assertEqual("sin(time) * 2", self.app.text_of("param.tx"))

    def test_a_bad_expression_is_rejected_and_the_field_reverts(self):
        self.add("addLine", "addTransform")
        self.setField("param.tx", "sin(time) * 2")
        self.setField("param.tx", "1 +")
        self.assertEqual("sin(time) * 2", self.app.text_of("param.tx"))

    def test_an_edit_is_an_undo_step(self):
        self.add("addLine", "addTransform")
        self.setField("param.tx", "5")
        self.app.click("undoButton")
        self.assertNotEqual("5", self.app.text_of("param.tx"))


class Viewport(EditorCase):
    def test_the_viewport_follows_the_display_node(self):
        self.add("addLine", "addResample")
        self.assertIn("points", self.label("viewportInfo"))


class ColorByAttribute(EditorCase):
    args = (os.path.join(GRAPHS, "attributes.json"), "--attribute", "height")

    def test_the_graph_opens_and_the_viewport_shows_geometry(self):
        self.assertIn("points", self.label("viewportInfo"))


class ColorRange(EditorCase):
    args = (os.path.join(GRAPHS, "attributes.json"), "--attribute", "height")

    def test_the_range_fields_start_empty_and_are_enabled_for_an_attribute(self):
        self.assertEqual("", self.app.text_of("rangeMin"))
        self.assertTrue(self.app.prop("rangeMin", "enabled"))
        self.assertTrue(self.app.prop("autoRangeButton", "enabled"))

    def test_a_typed_range_is_kept_and_auto_clears_it(self):
        self.setField("rangeMin", "0.25")
        self.setField("rangeMax", "0.75")
        self.assertEqual("0.25", self.app.text_of("rangeMin"))
        self.assertEqual("0.75", self.app.text_of("rangeMax"))
        self.app.click("autoRangeButton")
        self.assertEqual("", self.app.text_of("rangeMin"))
        self.assertEqual("", self.app.text_of("rangeMax"))

    def test_text_that_is_not_a_number_is_rejected(self):
        self.setField("rangeMin", "0.25")
        self.setField("rangeMax", "abc")
        self.assertEqual("0.25", self.app.text_of("rangeMin"))
        self.assertEqual("", self.app.text_of("rangeMax"))
        self.assertIn("numbers", self.label("statusText"))

    def test_the_legend_changes_with_the_colour_map(self):
        before = self.app.screenshot()
        r = self.app.find("viewportCanvas")
        box = (int(r.x + r.w - 140), int(r.y + r.h - 24), int(r.x + r.w - 10), int(r.y + r.h - 16))
        self.app.click("mapSelect")
        self.app.key("down")
        self.app.key("enter")
        after = self.app.screenshot()
        self.assertGreater(before.changed(after, *box), 0, "the legend bar is drawn with another map")


class ColorBy(EditorCase):
    args = (os.path.join(GRAPHS, "attributes.json"),)

    def choose(self, steps):
        self.app.click("attrSelect")
        for _ in range(steps):
            self.app.key("down")
        self.app.key("enter")

    def test_nothing_is_chosen_at_first(self):
        self.assertFalse(self.app.prop("rangeMin", "enabled"))

    def test_choosing_an_attribute_turns_the_colouring_on(self):
        self.choose(1)
        self.assertTrue(self.app.prop("rangeMin", "enabled"))
        self.assertTrue(self.app.prop("autoRangeButton", "enabled"))

    def test_choosing_none_turns_it_off_again(self):
        self.choose(1)
        self.app.click("attrSelect")
        self.app.key("up")
        self.app.key("enter")
        self.assertFalse(self.app.prop("rangeMin", "enabled"))

    def test_the_viewport_is_recoloured(self):
        r = self.app.find("viewportCanvas")
        area = (int(r.x + 10), int(r.y + 10), int(r.x + r.w - 10), int(r.y + r.h - 40))
        before = self.app.screenshot()
        self.choose(1)
        self.assertGreater(before.changed(self.app.screenshot(), *area), 0)


class Sprites(EditorCase):
    args = (os.path.join(GRAPHS, "clouds.json"),)
    SUN = (255, 237, 166)

    def viewportShot(self):
        r = self.app.find("viewportCanvas")
        return self.app.screenshot(), (int(r.x + 4), int(r.y + 4), int(r.x + r.w - 4), int(r.y + r.h - 4))

    def sunPixels(self):
        shot, (x0, y0, x1, y1) = self.viewportShot()
        return shot.count(x0, y0, x1, y1, lambda p: all(abs(p[i] - self.SUN[i]) <= 6 for i in range(3)))

    def distinctColours(self):
        shot, (x0, y0, x1, y1) = self.viewportShot()
        seen = set()
        for y in range(y0, y1, 3):
            for x in range(x0, x1, 3):
                seen.add(tuple(c // 16 for c in shot.pixel(x, y)))
        return len(seen)

    def test_the_graph_is_drawn_as_sprites(self):
        self.assertIn("points", self.label("viewportInfo"))
        self.assertGreater(self.sunPixels(), 0, "the sun core is drawn in its own colour")
        self.assertGreater(self.distinctColours(), 30, "sky gradient, glow and clouds give many colours")

    def test_choosing_an_attribute_switches_the_sprites_off(self):
        self.assertGreater(self.sunPixels(), 0)
        self.app.click("attrSelect")
        self.app.key("down")
        self.app.key("enter")
        self.assertEqual(0, self.sunPixels())

    def test_choosing_none_brings_the_sprites_back(self):
        self.app.click("attrSelect")
        self.app.key("down")
        self.app.key("enter")
        self.app.click("attrSelect")
        self.app.key("up")
        self.app.key("enter")
        self.assertGreater(self.sunPixels(), 0)


class Profile(EditorCase):
    def test_clicking_a_profile_row_selects_its_node(self):
        self.add("addLine", "addTransform")
        self.app.click_at(self.nodePoint(0, 0))
        r = self.app.find("profileRows")
        self.app.click_at((r.x + 20, r.y + 30))
        self.assertEqual("transform2", self.title())


class Subnets(EditorCase):
    def test_double_click_enters_a_subnet_and_up_leaves_it(self):
        self.add("addLine", "addSubnet")
        self.app.double_click(self.nodePoint(0, 1))
        self.assertNotEqual("/", self.label("graphPath"))
        self.add("addSubnetInput", "addTransform")
        self.assertTrue(self.status().startswith("2 nodes"), self.status())
        self.app.click("upButton")
        self.assertEqual("/", self.label("graphPath"))

    def test_add_parameter_promotes_a_parameter(self):
        self.add("addLine", "addSubnet")
        self.app.click("spareName")
        self.app.type("amount")
        self.app.click("addSpareButton")
        self.assertGreater(self.app.find("param.amount").w, 0)


class Assets(EditorCase):
    def test_the_library_lists_the_loaded_assets_with_details(self):
        self.app.find_row("assetList", "wobbleAsset")
        details = self.label("assetDetails")
        self.assertIn("wobbleAsset", details)
        self.assertIn("1 input", details)
        self.assertIn("amount", details)
        self.assertIn("0 in the graph", details)
        self.assertIn("wobble.asset.json", details)

    def test_add_asset_and_unlock(self):
        self.add("addLine", "addAssetButton")
        self.assertTrue(self.status().startswith("2 nodes"), self.status())
        self.assertIn("1 in the graph", self.label("assetDetails"))
        self.assertTrue(self.app.prop("unlockAssetButton", "enabled"))
        self.app.click("unlockAssetButton")
        self.assertFalse(self.app.prop("unlockAssetButton", "enabled"))
        self.assertIn("0 in the graph", self.label("assetDetails"))

    def test_clicking_a_row_selects_it(self):
        self.app.click_row("assetList", "wobbleAsset")
        self.assertTrue(self.app.prop("addAssetButton", "enabled"))
        self.assertTrue(self.app.prop("reloadAssetButton", "enabled"))

    def test_reload_asset_keeps_the_instances(self):
        self.add("addLine", "addAssetButton", "reloadAssetButton")
        self.assertIn("1 in the graph", self.label("assetDetails"))
        self.assertTrue(self.status().startswith("2 nodes"), self.status())


class KeyframeDialog(EditorCase):
    """The dialog is a modal window of its own; newui's automation server answers while it is open."""

    def openDialog(self):
        self.add("addLine", "addTransform")
        self.app.click("keys.tx")
        self.app.wait_until(lambda: self.app.find("addKeyButton").w > 0, "the keyframe dialog")

    def isOpen(self):
        try:
            return self.app.find("addKeyButton").w > 0
        except newui_guitest.AutomationError:
            return False

    def test_the_dialog_opens_for_the_parameter(self):
        self.openDialog()
        self.assertIn("tx", self.app.prop("dlgTitle", "text"))

    def test_add_key_adds_a_row_at_the_playhead(self):
        self.openDialog()
        self.app.click("addKeyButton")
        self.app.wait_until(lambda: self.app.find("keyTime.0").w > 0, "the first key row")
        self.assertEqual("0", self.app.text_of("keyTime.0"))

    def test_editing_a_value_is_kept(self):
        self.openDialog()
        self.app.click("addKeyButton")
        self.app.wait_until(lambda: self.app.find("keyValue.0").w > 0, "the first key row")
        self.setField("keyValue.0", "5")
        self.assertEqual("5", self.app.text_of("keyValue.0"))

    def test_a_bad_value_is_rejected(self):
        self.openDialog()
        self.app.click("addKeyButton")
        self.app.wait_until(lambda: self.app.find("keyValue.0").w > 0, "the first key row")
        before = self.app.text_of("keyValue.0")
        self.setField("keyValue.0", "abc")
        self.assertEqual(before, self.app.text_of("keyValue.0"))
        self.assertIn("number", self.app.prop("dlgMessage", "text"))

    def test_delete_removes_the_row(self):
        self.openDialog()
        self.app.click("addKeyButton")
        self.app.wait_until(lambda: self.app.find("keyDelete.0").w > 0, "the first key row")
        self.app.click("keyDelete.0")
        self.app.wait_until(lambda: not self.hasRow("keyDelete.0"), "the row to go")

    def hasRow(self, name):
        try:
            return self.app.find(name).w > 0
        except newui_guitest.AutomationError:
            return False

    def test_cancel_closes_without_keys(self):
        self.openDialog()
        self.app.click("addKeyButton")
        self.app.click("cancelButton")
        self.app.wait_until(lambda: not self.isOpen(), "the dialog to close")
        self.app.click("keys.tx")
        self.app.wait_until(self.isOpen, "the dialog to open again")
        self.assertFalse(self.hasRow("keyValue.0"), "cancel kept no key")

    def test_ok_closes_and_keeps_the_keys(self):
        self.openDialog()
        self.app.click("addKeyButton")
        self.app.wait_until(lambda: self.app.find("keyValue.0").w > 0, "the first key row")
        self.setField("keyValue.0", "7")
        self.app.click("okButton")
        self.app.wait_until(lambda: not self.isOpen(), "the dialog to close")
        self.app.click("keys.tx")
        self.app.wait_until(self.isOpen, "the dialog to open again")
        self.app.wait_until(lambda: self.hasRow("keyValue.0"), "the saved key")
        self.assertEqual("7", self.app.text_of("keyValue.0"))


if __name__ == "__main__":
    sys.exit(newui_guitest.main())
