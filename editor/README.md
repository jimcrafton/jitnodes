# Editor

UI for jitnodes, built on newui (see "Editor UI" in `../docs/design.md`). Screens are HTML mockups in `mockups/`,
converted to `.newui` documents in `ui/`. Regenerate after changing a mockup, never hand-edit the `.newui`:

```
python ../.claude/skills/html-mockup-to-newui/scripts/html2newui.py mockups/mainWindow.html -o ui/mainWindow.newui --name mainWindow --title "jitnodes Editor"
python ../.claude/skills/newui-view-json5/scripts/validateNewui.py ui/mainWindow.newui
```

Open `mockups/mainWindow.html` in a browser to see the design (1280 x 800). The converted file is a Frame named
`mainWindow`, loaded with `newui::Bundle::loadFrame()` from `Resources/mainWindow.newui`.

## Building

The editor is opt-in (`JITNODES_BUILD_EDITOR`, Windows, single-config generators such as the Ninja presets) and needs
a newui checkout next to this repo (`JITNODES_NEWUI_DIR`, default `../newui`) whose reflectgen venv is set up
(`tools/reflectgen/README.md`, "Setup"):

```
cmake --preset x64-release -DJITNODES_BUILD_EDITOR=ON
cmake --build --preset x64-release --target jitnodesEditor
out\build\x64-release\editor\jitnodesEditor.exe --check    # loads Resources\mainWindow.newui, checks every named view, exits
out\build\x64-release\editor\jitnodesEditor.exe            # opens the window
out\build\x64-release\editor\jitnodesEditor.exe graph.json # opens the window with a graph loaded
out\build\x64-release\editor\jitnodesEditor.exe ..\examples\graphs\attributes.json --attribute height   # and colours by an attribute
```

newui is built by an `ExternalProject` in its own build tree (`<build>/newui`), so its Blend2D and the asmjit embedded in
it (namespace `abi_bl`) stay separate from jitnodes' asmjit (`v1_21`) and the two never clash in the editor binary. Every
build of newui bumps `VERSION.state` in the newui checkout, as any newui build does. The first build compiles Blend2D and
takes a few minutes; later builds only rebuild what changed. `ui/mainWindow.newui` is copied to `Resources/` beside the exe.

## Code

- `editorModel.h/.cpp` (`editorCore`, no UI dependency): `EditorModel` owns the `Network` and `History`, the file path and
  modified flag, selection, the playhead and the last cook result. `addNode` wires the selected node into the new node's
  first input, makes it the display node and commits an undo step. Covered by `editorModelTests` (run from ctest when
  `JITNODES_BUILD_TESTS` is on).
- `editorWindow.h/.cpp`: `EditorWindow` looks up every view by name and connects it to the model; a missing view or a
  view of the wrong class throws with its name. Errors thrown by an action (a bad file, an unknown type) are shown in
  `statusText`. Playback uses a 33 ms `RunLoop::postDelayed` timer.
- `graphLayout.*`: `placeNewNodes` gives nodes without a position one (below their first input, otherwise in the top row,
  moving right past other nodes). `EditorModel` calls it before every commit, so layout never shows up as an edit.
- `graphController.*` (`editorCore`, no UI dependency): the interaction logic of the graph canvas, driven by screen-space
  events and tested without a window. `graphCanvas.*`: `GraphCanvas`, a newui `SubView` that draws the graph with Blend2D
  and forwards mouse and key events to the controller. It is added as the only child of the mockup's `graphCanvas`
  placeholder, which gets a `FlexLayout` so the canvas always fills it.
- `parameterPanel.*`: `ParameterPanel` fills the mockup's `paramList` placeholder with a scroll view and builds one row per
  parameter of the selected node (name, text field, `fx`, `key`; a multi-line field for `code`). The row logic lives in
  `EditorModel` (`parameters`, `setParameter`, `toggleExpression`, `keyframeNow`) and is tested without a window.
- `viewportController.*` (`editorCore`, no UI dependency): the 2D viewport's view logic, tested without a window: bounds of
  the geometry, fit, pan, zoom around the cursor, world y pointing up, and a stride that keeps huge geometries within a
  drawing budget. `viewportCanvas.*`: `ViewportCanvas` draws the display node's cooked geometry with Blend2D inside the
  mockup's `viewportCanvas` placeholder. `drawText.h`: the shared text helper of both canvases.
- `profileController.*` (`editorCore`, no UI dependency): scrolling, clamping and row hit testing of the profile list.
  `EditorModel::profile()` builds the rows from the cook statistics. `profileCanvas.*`: `ProfileCanvas` draws them in the
  mockup's `profileRows` placeholder, with the columns lined up under the header labels.
