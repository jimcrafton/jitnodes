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

The order is: `minDistance` and `minTime`, then `simplify`, then attribute removal. Polylines with fewer than three points and cubic primitives are
copied whole.

## calligraphy (one input)

A calligraphic pen. Every path (polyline or cubic; a cubic is sampled 12 times per segment) is swept by a slanted oval nib and comes out as closed
polylines to fill. The result is one convex piece per step of the path, all wound the same way, so a non-zero fill is their union; the pieces keep
the shape key of their path. All point attributes are dropped (the output is plain polygons).

Samples are not one piece each: a run of samples that lie within `tolerance` of the straight line between the run's ends, with a pressure scale that
changes by under 10 %, becomes one piece (the nib swept along a chord is the nib swept along a path that stays close to it). `tolerance` 0 keeps a
piece for every sample. A mouse's samples are whole pixels, so their jitter is about 0.5 and a tolerance of 0.25 merges only part of it.

| Parameter | Default | Meaning |
|---|---|---|
| `width` | 8 | the nib's long diameter (above 0) |
| `angle` | 45 | the nib's tilt in degrees, counter-clockwise from horizontal |
| `roundness` | 25 | the nib's thickness as a percentage of its length, above 0 up to 100 (100 is a round pen) |
| `scaleAttr` | `w` | the point attribute that scales the nib along the stroke (pen pressure); ignored when the points have none or it is 0 everywhere |
| `tolerance` | 0.25 | how far (document units) a skipped sample may lie from the chord that replaces it; 0 keeps every sample |

Ported from curvy v1's `BrushDef::nibOutline`.

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
