r"""GUI load tests of jitnodesEditor: a graph of thousands of nodes opened and edited through real input.

    python editor\tests\gui\test_editor_gui_load.py --exe D:\code\jitnodes\out\build\x64-release-editor\editor\jitnodesEditor.exe

The graph (2,646 nodes, 10,000 points per source) is written by the stress test: out\build\x64-release\jitnodesStress.exe --write-graph.
Each step has a generous time limit; the measured times are printed so a slowdown shows up before it becomes a failure.
"""
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, r"D:\code\newui\tools\guitest")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import newui_guitest
from test_editor_gui import EditorCase

STRESS = r"D:\code\jitnodes\out\build\x64-release\jitnodesStress.exe"
NODES = 2646
LIMIT = 30.0

_graph = None


def graphFile():
    global _graph
    if _graph is None:
        _graph = os.path.join(tempfile.gettempdir(), "jitnodesBigGraph.json")
        subprocess.run([STRESS, "--write-graph", _graph], check=True, stdout=subprocess.DEVNULL)
    return _graph


class BigGraph(EditorCase):
    def launch(self):
        self.args = (graphFile(),)
        return super().launch()

    def warmUp(self):
        # The first input sent to any editor, even an empty one, takes about a second; keep it out of the measured steps.
        self.app.click_at(self.emptyPoint())

    def waitForNodes(self, count, what):
        started = time.time()
        self.app.wait_until(lambda: self.status().startswith(f"{count} nodes"), what, timeout=LIMIT)
        elapsed = time.time() - started
        print(f"  {what}: {elapsed:.2f} s")
        return elapsed

    def timed(self, what, action):
        started = time.time()
        action()
        elapsed = time.time() - started
        print(f"  {what}: {elapsed:.2f} s")
        self.assertLess(elapsed, LIMIT, what)
        return elapsed

    def test_the_graph_opens_and_the_display_node_cooks(self):
        self.waitForNodes(NODES, "open and cook")
        self.assertIn("points", self.label("cookInfo"))

    def test_select_all_and_nudge_every_node(self):
        self.waitForNodes(NODES, "open")
        self.warmUp()
        self.timed("select all", self.selectAll)
        self.assertEqual(f"{NODES} nodes selected", self.title())
        self.timed("nudge 3 times", lambda: [self.app.key("shift+right") for _ in range(3)])
        self.timed("undo", lambda: self.app.click("undoButton"))
        self.assertTrue(self.status().startswith(f"{NODES} nodes"), self.status())

    def test_copy_and_paste_doubles_the_graph(self):
        self.waitForNodes(NODES, "open")
        self.selectAll()
        self.timed("copy", lambda: self.app.key("ctrl+c"))
        self.timed("paste", lambda: self.app.key("ctrl+v"))
        self.waitForNodes(NODES * 2, "paste of the whole graph")
        self.timed("undo the paste", lambda: self.app.click("undoButton"))
        self.waitForNodes(NODES, "undo")

    def test_cut_everything_and_undo(self):
        self.waitForNodes(NODES, "open")
        self.selectAll()
        self.timed("cut", lambda: self.app.key("ctrl+x"))
        self.waitForNodes(0, "cut of the whole graph")
        self.timed("undo the cut", lambda: self.app.click("undoButton"))
        self.waitForNodes(NODES, "undo")

    def test_adding_a_node_to_the_big_graph(self):
        self.waitForNodes(NODES, "open")
        self.timed("add a node", lambda: self.app.click("addLine"))
        self.waitForNodes(NODES + 1, "the added node")


if __name__ == "__main__":
    sys.exit(newui_guitest.main())