- `keyframeEditor.*` (`editorCore`, no UI dependency): `KeyframeEditor` is the working copy of one parameter's keys used by
  the keyframe dialog (sorted, no two keys at one time, add on the curve, range for drawing), tested without a window.
  `keyframeDialog.*`: `KeyframeDialog`, a modal newui `Dialog` loaded from `ui/keyframeDialog.newui` (mockup
  `mockups/keyframeDialog.html`). Its deferred actions use `RunLoop::postIdle`, because the modal loop runs idle tasks but
  ignores the thread message `post` uses.
- Subnet view: `EditorModel` keeps the path of subnet nodes the editor has entered; `network()` is the network being edited
  (the root graph or the contents of the entered subnet) and `rootNetwork()` the root, so the canvas, parameter panel and
  every edit follow without changes. `cook()` cooks each subnet on the way in first, so inner networks hold their inputs and
  the viewport shows the output of the network being edited. History stays one root-level history: an inner edit changes the
  subnet node's text, and undo that removes the subnet you are in leaves it.
- Assets: `EditorModel::createAsset`, `loadAssetFile`, `loadAssetsFrom`, `unlockAsset` and `assetNames` (tested without a
  window) over the library's `saveAsset`, `registerAsset`, `updateAssetInstances` and `unlockAssetInstance`. At startup the
  editor registers every `*.asset.json` in the `Assets` folder beside the exe (CMake copies `examples/assets` there).
- `main.cpp`: loads the frame, builds the model and window. `--check` clicks the wired buttons through their delegates
  (add nodes, undo, redo), sends mouse events to the canvas and paints it into an image, so the whole path from view
  to model is tested without a mouse or a visible window.

Wired so far: New/Open/Save (Save asks for a path the first time; New and Open ask before discarding changes), Undo/Redo
(enabled from `canUndo`/`canRedo`), the eleven palette buttons, Play/Pause with the time slider and label, the fusion
toggle, thread count and instruction set drop-downs, the node title/type, status, cook info, profile summary and the window
title (`name*` when modified). The graph starts empty: click a palette button (for example `line`, then `transform`) to add nodes. Graph canvas: left-drag a node to move it; left-drag from an output port (bottom centre) to a node or an input port (top)
to connect; left-drag a connected input port to detach and re-drop the wire; double-click a node to make it the display
node; Delete removes the selected node; left-drag on empty space or middle/right-drag pans; the wheel zooms around the
cursor; Fit frames the whole graph (also after Open). Every change is one undo step; a cycle is refused and reported.
Node positions are saved with the graph (`"pos": [x, y]`) and follow undo.

Parameter panel: select a node and its parameters appear. Press Enter or leave the field to apply it. A number sets the value
and removes any expression or keyframes; anything else is an expression (`sin(time) * 2`, `parent.amount`, `node.param`),
and a bad one is rejected with the message in the panel's status line and the field reverts. Text parameters (`class`,
`code`) are set as typed. `fx` turns a value into an expression holding that value and back; `key` sets a linear
keyframe at the playhead (a second press at the same time updates it). Expression fields are orange, keyframed ones
green and show the value at the playhead. Every edit is one undo step labelled `edit <node>.<param>`. The Display node
check box makes the selected node the display node.

Viewport: shows the geometry of the display node after every cook, so editing a parameter or moving the playhead redraws it.
Primitives are stroked as polylines (closed ones are closed); points are drawn as dots for geometry without primitives or
with at most 64 points. Left, middle or right drag pans, the wheel zooms around the cursor, and Fit or a double click frames
the geometry (the first geometry after New or Open is fitted automatically). The grid steps by powers of ten and its step
is shown in the corner; the header shows the point and primitive counts. Geometry over 40,000 points is drawn with every
n-th point.

Renaming and the clipboard: the Name field in the parameter panel (Enter or Rename) renames the selected node; names are
identifiers, unique in the network, and expressions that refer to `oldName.param` are rewritten. Copy, Cut and Paste (the
buttons above the graph, or Ctrl+C, Ctrl+X, Ctrl+V with the graph focused) work on the selected node: parameters,
expressions, keyframes, spare parameters and a subnet's contents. A paste gets a unique name (`wrangle1`, `wrangle3`, ...),
is offset 30 px further for every paste of the same copy, reconnects its inputs to nodes of the same names in the current
network, is selected, and leaves the display node alone. The clipboard lives in the editor and is also put on the system clipboard as text (see
"Shortcuts and renaming" below), so it survives New and Open.

