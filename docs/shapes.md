# Shaping curves and layouts

Ten small nodes for working on polylines and point layouts. The nodes that follow a curve's shape (`smooth`, `offset`, `join`, `bevel`,
and `peak` in normal mode) flatten cubic curves first, with the `tolerance` parameter; `sort`, `polar`, `twist`, `lattice` and `switch` leave
them alone (the point warps move control points like any other point). Sources: `src/graph/shapeNodes.cpp`, and `switch` in
`src/graph/builtinNodes.cpp`.

## smooth (one input)

Laplacian smoothing: each interior point moves towards the midpoint of its two neighbours by `strength`, `iterations` times. The ends of an
open curve stay put, so a smoothed path still starts and ends where it did.

| Parameter | Default | Meaning |
| --- | --- | --- |
| `iterations` | 5 | passes, 0 to 10000 |
| `strength` | 0.5 | 0 to 1 per pass |
| `preserve` | 1 | alternate each pass with a slightly stronger pass in the opposite direction (Taubin), which smooths without shrinking the curve; 0 gives plain Laplacian smoothing, which shrinks closed curves |
| `tolerance` | 0.1 | flattening of cubic curves |

For Chaikin corner cutting (which adds points) use `subdivide`.

## offset (one input)

Parallel copies of each curve, pushed to the left of the direction of travel (negative distances go right): copy k is `k * distance`
away. Corners are mitred, up to `miterLimit` times the distance. Inside corners are not trimmed, so a large offset of a tight curve
crosses itself. Output primitives have the int attribute `offsetIndex` (the k), and every point keeps the attributes of its source point.

| Parameter | Default | Meaning |
| --- | --- | --- |
| `distance` | 0.1 | step between copies |
| `count` | 1 | number of copies |
| `keepOriginal` | 0 | also output the curve itself (`offsetIndex` 0) |
| `miterLimit` | 2 | longest mitre, in distances (at least 1) |

`ribbon` is the stroke: it offsets both sides and joins them into an outline (see [fx.md](fx.md)).

## sort (one input)

Reorders primitives (with their points and attributes) or, for geometry without primitives, points.

| Parameter | Default | Meaning |
| --- | --- | --- |
| `class` | `primitive` | `primitive` or `point`; points that belong to primitives cannot be reordered |
| `key` | `x` | `x`, `y` (a primitive's centroid), `distance` and `angle` about (`centerX`, `centerY`), `length` and `size` (primitives only), `random` (`seed`), `index`, `attrib` |
| `attrib` | | for `key = attrib`: a primitive attribute, else the mean of the point attribute over the primitive. String attributes sort by their text |
| `reverse` | 0 | flip the order |
| `shift` | 0 | rotate the order by this many places (a new start for a closed ordering) |

Ties keep their input order. Points that no primitive uses follow the sorted primitives.

## join (one input)

`mode = ends`: open curves whose end points lie within `distance` of each other become one curve. A curve is reversed when that makes the
ends meet, every end joins at most one other (the nearest pairs first), and a chain that returns to its start becomes closed. The
joined curve keeps the primitive attributes of its first curve; the second point of each join is dropped. Closed curves and single points
pass through unchanged.

`mode = all`: every curve in order becomes one curve, closed when `close` is 1; where a curve starts within `distance` of the previous
curve's end, the duplicate point is dropped.

## bevel (one input)

Cuts each corner of a polyline: the vertex is replaced by a point `distance` back along the incoming edge and one `distance` forward along
the outgoing edge (a chamfer, `segments = 1`), or by `segments + 1` points on the quadratic curve between them (a round). The cut is
limited to half of either edge, corners turning by less than `minAngle` degrees are left alone, and the ends of an open curve stay.
The cut points take the attributes of the edge they lie on; the points of a round take the corner's.

## polar (one input)

Bends a flat layout around a centre. `toPolar`: `x` is the angle (`width` units of x make `sweep` degrees, starting at `startAngle`) and
`y` is the radius, so a grid becomes rings and spokes. `fromPolar` is the inverse and unrolls rings into a grid. Only points move, so
edges stay straight: `resample` first when long edges should curve. `amount` (0 to 1) blends from the input, which animates the bend.

## twist (one input)

Rotates points about (`centerX`, `centerY`) by an angle that depends on their distance r from it. With `radius` above 0 the swirl is
strongest at the centre and fades to nothing at the radius: `angle * (1 - r / radius) ^ power` degrees. With `radius` 0 the angle grows
with distance, `angle` degrees per unit. `amount` scales the whole effect (animate it to wind a shape up).

## peak (one input)

Pushes each point along its normal by `amount` (multiplied by the point attribute named in `scaleAttrib`, if any). The normal is the left
normal of the curve, the same side `offset` uses: the `nx` / `ny` point attributes when the input has them (`curveSample` writes them),
otherwise the average over the primitives that use the point (cubic curves are flattened first). `mode = radial` pushes away from
(`centerX`, `centerY`) instead and works on bare points. A point without a normal stays where it is.

## lattice (two inputs)

Free-form deformation. Input 0 is the geometry; input 1 holds the control points of a `columns` x `rows` lattice (2 to 16 each) in
their new places, row by row from the bottom left with x running fastest. The rest lattice is an even grid over the box
`minX..maxX`, `minY..maxY` (all four 0 uses the bounds of input 0). Each point moves by the Bezier blend of the control point
displacements at its place in the box, so the deformation is smooth, and a point outside the box gets the displacement of the nearest
edge. Control points left on the even grid change nothing; `amount` blends from the input. A `generate` grid or a hand-drawn point set,
moved by `noise`, `twist` or a `wrangle`, makes a good cage.

## switch (variable inputs)

Passes input number `index` through (clamped to the connected inputs, counting from 0). Only the selected input is cooked, so a branch
that is switched off costs nothing, and `index` may be animated (`time`), which cuts between alternatives. It is the one node that
needs `Node::selectsInput()` / `selectedInput()`, asked while the cook is planned.
