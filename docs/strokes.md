# Stroke nodes

Nodes for geometry that is a hand-drawn stroke: one open polyline whose points carry the pen or mouse data (`t` seconds since the stroke
started, `pressure`, `tiltX`, ...). Curvy feeds strokes through these inside its input assets (docs/input-generator-plan.md in curvy); they work on
any polyline with the attributes they name.

## oscillator (one input)

Adds an attribute that follows a wave of a driver: the playhead, or a float point attribute.
`value = offset + amplitude * wave(frequency * driver * driverScale + phase)`, where the wave runs from -1 to 1 once per unit of its argument.

| Parameter | Default | Meaning |
|---|---|---|
| `name` | `osc` | the attribute (or detail value) written |
| `class` | `point` | `point`: one value per point; `detail`: one value for the geometry (the driver must be `time`) |
| `waveform` | `sine` | `sine`, `saw` (rises from -1), `square`, `triangle` (starts at 0, rising), `noise` (smooth value noise, one random value per whole cycle, blended with a smoothstep), `ramp` (the curve in the `ramp` parameter over one cycle, see [ramps.md](ramps.md)) |
| `driver` | `time` | `time` (the network time, so the node recooks when it changes) or the name of a float point attribute such as `t` or `s` |
| `frequency` | 1 | cycles per unit of the driver (per second for `time` or `t`) |
| `amplitude`, `offset` | 1, 0 | scale and shift of the wave |
| `phase` | 0 | start of the wave in cycles |
| `pulseWidth` | 0.5 | `square`: the fraction of a cycle that is high |
| `driverScale` | 1 | multiplies the driver before the frequency |
| `seed` | 1 | `noise`: the random sequence |

A driver attribute the points do not have is an error that names it.

## pathMotion (one input)

Recomputes how a stroke moves from its positions and the sample times: `s` (arc length from the start of each polyline), `dt`, the smoothed
velocity `vx`, `vy`, `speed`, `ax`, `ay`, `accel` (the signed rate of change of speed) and `heading` (radians, the direction of the smoothed
velocity). Use it after something moved the points (a `smooth`, a warp) so the attributes follow.

| Parameter | Default | Meaning |
|---|---|---|
| `timeAttr` | `t` | the point attribute with the sample time in seconds (an error when missing) |
| `smoothing` | 0.3 | weight of the newest velocity in an exponential moving average, above 0 and at most 1 (1: none) |

The first point takes the values of the second so a stroke does not start at rest; two samples with the same time repeat the values before them;
the first acceleration is 0. The maths is `PathMotion::compute` (`geo/pathMotion.h`), which an application can call directly.

## strokeClean (one input)

Trims a stroke. Removes point attributes, and drops points that carry little. A dropped point takes its attribute values with it; the points that
stay keep their own. `x`, `y` and the primitive attributes always stay, and so does the detail data.

| Parameter | Default | Meaning |
|---|---|---|
| `remove` | empty | names to remove, separated by spaces or commas; `*` matches any run of characters (`tilt* twist vx vy`) |
| `keep` | empty | when not empty, every point attribute not listed (patterns allowed) is removed |
| `simplify` | 0 | Ramer-Douglas-Peucker tolerance in geometry units: points closer than this to the line between their kept neighbours go (0: off) |
| `minDistance` | 0 | a point nearer than this to the last kept one goes (0: off) |
| `minTime` | 0 | a point less than this many seconds after the last kept one goes (0: off) |
| `timeAttr` | `t` | the time attribute `minTime` reads |
| `keepEnds` | 1 | the last point of a polyline stays (it replaces a kept point that was too close to it) |
| `follow` | `w` | float point attributes (names or patterns) that `simplify` must not flatten; names the points do not have are ignored |
| `followTolerance` | 0.05 | `simplify` keeps a point where a followed attribute differs by more than this from a straight change, by path length, between its kept neighbours (0: positions only). `minDistance` and `minTime` do not look at attributes |

The order is: `minDistance` and `minTime`, then `simplify`, then attribute removal. Polylines with fewer than three points and cubic primitives are
copied whole.

## calligraphy (one input)