Selection: click selects a node, shift- or ctrl-click toggles it, and dragging on empty space draws a selection box (with
shift or ctrl the nodes it touches are added to the current selection); Ctrl+A selects everything in the current network.
Dragging any selected node moves the whole selection as one undo step, and Delete, Copy, Cut and Paste work on all selected
nodes. The parameter panel, Name field, Display node and Enter (go into a subnet) work only while exactly one node is
selected; the panel title says "N nodes selected" otherwise. Panning is now middle or right button drag (a left drag on empty
space is the selection box). Pasting several nodes keeps the wires between them (the copies are wired to each other), points
expressions that refer to a copied node at its copy, and reconnects wires from outside the copied set to nodes of the same
names.

Sprites: when the display geometry has a point attribute `radius` and no colour attribute is chosen, each point is drawn
as an alpha-blended sprite in index order. Optional point attributes: `Cd.x`, `Cd.y`, `Cd.z` (colour 0 to 1, default
white), `alpha` (default 1), `softness` (0 hard disc, 1 radial gradient from `alpha` at the centre to transparent at the
edge; default 1), `aspect` (width over height; 0 or less means 1) and `shape` (0.5 or more draws a box and ignores
`softness`). Missing attributes after a merge read as 0, so set what you need explicitly. `examples/graphs/clouds.json`
(sky gradient from stacked boxes, sun glow, drifting clouds, hills) and `bokeh.json` use it; `rosette.json` is a plain
copyToPoints example.

Colour by attribute: the Color by list under the viewport title holds the point attributes of the display geometry (including
`x` and `y`), the length of every vector attribute (`N (length)` for `N.x`, `N.y`, optionally `N.z`) and primitive
attributes (`prim: name`). Choosing one colours the lines (each segment by the average of its end points; a primitive
attribute colours the whole primitive) and the dots with a viridis-like map from purple (smallest) to yellow (largest), and
draws a legend with the range in the corner; a constant attribute gets a range of one around its value and values that are
not finite are grey. The Map list picks another colour map (Viridis, Magma, Grayscale, Cool-warm); the Range fields set
the smallest and largest value by hand (empty is auto for that end, an inverted range reverses the map) and Auto clears both.
The range resets when another attribute is chosen. The choice is remembered by name while the graph changes, so it comes back when the attribute does.
Hovering within 14 px of a point rings it and shows its index, position and the attribute value. Colouring draws at most
20,000 segments (every n-th point beyond that); hover search is skipped above 300,000 points. The graph header has its own Fit button.

Profile list: one row per node with its type, number of cooks, last cook time and a note (`fused chain of N`, `fused into
<node>`, `compiled x ms`); nodes inside a subnet follow it, indented. It updates after every cook, the wheel scrolls it,
clicking a row selects that node (root-graph rows only), and Reset clears the statistics. Nodes that were fused into
another (not cooked on their own) show `-` for the time. The summary line shows the kernels compiled and the thread count.

Keyframe editing: the `...` button of a float row opens the keyframe dialog. It shows the curve (blue) with the keys as squares
and the playhead as an orange line, and one row per key: time, value, interpolation (constant, linear or smooth, applying
to the segment that starts at the key) and a delete button. Press Enter or leave a field to apply it; changing a time
re-sorts the keys; a time that another key already uses, or text that is not a number, is refused with a message and the
field reverts. Add key at playhead adds a key on the current curve, so the animation does not change. OK replaces the
parameter's keys as one undo step (`keys <node>.<param>`); with no keys left the parameter becomes a plain value holding
what it had at the playhead. Cancel changes nothing.

Subnets: double-click a subnet node (or select it and press Enter) to enter it; Up (or Backspace) leaves it and selects the
subnet again. The header shows the path (`/subnet2/inner`). Inside, everything works as at the top level, and the first node
you add becomes the subnet's output (its display node). Add `subnetInput` nodes (palette) and set their `index` to read the
subnet's inputs; select a subnet and use Add parameter to promote a float parameter, which nodes inside read as
`parent.<name>` (type it as an expression, for example `parent.amount * 2`). Double-clicking any other node still makes it
the display node. Asset instances cannot be entered (their contents come from the asset definition). The profile list
shows the whole tree, with the highlight only while at the top level (node ids repeat between networks), and the viewport
and thread, instruction-set and fusion settings are global.

