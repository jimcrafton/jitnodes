# Plan: parameters declared in wrangle code

Status: planned 2026-10-27, nothing implemented yet. Goal: variables in wrangle code can be exposed in the editor as parameters
that the user adjusts with a slider, a typed number, an expression or keyframes.

## Findings

- Spare float parameters already exist (`Node::addSpareFloat`, `spare_`). They are saved (`"spare"` list in `serialize.cpp`), undone,
  copied, and support expressions and keyframes like declared floats. Load order in `applyNodeBody`: spare names, floats, strings,
  expressions, keyframes.
- `WrangleNode` (`src/graph/builtinNodes.cpp`) has fixed `a`, `b`, `c`, `d`. It caches its parse by `code` text and fills
  `out.params`, then `resize(spec.paramCount)`; the slots after the named ones belong to string literals (`KernelSpec::StringParam`),
  so a declared parameter must record its slot index rather than assume position.
- The parser (`src/jit/exprParser.cpp`) takes `paramNames` as a `const&` and resolves them in `primary()`; assignment to one fails
  with "cannot assign to parameter". `parseWrangle` is also used for parameter expressions (`node.cpp:83`) and `transform`.
- `EditorModel::parameters` (`editor/editorModel.cpp:303`) lists every float via `floatNames()`; `ParameterPanel` (`editor/parameterPanel.cpp`)
  is built in code (not `.newui`), one text field per row plus `fx`, `key` and `...` buttons. There is no slider row yet. newui has
  `newui::Slider` (`setRange`, `onValueChanged`, `setStep`) in `D:\code\newui\include\newui\controls.h`.
- `Node::setString` has no change hook, and there is no `removeSpareFloat`.

## Design

```
param gain = 1.5;            // text field, expression, keyframes
param radius = 2, 0, 10;     // default, min, max: also gets a slider
x = x * gain;
```

- `param` is a keyword only at the start of a statement when followed by an identifier (an attribute named `param` still works as
  `param = 3;`). Default and range are numeric literals, a leading `-` allowed. Declare before use.
- Each declaration becomes a spare float named after the variable and goes through the existing parameter block, so editing it never
  recompiles (the kernel cache key does not include parameter values).
- Editing `code` adds new parameters and removes ones no longer declared; a parameter that stays declared keeps its value,
  expression and keyframes. A parse error leaves the parameters alone; the cook reports it as today.
- The range only drives the slider; it never clamps the value.
- Rejected names: duplicates, `a`-`d`, `code`, `class`, `time`, `frame`, `pi`, `parent`, any existing non-spare parameter.
  Assigning to a declared name is "cannot assign to parameter".
- Open questions for the user: keep this syntax or use `float name = 1.5;`? A parameter without a range gets no slider (proposed)
  or a guessed one?

## Phases

1. **Parser** (`exprParser.cpp/.h`): add `struct WrangleParam {name, value, hasRange, lo, hi, slot}` and an optional out-parameter on
   `parseWrangle` (keep the existing signature working). Local copy of the name list so declared names resolve in `primary()`;
   `spec_.paramCount++` for the slot. Parser tests in `tests/testMain.cpp`.
2. **Node plumbing**: `Node::removeSpareFloat` (also `rebuildReferences`), a protected virtual called from `Node::setString` before
   `markChanged`, a virtual `floatRange(name, lo, hi)` defaulting to false. `WrangleNode` parses in the hook, syncs spares, stores the
   declarations, fills their values by slot in `pointKernel`, and rethrows validation errors there. Library tests: save/load, undo/redo
   (`sameMeta` compares spare lists, so a differing list rebuilds the node), copy/paste, fused chains, keyframes surviving a code edit,
   removal when a declaration is deleted.
3. **Editor**: `ParamInfo` gains `hasRange, lo, hi`; `ParameterPanel::addRow` adds a `newui::Slider` (named `slider.<name>`) for ranged
   rows, writing through `EditorModel::setParameter`; disabled while the parameter is an expression or keyframed; `sync` keeps it in
   step with the text. Editor model tests.
4. **GUI test and docs**: extend `editor/tests/gui/test_editor_gui.py` (type code, row appears, drag slider, keyframe); this sends real
   input, so ask before running. `docs/wrangle.md` new "Declared parameters" section (also update the "no declarations" and
   "Parameters a, b, c, d" text and the grammar), `editor/README.md`, an example graph under `examples/graphs/`.

## Build and test

See CLAUDE.md. Library: `cmake --preset x64-release && cmake --build --preset x64-release && ctest --preset x64-release`; editor presets
`x64-release-editor` / `x64-debug-editor`; the editor model tests and `jitnodesEditor --check` are in the editor build.
