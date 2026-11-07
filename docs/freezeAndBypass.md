# Freeze and bypass

Two flags that every node has, whatever its type. They are set from outside (a button in an editor, a call in code), not by the node, and they are saved with
the node, so they survive save and load, undo and redo, copy and paste.

| | Bypass | Freeze |
|---|---|---|
| Question it answers | "What if this node weren't here?" | "I am done with everything up to here." |
| Result of the node | its first input, unchanged | the geometry stored when it was frozen |
| Upstream is cooked | yes (the first input) | no |
| The node's own work | skipped | skipped |
| Stored in the file | one number | the whole geometry |

Sources: `include/jitnodes/graph/node.h`, `network.h`, `src/graph/network.cpp` (planning and running), `src/graph/serialize.cpp` (saving), tests in
`tests/testMain.cpp` (`bypassed...`, `frozen...`, `bypassAndFreezeSurvive...`).

## Bypass

```cpp
net.node(id).setBypassed(true);    // the node is skipped
net.node(id).setBypassed(false);   // it works again
bool skipped = net.node(id).bypassed();
```

A bypassed node returns its first input as it is; the rest of the graph cooks as usual. With no first input connected it returns an empty geometry instead of
raising "input 0 is not connected", so a bypassed node can sit unwired. Other inputs of a multi-input node are ignored while it is bypassed.

Use it to turn an effect off without losing its parameters or its wiring: a `wave`, a `blast`, a `transform`. Turning it back on restores exactly what was there.

Rules:

* A bypassed node is never fused into a point-wise kernel chain and never absorbed into a merge. The nodes around it still fuse with each other.
* Bypass wins over freeze: a node with both set behaves as bypassed, and `Network::freezeNode` refuses a bypassed node.
* Switching it, like any edit, dirties the node and what follows it.

## Freeze

```cpp
net.freezeNode(id);                        // cooks the node (and what is upstream of it) once and keeps the result
net.node(id).frozen();                     // true
net.node(id).frozenGeometry();             // the stored GeometryHandle
net.node(id).setFrozenGeometry(nullptr);   // unfreeze: the node cooks normally again
net.node(id).setFrozenGeometry(geometry);  // freeze on geometry you made or edited yourself
```

While a node is frozen:

* its result is the stored geometry, and the planner gives it no inputs, so **nothing upstream is cooked for it**, and its inputs need not even be connected;
* an edit upstream stops at the frozen node: the node and everything after it keep their caches, and `dirty()` stays false for them;
* it does not depend on time: a frozen node whose parameters are keyed, or a frozen `particles` or subnet, does not change when the playhead moves, and its
  child network (for a subnet) is not cooked;
* it is not fused into a kernel chain and not absorbed into a merge, so it is a clean cache boundary;
* its own parameters still exist, can be edited and are saved, but they do not change its output. Editing one recooks what follows (from the stored geometry,
  which is cheap). Expressions on other nodes that read its parameters still work.

`freezeNode` throws for a bypassed node, and a node that is cooked as part of a larger task is simply cooked first, so freezing a node in the middle of a fused
chain works.

Use it to:

* stop paying for an expensive stage you have finished with (a simulation, a long boolean chain, a `reactionDiffusion` result), while keeping it in the graph;
* keep a result stable while you edit something upstream you do not want to propagate;
* make hand-authored data: freeze, then change the geometry with `setFrozenGeometry`. (A host such as curvy uses this for a hand-edited shape.)

To go back, unfreeze. The upstream nodes were never lost; they cook again the next time something asks for them.

### What is saved

A frozen node writes its geometry inline in the graph file, under `"frozen"`, in the same form as a geometry parameter (point count, primitives, point and
primitive attributes, details). Floats are written as the shortest decimal that reads back to the same float, so a save and load gives bit-identical values.
A bypassed node writes `"bypass": 1`. A key that is absent means off: restoring an older snapshot over a node that has since been frozen unfreezes it.

### File size

Frozen geometry is the one thing in a graph file that grows with the data, and the text form is not compact. Measured on 2026-11-02 with a `line` of N points
through a `wrangle` (3 float point attributes), file size with the node frozen minus without:

| Points | Added to the file | Per point |
|---:|---:|---:|
| 1,000 | 76 KB | 75.9 bytes |
| 10,000 | 755 KB | 75.5 bytes |
| 100,000 | 7.5 MB | 75.5 bytes |

Roughly 25 bytes per float value, so the cost is points x attributes x 25 bytes, plus the primitives. The graph itself without the geometry was about 600 bytes.
Houdini has the same property, and people avoid it by dumping the data to disk instead of leaving it in the scene.

It is also in memory twice while a graph is being saved or snapshotted (the node's geometry and the text), and undo snapshots of a graph with a large frozen
node are as large as the node's text. Undo reuses the text of nodes that did not change (`snapshotNetwork` with a previous snapshot), so an unrelated edit
does not copy it again, but the first snapshot after a freeze does.

Guidance:

* Freeze what is small or precious: a hand-edited outline, a few thousand points. That is a fraction of a megabyte.
* For a large result use `fileCache` ([geometryFiles.md](geometryFiles.md)): in `read` mode it loads a `.jngeo` file and never cooks its input, which is the
  same saving as freeze, and the file is binary (floats copied bit for bit, about 4 bytes per value, so roughly 6 times smaller than the text above; this ratio is
  from the format, not measured) and lives beside the graph rather than inside it. The cost is that the file is a separate thing to keep with the graph.
* Unfreeze, or do not freeze, nodes that produce data you can recompute cheaply.

Not built: writing a frozen node's geometry out to a `.jngeo` file and reading it back from there, so that one flag covers both cases.

## How it works

`Network::buildTask` decides what a node's cook needs. For a bypassed node it plans one input (the first, if connected) and returns; for a frozen node it plans
none. The checks come before the fusion and merge branches, and the places that fold an upstream node into a task (`fusable` in the point-wise chain loop and
`absorbable` in `collectMergeInputs`) skip bypassed and frozen nodes. `runTask` then returns the input or the stored geometry without calling `cook`.
`markDownstream` does not mark a frozen node (other than the one edited) or walk past it, `setTime` ignores frozen nodes when it collects the time-dependent
ones, and `dependsOnTime` is false for them. `Node::setBypassed` and `Node::setFrozenGeometry` are ordinary edits (`markChanged`), so snapshots, history and the
revision counter see them.

## Limits and notes

* `Network::executionOrder` still lists the nodes upstream of a frozen node, though they are not cooked. It is for display and tooling, not for planning.
* The stats and profile report count a bypassed or frozen node as a cook that did no work.
* A frozen subnet or asset instance is frozen as a whole; its child graph is not cooked while it is, and it is not saved twice (the child graph is still in the
  file, plus the frozen result).
* Freezing does not record what produced the geometry, so a frozen node does not notice that its upstream changed. Unfreeze to bring it up to date, or freeze again
  after the edit (`freezeNode` on a node that is already frozen returns its stored geometry; unfreeze first to recook).