Assets: select a subnet and press Create asset... to save it as an asset definition (the file name before the first dot is
the asset's type name; the version is one higher than the registered one, 1 for a new asset). The subnet is replaced by an
instance of the new asset with the same name, wiring and promoted parameter values, and instances of an older version are
updated (which resets the undo history, and the status line says so). Registered assets appear in the Assets list of the
palette, a list under the heading with a line of details (version, inputs, promoted parameters, instances in the graph, source
file): choose one and press Add asset, Reload asset to read its source file again (instances are rebuilt), or Load asset... to
register a definition file. Select an instance and press Unlock
to turn it back into an ordinary subnet holding a copy of the contents. Graph files embed the definitions they use, so they
open without the asset files. Instances cannot be entered; unlock them first, or edit the definition and save it again.

Not wired: removing or renaming a promoted parameter (the library has no API for it), subnets with more than one input from
the palette (use the file format), saving the changes of an unlocked subnet back to its asset in one step (use Create asset... with the same name), dragging keys on the curve, tangent handles, string expressions.

## Looking at the running window

`uiShot.ps1` starts the editor, optionally posts mouse clicks to it (client coordinates of the root view, separated by `;`),
and saves a screenshot of the window with `PrintWindow`, which works even when another window is in front. Other windows of
the process (such as the keyframe dialog) are saved as `<out>-1.png`, and `-dialogClicks` clicks inside the first of them:

```
powershell -File editor/uiShot.ps1 -out shot.png -clicks "81,90;81,118"    # clicks the line and transform palette buttons
```

## Main window

| Area | Views (by `name`) | Wired to |
|---|---|---|
| Toolbar | `newButton`, `openButton`, `saveButton` | `loadNetworkFromFile` / `saveNetworkToFile`, `Network::clear` |
| | `undoButton`, `redoButton` | `History::undo` / `redo` (enable from `canUndo` / `canRedo`, tooltip from the labels) |
| | `playButton`, `timeSlider`, `timeLabel` | `Network::setTime` on a timer; slider 0..100 maps to the playhead range |
| | `fusionToggle`, `threadsSelect`, `isaSelect` | `setFusionEnabled`, `setThreadCount`, `KernelCompiler::setIsa` (models filled in code) |
| Palette | `addLine`, `addTransform`, ... `addNull` (one per node type) | `Network::addNode` at the canvas centre, then `History::commit` |
| Graph | `graphCanvas`, `graphStatus` | `GraphCanvas` (Blend2D): nodes, wires, display flag, select/move/connect/delete |
| Parameters | `nodeTitle`, `nodeType`, `paramSx` ... `keyTy`, `displayToggle`, `paramError` | sample rows for a transform; rebuilt per selected node from `floatNames` / `stringNames`, `fx` opens `setExpression`, `key` opens `setKeyframes` |
| Viewport | `viewportCanvas`, `fitButton` | placeholder drawn from the display node's cooked geometry (SVG path / Blend2D) |
| Profile | `profileRows`, `profileSummary`, `resetProfileButton` | placeholder list from `Network::profileReport` data (`NodeStats`), `resetProfile` |
| Status | `statusText`, `cookInfo` | cook errors (`std::runtime_error` text), last cook time |

What the mockup cannot hold and the editor code has to supply: the three canvases, the `DropDownList` models, the
parameter rows for the selected node (the mockup shows one transform), and the list rows of the profile table.

## Shortcuts and renaming

F2 over the graph moves the focus to the node name field with its text selected (Enter renames). With several nodes selected,
the name field renames all of them as one undo step: the name's letters followed by 1, 2, ... in id order (`wall` gives `wall1`,
`wall2`). The Display node toggle works on the only selected node, or on the last of several.

Copy and cut also put the nodes on the system clipboard as JSON text (`format: "jitnodes-nodes"`), and paste reads it first, so
nodes can be pasted into another editor window, a text editor or a file, and back. Other clipboard text is ignored. Asset
definitions are not included: pasting an instance of an asset the receiving editor has not loaded reports an error. The Paste
button enables itself when the canvas is entered or focused if the clipboard holds nodes.

The arrow keys over the graph move the selected nodes by 10 units (Shift: 50), one undo step per key press.

## GUI tests

`tests/gui/test_editor_gui.py` drives the built editor with newui's guitest driver (real mouse and keyboard through
`--automation <pipe>`; see `D:\code\newui\automation.md`). It needs an idle desktop with nothing covering the window:

```
python tests\gui\test_editor_gui.py --exe ..\out\build\x64-release-editor\editor\jitnodesEditor.exe
```

`JITNODES_GUI_AUTOMATION` (default ON) links the automation server; turn it off for a shipping build.
