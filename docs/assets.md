# Assets: reusable nodes

An **asset** is a subnet saved as a node type of its own. You build a little graph once, decide which of its parameters
the outside world may change, and from then on use it like any built-in node: it appears in the palette, takes inputs,
has parameters, and can be used any number of times. Each use is an independent **instance** with its own parameter
values and its own cache.

Use an asset when the same group of nodes appears in several places or several graphs, or when you want to hand someone
a node whose insides they do not need to understand. For a one-off grouping, a plain subnet is enough (see
[Subnets and assets](architecture.md#subnets-and-assets) in the architecture guide for how both are cooked).

Contents: [a first asset](#a-first-asset-in-the-editor) · [what is in an asset](#what-an-asset-is-made-of) ·
[writing assets](#writing-assets) · [using assets in a graph](#using-assets-in-a-graph) ·
[using assets in the editor](#using-assets-in-the-editor) · [changing an asset later](#changing-an-asset-later) ·
[rules and pitfalls](#rules-and-pitfalls)

## A first asset in the editor

1. Add a `subnet` node and double-click it to go inside (see [editor.md](editor.md#subnets-and-assets)).
2. Add a `subnetInput` node (its `index` is the input it reads, 0 for the first) and build the processing after it. The
   last node you add is the output. For example a `wrangle` with `x = x * a + b;`.
3. Go back up (**Up** or Backspace) and select the subnet. Use **Add parameter** to promote the parameters you want to
   expose, say `scale` and `offset`.
4. Inside, drive the wrangle's `a` and `b` with the expressions `parent.scale` and `parent.offset`.
5. Select the subnet and choose **Create asset...**. Pick a file name such as `scaler.asset.json`: the part before the
   first dot is the node type name. The subnet is replaced by an instance of the new `scaler` node with the same name,
   wiring and values, and `scaler` appears in the **asset library** of the palette.

From now on **Add asset** creates more `scaler` nodes, anywhere, including in other graphs that load the file.

## What an asset is made of

A definition is a JSON5 file, conventionally named `<type>.asset.json` (`examples/assets/wobble.asset.json`):

```json5
{
  format: "jitnodes-asset",
  version: 1,              // file format version, always 1
  assetVersion: 1,         // the asset's own version, see "Changing an asset later"; 1 when omitted
  type: "wobbleAsset",     // the node type name
  inputs: 1,               // number of inputs, 0 to 16
  promote: { amount: 0.3, speed: 3 },   // promoted parameters and their defaults
  assets: [ ... ],         // definitions of other assets used inside (written for you, see below)
  graph: {                 // the contents: an ordinary graph body
    fps: 24,
    display: 2,            // id of the node whose result is the asset's output
    nodes: [
      { id: 1, type: "subnetInput", name: "in", floats: { index: 0 }, inputs: [] },
      {
        id: 2, type: "wrangle", name: "wobble",
        strings: { code: "x = x + sin(y * 2 + a) * b; y = y + cos(x * 2 + a) * b;" },
        expressions: { a: "time * parent.speed", b: "parent.amount" },
        inputs: [1],
      },
    ],
  },
}
```

- **Inputs.** Input *n* of an instance is what the `subnetInput` node with `index: n` delivers. A `subnetInput` that
  is not wired from outside makes cooking fail with "subnet input n is not connected".
- **Output.** The result of the child's `display` node.
- **Promoted parameters** (`promote`) become float parameters of every instance, with these defaults. Inside, they are
  read as `parent.<name>` in *parameter expressions* (not in wrangle code, see the pitfalls).
- **Time.** Inside an asset, `time`, `frame` and `pi` work as everywhere; the child follows the graph's time and fps.
- **Nested assets.** An asset may use other assets. Their definitions are embedded in `assets`, so the file stands alone.
- **Instances do not store their contents.** A graph file stores only the instance (type, name, parameter values,
  wiring, the version it was built from), and the asset's definition once.

## Writing assets

**In the editor** (described above) is the usual way: build it, promote, **Create asset...**. It chooses the version
(one more than the registered one when you re-create an existing asset) and registers the result immediately.

**By hand.** Copy `examples/assets/wobble.asset.json` and edit it. The node format inside `graph` is the same as in a
graph file (see [Persistence, undo and clips](architecture.md#persistence-undo-and-clips)): `id`, `type`, `name`,
`floats`, `strings`, `expressions`, `keyframes`, `inputs` (ids, 0 = unconnected). Comments and trailing commas are
allowed. Loading validates by building one instance, so a mistake is reported with the node's name when the asset is
registered.

**From C++.** This is the whole cycle: build the contents in a subnet, save, register, use, change, update. (The program
below was compiled and run; it prints the lines in the comments.)

```cpp
#include "jitnodes/jitnodes.h"
using namespace jitnodes;

// 1. Build the contents inside a subnet.
Network authoring;
NodeId sub = authoring.addNode("subnet");                       // "subnet" has one input; subnet0, subnet2..4 exist too
Network& inner = static_cast<SubnetNode&>(authoring.node(sub)).network();
NodeId in = inner.addNode("subnetInput");                        // index 0: the geometry wired to the subnet
NodeId scale = inner.addNode("wrangle");
inner.node(scale).setString("code", "x = x * a + b;");
inner.node(scale).setExpression("a", "parent.scale");            // promoted parameters are read in expressions
inner.node(scale).setExpression("b", "parent.offset");
inner.connect(in, scale);
inner.setDisplayNode(scale);                                     // the child's display node is the output
authoring.node(sub).addSpareFloat("scale", 2.0f);                // promote, with a default
authoring.node(sub).addSpareFloat("offset", 0.0f);

// 2. Save the definition (type name, version) and register it as a node type.
registerAsset(saveAsset(authoring, sub, "scaler", 1));           // or saveAssetToFile(...) / registerAssetFromFile(...)

// 3. Use it like any other node type.
Network net;
NodeId line = net.addNode("line");
net.node(line).setFloat("count", 5.0f);
net.node(line).setFloat("length", 4.0f);
NodeId a = net.addNode("scaler");
net.node(a).setFloat("scale", 3.0f);
net.connect(line, a);
net.setDisplayNode(a);
GeometryHandle geo = net.cookDisplay();                          // x = 0 3 6 9 12

// 4. Change the definition (version 2 adds a promoted parameter) and update the existing instances.
authoring.node(sub).addSpareFloat("mix", 1.0f);
registerAsset(saveAsset(authoring, sub, "scaler", 2));
outdatedAssetInstances(net);                                     // 1
updateAssetInstances(net);                                       // 1 rebuilt; scale is still 3
net.node(a).setFloat("offset", 0.5f);                            // x = 0.5 3.5 6.5 9.5 12.5

// 5. A migration hook for a renamed parameter (see below).
NodeRegistry::instance().setAssetMigration("scaler", [](AssetUpgrade& up) {
    if (up.fromVersion < 3 && up.old.hasFloat("scale")) up.fresh.setFloat("gain", up.old.baseFloat("scale"));
});

// 6. A saved graph embeds the definition, and an instance can be turned back into an editable subnet.
std::string text = saveNetwork(net);                             // contains "jitnodes-asset"
unlockAssetInstance(net, a);                                     // now an ordinary "subnet"
```

`registerAsset` rejects a type name that belongs to a built-in node type, and an empty name.
Asset type names must start with a letter or underscore and contain only letters, digits and underscores (the editor
enforces this; hand-written names should follow it so that they stay usable in graph files and menus).

## Using assets in a graph

- **As a node.** After registration `net.addNode("scaler")` works like any type; so does a graph file with
  `type: "scaler"`. An instance has the declared inputs, the promoted parameters (editable, animatable with
  expressions and keyframes like any float) and nothing else: its insides cannot be reached from outside.
- **In a graph file.** `saveNetwork` embeds the definition of every asset the graph uses (transitively) in an
  `assets` list, so the file is self-contained and opens on a machine that does not have the asset. When loading, an
  embedded definition is registered if that type is not registered yet, or if the file's `assetVersion` is higher than
  the registered one; otherwise the registered definition wins. Registrations are global to the process.
- **From the command line.** `jitnodesCli --asset examples/assets/wobble.asset.json examples/graphs/assetDemo.json --time 1 --svg demo.svg`
  registers the library files first (the option can be repeated), then loads and cooks the graph.
- **In your own program.** Call `registerAssetFromFile` for each library file at start-up, before loading graphs; list
  what is available with `NodeRegistry::instance().assetNames()`.
- **Performance.** All instances of an asset share compiled kernels (identical kernels compile once), and an edit of a
  promoted parameter only recooks the child nodes whose expressions mention `parent`.

## Using assets in the editor

The **Assets** section of the palette is the *asset library*. Selecting an entry shows its version, number of inputs,
promoted parameters, how many instances the current graph has, and the file it came from.

| Action | Effect |
|---|---|
| **Add asset** | creates an instance and wires the selected node into its first input, like the node buttons |
| **Load asset...** | registers a definition file you pick; instances built from an older definition are updated |
| **Reload asset** | reads the selected asset's source file again, so edits made outside the editor are picked up |
| **Create asset...** | saves the selected subnet as a definition and replaces the subnet by an instance |
| **Unlock** | turns the selected instance back into an ordinary subnet with a copy of the contents, so they can be edited |

- **Where the editor looks.** At start-up it registers every `*.asset.json` file in the `Assets/` folder next to
  `jitnodesEditor.exe`. That folder is filled from `examples/assets/` when the editor is built, so put library assets
  there (and rebuild), or copy files into `Assets/` directly, or use **Load asset...** for a file anywhere.
- **Editing an asset.** Instances cannot be entered. Either **Unlock** an instance, change its contents, and
  **Create asset...** again with the same name (the file is overwritten with version + 1, and the remaining instances
  in the open graph are rebuilt), or edit the `.asset.json` by hand and **Reload asset**. Rebuilt instances keep every
  parameter value that still exists.
- **Undo.** Updating instances changes node types, so the undo history is cleared and the status line says so.
- **Opening graphs.** Graph files carry the asset definitions they use, so they open in an editor that has never seen
  the asset; the asset then shows up in the library.

## Changing an asset later

Instances are never changed behind your back: re-registering a definition affects *new* instances; existing ones keep
the definition they were built from until you update them (**Reload asset** / **Load asset...** in the editor, which do
this for you, or `updateAssetInstances(net)` in code).

- **Updating** rebuilds each outdated instance from the current definition with the same id, name, position and
  wiring (wiring that still fits the new input count is kept). Every parameter that exists under the same name keeps its
  value, expression or keyframes; parameters that are gone are dropped; new ones start at their defaults.
- **Versions.** Give a changed definition a higher `assetVersion`. Instances remember the version they were built from
  (saved as `assetVersion` on the node). The editor picks "registered version + 1" for you.
- **Renamed or rescaled parameters** need a migration hook, because matching is by name:
  `NodeRegistry::setAssetMigration(type, fn)`. The hook gets an `AssetUpgrade` with `fromVersion`, `toVersion`, the `old`
  node (saved parameters only), the `fresh` node (already holding every same-named parameter) and the `inputs` wiring
  (reorderable). It runs once per instance, when its version differs: in `updateAssetInstances` and when a graph file
  saved with an older version is loaded. Switch on `fromVersion` to chain steps 1 to 2 to 3. It must be a pure function
  of its argument (loading runs it twice, once to validate). Without a hook, a load of an older instance is lenient:
  unknown parameters are dropped and a changed input count is padded or trimmed.
- **Unlocking** (`unlockAssetInstance`, **Unlock**) detaches one instance for good: it becomes a normal subnet saved
  with the graph, and later changes to the asset no longer reach it.

## Rules and pitfalls

- **Promoted parameters are read in expressions, not in wrangle code.** `x = x * parent.scale;` does not work: inside
  code `parent.scale` is just an attribute name (and reads 0). Give the wrangle a parameter (`a`) and set its expression
  to `parent.scale`, then use `a` in the code.
- **Only float parameters can be promoted.** No strings or ints, and no per-instance overrides of nodes inside.
- **`parent` is a reserved name** inside subnets and assets; `parent.x` itself can be an expression that reads the next
  level's `parent`.
- **Every `subnetInput` index you read needs a wired input**, and the asset's `inputs` count decides how many an
  instance has. **Unlock** supports up to four inputs (the subnet types go up to `subnet4`).
- **A type name cannot clash with a built-in node.** Re-using the name of an existing *asset* replaces it for new
  instances.
- **Registrations are process-global** and not undone if a later load fails. Tests and tools that need a clean state
  should use unique asset names.
- **Nesting is limited** to 64 levels of subnets and assets, and a promoted-parameter chain to 400 references.
- **Not done:** per-instance overrides of child nodes, string and int promoted parameters, and `parent.` references to
  string parameters.

Related: [architecture.md](architecture.md#subnets-and-assets) (how subnets cook), [editor.md](editor.md#subnets-and-assets)
(the editor's asset tools), [design.md](design.md#subnets-and-assets) (design notes and file details).
