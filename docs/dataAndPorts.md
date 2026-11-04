# Data kinds and typed ports

A node's result used to be implicitly a `Geometry`. Results are now a `Data` of some *kind*, and every wire connects an
output of one kind to an input of the same kind. Today the only kind that exists as a class is geometry; `image` is
reserved so that image nodes can be added without changing the wiring rules again (the image cook path is planned, it
is not in the library yet).

Nothing about existing graphs, files or node types changes.

## If you use the node graph

- **Wires are checked by type.** Connecting an output to an input of a different kind is refused with a message naming
  both nodes and both kinds, for example
  `type mismatch: myLine outputs geometry but input 0 of myFilter takes image`.
  In the editor the wire is not made and the message appears on the status line, the same way a cycle is refused.
  All built-in nodes take and produce geometry, so you will not hit this until nodes of another kind exist.
- **Bypass needs matching ends.** Bypass passes a node's first input straight through, which only makes sense when the
  input and the output are the same kind. Bypassing a node whose first input and output kinds differ is refused with a
  message.
- **Saved files are unchanged.** Port types come from the node's type, not from the file, so old files load as before and
  files written now are still readable by older builds (until a node of another kind appears in a graph).
- **A file that wires incompatible nodes fails to load**, with an error naming the node and input
  (`node 'flt' input 0: type mismatch: ...`). This can only happen if a file was edited by hand or a node type changed its
  ports since the file was saved.
- **Replacing a node** (asset updates, `replaceNode`) is refused, leaving the graph as it was, if the new node type would
  break an existing wire.

## If you write code against the library

### What changed in the API

| Before | Now |
|---|---|
| `class Geometry` | `class Geometry : public Data`. Still copyable, still held as `GeometryHandle` (`shared_ptr<const Geometry>`). It now has a vtable pointer, so it is not trivially copyable (it never was: it owns containers). |
| (none) | `jitnodes/core/data.h`: `enum class DataKind { geometry, image }`, `class Data` (immutable base with `virtual DataKind kind() const`), `DataHandle = shared_ptr<const Data>`, `dataKindName(kind)`. |
| (none) | `asGeometry(const DataHandle&)` in `geometry.h`: the typed handle, or null for an empty handle or another kind. |
| `Node::cook(CookContext&, const std::vector<GeometryHandle>&) -> GeometryHandle` | **Unchanged.** Node implementations do not need editing. |
| (none) | `virtual DataKind Node::inputType(int index) const` and `virtual DataKind Node::outputType() const`, both defaulting to `geometry`. |
| `Network::connect` threw on a bad index or a cycle | Also throws `std::runtime_error` on a kind mismatch (checked before the cycle test). |
| `Network::replaceNode` | Checks, before changing anything, that the new node accepts its inherited inputs and still feeds its consumers; throws and leaves the graph untouched otherwise. |
| `Node::setBypassed(true)` always worked | Throws `std::runtime_error` when `inputCount() > 0` and `inputType(0) != outputType()`. |

Code that only handles geometry needs no change. Code that stores or passes results generically should hold a
`DataHandle` and use `kind()` / `asGeometry()` rather than assuming geometry.

### Declaring ports on a node type

Override the two virtuals; the defaults are geometry in, geometry out. A node whose first input is an image and whose
output is a geometry:

```cpp
class ImageToPointsNode : public Node {
public:
    ImageToPointsNode() : Node("imageToPoints", 1) {}
    DataKind inputType(int) const override { return DataKind::image; }
    // outputType() stays geometry
    GeometryHandle cook(CookContext&, const std::vector<GeometryHandle>&) override;
};
```

Rules to keep in mind:

- Port types belong to the **node type**. Do not return different kinds from the same node type depending on
  parameters: the types are read when wiring, loading and replacing, and they are not saved.
- For `hasVariableInputs()` nodes (`merge`) `inputType(i)` is asked for every slot, including the free one.
- A node of a non-geometry kind overrides `Node::cookData` (the widened cook that returns any `DataHandle`) instead of `cook`;
  `ImageNode` does this for you. See [images.md](images.md).
- A type mismatch from `connect()` is a `std::runtime_error` like the cycle and range errors, so callers that already
  catch those (the editor, `loadNetwork`) need no change.

### Tests

`connectRejectsPortTypeMismatches`, `portTypeMismatchMessageNamesTheNodes`,
`loadingAGraphWithMismatchedPortsFailsWithTheNodeName`, `bypassNeedsMatchingInputAndOutputTypes` and
`replacingANodeKeepsPortTypesConsistent` in `tests/testMain.cpp` use three stand-in node types (image source, image
filter, image-to-geometry) that are registered only by those tests.

## What is not done yet

The image payload and tile cook path now exist; see [images.md](images.md). No built-in node uses `DataKind::image` yet.
