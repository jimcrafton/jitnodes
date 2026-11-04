# Image nodes and tile providers

Besides geometry, a graph can carry **images**. An image node does not compute pixels when it cooks. It produces a
*recipe* (`ImageData`: a description plus the node's captured parameters and its inputs). A `TileProvider` turns a
recipe into pixels, one square tile at a time, and caches the tiles. This keeps cooking cheap, lets a host evaluate only
the tiles it needs (the visible ones, the ones an edit changed) and lets a host run the work on a GPU.

This is the base layer. The library ships no image nodes yet; the test suite defines a few (solid colour, gain, shift)
and a CPU reference provider. Typed ports ([dataAndPorts.md](dataAndPorts.md)) keep image and geometry wires apart.

## If you use the node graph

- Image and geometry wires are different types and cannot be mixed.
- Pixels are 1 to 4 float channels, **unclamped**: values below 0 and above 1 are kept all the way through. Four
  channels are premultiplied RGBA.
- An image has *bounds* and a *fill* value. Everything outside the bounds, and every tile nobody has written, is the
  fill. Bounds may sit at negative coordinates or hang outside any "canvas".
- Editing a parameter recomputes only the tiles that depend on it. Tiles upstream of the edit are reused.
- **Bypass** an image node and its input passes through. **Freeze** is not available for images (it would have to store
  the pixels in the graph file).

## If you write code

### Cooking and evaluating

```cpp
Network net;
NodeId solid = net.addNode("testSolid");
NodeId gain  = net.addNode("testGain");
net.connect(solid, gain);

ImageHandle image = asImage(net.cookData(gain));   // a recipe; no pixels yet
CpuTileProvider provider(256);                      // tile edge in pixels
std::vector<float> px(w * h * image->desc().channels);
provider.readRect(*image, {0, 0, w, h}, px.data()); // evaluates the needed tiles
```

`Network::cook(id)` still returns `GeometryHandle` (null for an image node); `Network::cookData(id)` returns any kind.
`Node::cookData` is what the network calls; its default adapts the old `Node::cook`, so geometry nodes are unchanged.

### Writing an image node

Derive from `ImageNode`, declare parameters as usual and implement two things:

```cpp
class GainNode : public ImageNode {
public:
    GainNode() : ImageNode("gain", 1) { declareFloat("gain", 1.0f); }
protected:
    ImageDesc outputDesc(const std::vector<ImageDesc>& in) const override { return in[0]; }   // blur: grow the bounds
    std::shared_ptr<const ImageEvaluator> makeEvaluator() const override;                    // capture the parameters
};
```

The `ImageEvaluator` is immutable and must not refer back to the node, so tiles can be evaluated later or on another
thread while the node is edited. It answers two questions:

- `inputRect(input, outRect)`: the pixels of that input needed to compute `outRect`. A pointwise node returns `outRect`; a
  blur returns it grown by the radius. The provider uses this both to fetch input tiles and to decide which output tiles
  an upstream change invalidates.
- `evaluateCpu(CpuTileContext&)`: the reference implementation. Write the pixels of `ctx.outRect()` with `ctx.outputAt(x, y)`
  (the tile is pre-filled with the fill value) and read inputs with `ctx.readInput(i, rect, out)`.

`ImageNode` builds the recipe: it hashes the type name, the resolved parameters (`Node::paramsHash()`), `extraHash()` and
the inputs' hashes. Override `extraHash()` if something other than parameters and inputs affects the pixels.
Image nodes are never fused and never absorbed into a merge.

### Pixels that do not come from a node

Implement `ExternalImage` (`desc`, `tileVersion`, `readTile`) and wrap it with `ImageData::fromExternal`; a source node
returns that from `cookData`. `tileVersion(coord, tileSize)` must change whenever that tile's pixels change: it is how
edits to the source reach the tile cache.

### Providers

`TileProvider` is the backend interface: `acquire` (non-blocking, ready / pending / failed), `tileVersion`, `pump`, `cancel`,
`readRect`. `CpuTileProvider` is the reference implementation: synchronous, float32 storage, FIFO-evicted cache,
`evaluations()` counts tiles actually computed. A GPU host implements `TileProvider` itself and runs its own kernels for
the node types it knows; it can read the node type and the evaluator from `ImageOp`. Providers are used from one thread.

### Cache correctness

A tile's cache key is a 64-bit hash of its image description, the tile size and coordinate, the recipe hash and the
versions of every input tile it reads (through `inputRect`), computed by `tileVersionOf()` (also what a GPU provider should
use). Hashes are combined with `hashCombine()` (xxh64 of the pair): an earlier shift/xor combiner produced real key
collisions between distinct tiles, so write new hash code with `hashCombine`, not ad-hoc mixing. A 64-bit collision would
still return wrong pixels; it is not checked.

### Tests

`tests/testMain.cpp`: `imageNodesCookToRecipesNotPixels`, `cpuProviderEvaluatesTilesWithNegativeCoordinatesAndHdrValues`,
`cpuProviderCachesTilesAndEditsOnlyRecomputeDownstream`, `externalTileVersionsInvalidateOnlyTilesThatReadThem`,
`bypassedImageNodePassesItsInputAndImagesCannotBeFrozen`, `imageEvaluatorFailureIsReportedPerTile` and
`imageNodesAreNeverFusedWithGeometryNodes`.

## Not done

Basic image nodes, geometry-to-image nodes (a node with a geometry input and an image output must override `inputType`
and `cookData` itself), 8-bit/half storage in the CPU provider, a thread pool for the CPU provider, serialization of
images, and per-node host kernel registration (a host-side concern).