A calligraphic pen. Every path (polyline or cubic; a cubic is sampled 12 times per segment) is swept by a slanted oval nib and comes out as one closed
cubic outline to fill: a few dozen cubics for a hand-drawn stroke, not a polygon per sample. A closed path gives two outlines, the outer and the inner
loop, wound opposite ways under the one shape key so a non-zero fill leaves the inside open. A path of a single point (a tap) gives the nib itself.
The outlines keep the shape key of their path. All point attributes are dropped.

How it works (Pudet 1993, "Real time fitting of hand-sketched pressure brushes"; see curvy `docs/brush-research.md`):

1. `TrajectoryFit` fits the path's samples to cubics within `trajectoryTolerance`, flattens the result to a polygon and carries the pressure by arc length.
   The node uses the forward mode of `CurveFit`: each cubic grows from the previous join as far as it fits and is then frozen, so the start of a stroke does
   not move while the stroke is still being drawn (the global mode re-split the whole run on every sample).
2. `BrushEnvelope` walks the nib along that polygon. On each segment the nib touches each border with one fixed corner; round the outside of a
   turn the border follows the nib's corners, across the inside it takes the crossing of the offset segments. A turn too sharp for the segments
   to cross, taken across the path (a hairpin), keeps both points and marks them as corners. The caps are the nib's own corners.
3. `OutlineFit` fits the border polygon, with the corners as cuts, to cubics within `outlineTolerance`. For an open stroke (`openStrokeCubic`) the seam sits in
   the middle of the start cap and the left and right borders are each fitted forward in stroke order, sharing one tangent at the seam and at the end-cap
   middle; this keeps the outline of the start stable too. Closed paths and single points use the cyclic fit (`closedCubic`).

| Parameter | Default | Meaning |
|---|---|---|
| `width` | 8 | the nib's long diameter (above 0) |
| `angle` | 45 | the nib's tilt in degrees, counter-clockwise from horizontal |
| `roundness` | 25 | the nib's thickness as a percentage of its length, above 0 up to 100 (100 is a round pen) |
| `scaleAttr` | `w` | the point attribute that scales the nib along the stroke (pen pressure); ignored when the points have none or it is 0 everywhere |
| `trajectoryTolerance` | 0 | how far (document units) the fitted path may stray from the samples; 0 is a quarter of `width`, at most 1 (a wide nib on a tight scribble was 9.9 units and 8 percent of the area off at `width`/4). A mouse path is a coarse polyline (about 9 units between samples, 8 degrees of turn each); following every corner of it costs a cubic or two per sample |
| `outlineTolerance` | 0 | how far the outline may stray from the swept border; 0 is 0.075 of `width` or 0.3 of the nib's thickness at the stroke's thinnest scale (taken as at least 0.3), whichever is less, and at least 0.05; following the thinnest pressure keeps the two borders of a light stretch from fitting into one |

The `tolerance` parameter of 2026-11-24 (skip samples on straight runs) is gone. Measured on curvy's `perf2.curvy` (23 strokes, 8 wide nib): 3,751
primitives and 67,518 points before, 23 primitives and 2,232 points now, the outline within 2.83 units of the exact union of per-step hulls, area 0.55 percent smaller.

The nib is `NibBrush::oval`, ported from curvy v1's `BrushDef::nibOutline`.

## strokeScatter (two inputs)

A scatter brush. Input 0 is the path, input 1 the artwork; copies of the artwork (centred on its bounding box) stand along every path (polyline or
cubic). Each copy keeps the artwork's primitives, takes the shape key of the first path (the stroke stays one shape) and carries the point and primitive attribute
`copyIndex`. The randomness depends only on `seed` and the copy's number, so cooking again gives the same picture. Built on `copyToPoints`.

| Parameter | Default | Meaning |
|---|---|---|
| `size` | 1 | the stroke weight: 1 draws the artwork at its own size, 2 twice as big (above 0) |
| `spacing` | 100 | distance between copies as a percentage of the artwork width times `size` (100: side by side; above 0) |
| `scatter` | 0 | how far a copy strays across the path, as a percentage of the artwork height times `size` |
| `sizeVariation` | 0 | random change of size, percent either way |
| `rotation` | 0 | random turn in degrees either way |
| `followPath` | 1 | 1 turns the copies with the path |
| `seed` | 1 | changes the random numbers |
| `scaleAttr` | `w` | point attribute that scales the copies along the stroke (pen pressure) |

Ported from curvy v1's `BrushDef::placements`. At most 5000 copies per path.
