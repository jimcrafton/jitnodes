# Generating points and drawing pictures with them

Twenty-one nodes turn jitnodes into a small particle-image tool: four make points (`reactionDiffusion` makes a grid and `lsystem`
plants), sixteen shape them (noise, warp, colour, spacing, sampling curves, widening curves into ribbons, motion streaks,
smoothing, waves, strange attractors, flow lines, Voronoi cells, Lloyd relaxation, and three simulations: particles, boids,
springs), and one (`render`) says
how the points become an image. Together they draw pictures such as the nebula in
`examples/graphs/nebula.json` (about 650,000 sprites; cooking and drawing it takes about 0.35 s).

```
jitnodesCli examples/graphs/nebula.json --png nebula.png
jitnodesEditor examples/graphs/nebula.json
```

![The nebula graph rendered by jitnodesCli](images/nebula.png)

In the editor the twenty-one nodes are buttons in the palette (generate, scatter, noise, warp, colorRamp, relax,
attribBlur, wave, attractor, flow, voronoi, lloyd, particles, boids, springs, reaction, lsystem, curveSample, ribbon, trail, render): select a
node, click a button and the new node is wired to the selected one. Each example below is a small graph in
`examples/graphs/fx/`, rendered with `jitnodesCli <graph> --png <file>`; load it in the editor to change its
parameters and watch the picture change (the `render` node switches the viewport to the image view; double-click the
viewport to enlarge it).

## Contents

1. [The nodes](#the-nodes)
2. [From points to an image](#from-points-to-an-image)
3. [The nebula graph](#the-nebula-graph)
4. [Writing your own look](#writing-your-own-look)
5. [Performance and limits](#performance-and-limits)

## The nodes

All of them run on the thread pool for large point counts, and the same graph always gives the same points, whatever
the thread count: random numbers are a pure function of `(seed, point index)`.

### generate (no input)

Makes `count` points in a box, an ellipse or a Gaussian blob. Up to 50 million points.

| Parameter | Default | Meaning |
|---|---|---|
| `count` | 100000 | number of points |
| `seed` | 1 | random seed |
| `shape` | `rect` | `rect`: uniform in the box. `disc`: uniform in the ellipse inscribed in the box. `gaussian`: normal distribution, the box is about three standard deviations to each side |
| `cx`, `cy` | 0 | centre |
| `width`, `height` | 20, 10 | size of the box |

Point attributes: `rand` and `rand2` (independent, uniform in 0..1) and `id` (int, the index). There are no primitives.

![rect, disc and gaussian, 20,000 points each](images/fxGenerate.png)

`examples/graphs/fx/fxGenerate.json`: three `generate` nodes (`shape` rect, disc, gaussian; `cx` -11, 0, 11), each
followed by a wrangle that sets `radius`, `alpha` and colour, merged into a `render`. Note that a `generate` node
makes bare points: without a `radius` and `alpha` a downstream `render` draws nothing, so something has to set them.

### scatter (one input)

Scatters points on the shapes of its input: **inside closed primitives, along open ones**.

| Parameter | Default | Meaning |
|---|---|---|
| `count` | 10000 | points for all closed primitives together, shared out by area |
| `lineCount` | 1000 | points for all open primitives together, shared out by length |
| `spread` | 0 | open primitives only: random offset sideways, up to this distance either way |
| `seed` | 1 | random seed |
| `tolerance` | 0.1 | cubic Bezier curves are flattened to this accuracy first, as in `flatten` |

Each closed primitive is a simple polygon (concave is fine, holes are not: a primitive nested inside another is not
subtracted from it). A closed shape gets `count * area / total area` points, so density is uniform across shapes. The
points are found by rejection sampling inside the primitive's bounding box; a very thin shape wastes samples and, after
256 failed attempts for one point, that point is placed on a vertex of the polygon. Degenerate primitives (fewer than
three or two points, zero area or length) are skipped.

Point attributes: `rand`, `rand2`, `srcPrim` (int, the index of the input primitive) and `u` (0..1 along an open
primitive, 0 for a closed one). Input attributes are not carried over: scatter creates new points.

![scatter inside a closed shape and along an open curve](images/fxScatter.png)

`examples/graphs/fx/fxScatter.json`: `circle` bent by a wrangle into a lumpy closed shape and filled with
`count` = 20000 (left); `line` bent into a wave and walked with `lineCount` = 12000 and `spread` = 0.25 (right, tinted
by `u`, the position along the curve). A `scatter` with both kinds of primitive in its input does both at once; set
`count` or `lineCount` to 0 to switch one off.

### curveSample (one input)

Places points along the input curves and gives each one a frame and the curve's attributes. Open curves get a point at each end;
closed ones do not repeat the first point. Unlike `scatter`, which only keeps `rand`, `u` and `srcPrim`, every point attribute of
the curve (`Cd`, `radius`, `speed`, `strain`, ...) is **interpolated** along the segment it falls on (integer attributes take the
nearer end), so a colour, a size or a simulation attribute survives and the result can be coloured, warped or rendered afterwards.
Cubic curves are flattened first (`tolerance`, as in `scatter`).

| Parameter | Default | Meaning |
|---|---|---|
| `count` | 100 | points per curve (ignored when `spacing` is above 0) |
| `spacing` | 0 | if above 0, one point every `spacing` units of length along each curve (a curve of length L gets `floor(L / spacing) + 1` points, `floor(L / spacing)` if closed) |
| `random` | 0 | 1: random positions along the curve instead of even ones (`count` per curve, from `seed`) |
| `offset` | 0 | moves each point this far along its normal (to the left of the direction of travel); negative moves it right |
| `seed`, `tolerance` | 1, 0.1 | random seed; curve flattening tolerance |

The output has points only (no primitives), with `x`, `y`, every input point attribute, and: `u` (0 to 1 along the curve), `tx`, `ty`
(unit tangent), `nx`, `ny` (unit normal, the tangent turned a quarter turn left) and `curve` (the input primitive's index). Curves
with fewer than two points or no length produce nothing. The cost is linear in the samples and the attributes (2 million samples
of a curve with 8 attributes take about 50 ms) and the result is the same on any thread count. Use it after `springs` (`output` `springs`), `lsystem`,
`flow` or any other node that makes polylines, to turn lines into points that the renderer and `colorRamp` can use.

### noise (one input)

Writes a noise field, sampled at each point's `x`, `y`, into a point attribute.

| Parameter | Default | Meaning |
|---|---|---|
| `name` | `noise` | attribute to write |
| `mode` | `fbm` | `fbm`: smooth clouds, centred on 0.5 (about 0.1..0.9). `ridged`: 0..1 with bright creases, good for filaments and veins. `turbulence`: 0..0.7, absolute value of the noise |
| `scale` | 0.1 | frequency: features are about `1 / scale` units wide |
| `offsetX`, `offsetY` | 0 | shift the field |
| `phase` | 0 | third noise dimension: animate it (`time * 0.2`) and the field evolves smoothly instead of sliding |
| `octaves` | 5 | layers of detail, 1 to 12 |
| `lacunarity`, `gain` | 2, 0.5 | frequency and amplitude factors from one octave to the next |
| `seed` | 1 | picks another field |
| `amplitude`, `bias` | 1, 0 | result is `value * amplitude + bias` |

![fbm, ridged and turbulence noise through colour ramps](images/fxNoise.png)

`examples/graphs/fx/fxNoise.json`: the same `scale` and `seed` in each mode (fbm, ridged, turbulence), 60,000 points on
a box each, coloured with a `colorRamp` whose `min` and `max` cover the range of that mode (fbm 0.15..0.85, ridged
0..1, turbulence 0..0.6). The noise is a function of the point's `x` and `y`, so the three boxes show different parts
of the same field; give them the same `offsetX` to see the modes on identical coordinates.

### warp (one input)

Moves the points through a noise field. Attributes (including ones computed by `noise` earlier) travel with the points,
so the pattern is carried into the new shapes.

| Parameter | Default | Meaning |
|---|---|---|
| `mode` | `noise` | `noise`: offset by two independent noise values (a domain warp, gives billowy distortion). `curl`: follow the curl of a noise potential, a flow that neither compresses nor stretches, which pulls clouds of points into swirls and wisps |
| `amount` | 1 | distance moved, in world units per unit of noise slope (`curl`) or per noise value (`noise`), over all iterations |
| `scale`, `octaves`, `lacunarity`, `gain`, `seed`, `phase` | as in `noise` | the field that moves the points |
| `iterations` | 1 | the move is split in this many steps and the field is re-sampled at each new position (and phase); more steps give longer, more curled streaks. 1 to 64 |

![a grid, the same grid noise-warped and curl-warped](images/fxWarp.png)

`examples/graphs/fx/fxWarp.json`: a checker pattern made by a wrangle (left, unwarped), then through `warp` in `noise`
mode (middle, `amount` 1.2: the grid wobbles but stays connected) and in `curl` mode (right, `amount` 1.5,
`iterations` 3: the pattern is dragged into swirls and tears apart, since curl flow moves points rather than
stretching a surface). Both warps read the same unwarped points, and the colour travels with each point.

### colorRamp (one input)

Maps a float attribute onto a gradient and writes `Cd.x`, `Cd.y`, `Cd.z`.

| Parameter | Default | Meaning |
|---|---|---|
| `attribute` | `noise` | point attribute to read (float or int) |
| `ramp` | `0 #000000, 1 #ffffff` | stops separated by commas, semicolons or new lines; each is a position then a colour, either `#rgb`, `#rrggbb` or three numbers 0..1 (`0.5 1 0.2 0.1`). Colours are interpolated linearly in RGB; positions outside the stops clamp to the end colours |
| `min`, `max` | 0, 1 | attribute values that map to ramp positions 0 and 1 (an inverted range reverses the ramp) |
| `gamma` | 1 | the position is raised to this power first; above 1 favours the dark end |

The ramps in the pictures above are, for example, `0 #0b0630, 0.35 #3b4cc0, 0.7 #f4a582, 1 #fff7e0` (the fbm box in
`fxNoise.json`) and `0 #101a4a, 0.5 #3aa0d8, 1 #ffd070` (the checker in `fxWarp.json`).

### relax (one input)

Pushes points that are closer than `radius` apart until they are not: even, blue-noise-like spacing instead of the clumps
and gaps of random points. Each iteration every point moves away from all its neighbours within `radius` by half the
overlap (times `strength`); all points move at once, so the result does not depend on the order or the thread count.

| Parameter | Default | Meaning |
|---|---|---|
| `radius` | 0.5 | the distance points are pushed apart to. Start a little above the average spacing, `sqrt(area / points)`; a radius far larger than that cannot be satisfied, so the points pack tight and pile up at the edges |
| `iterations` | 20 | 1 to 500 |
| `strength` | 0.4 | fraction of the overlap corrected per iteration; high values can overshoot in dense clusters, low ones just need more iterations |
| `keepInside` | 1 | 1: points stay inside the bounding box of the input. 0: they may drift out; the cloud then grows slightly |
| `seed` | 1 | only picks the direction in which two points at exactly the same place separate |

Attributes are untouched and travel with the points. Points sit on a uniform grid, so the cost is the number of points times
the number of neighbours within `radius`: 2,500 points and 60 iterations take about 20 ms.

![2,500 random points, and the same points after relax](images/fxRelax.png)

`examples/graphs/fx/fxRelax.json`: the same points (same `seed`, different `cx`) with and without `relax`
(`radius` 0.3, `iterations` 60, `strength` 0.5). The right-hand side settles into a loose hexagonal packing, which is what
the even distribution of equal discs looks like; the `scatter` node's random points are the left-hand kind.

### attribBlur (one input)

Replaces a point attribute with the average of its neighbours within `radius`, each weighted by distance, and repeats
that `iterations` times. It turns the noise of a random attribute into soft regions, and it smooths colours, `alpha`, or
any other float or int attribute (the output is always a float).

| Parameter | Default | Meaning |
|---|---|---|
| `attribute` | `rand` | the attribute to blur; several are separated by commas or spaces (`Cd.x, Cd.y, Cd.z` blurs a colour) |
| `output` | empty | where to write the result, one name per attribute. Empty: overwrite the input attributes (they must be floats); a name that already exists as an int is an error |
| `falloff` | `linear` | weight by distance: `linear` (1 at the point, 0 at `radius`), `box` (everyone counts the same), `gaussian` (`exp(-4 d^2 / r^2)`) |
| `radius` | 0.5 | neighbourhood size |
| `iterations` | 1 | 1 to 64; each pass blurs the result of the last, which behaves like a wider radius |

A point always counts itself with weight 1, so an isolated point keeps its value. The blur uses the positions of the input, so
positions are not part of it unless you name `x` or `y`. Cost is points times neighbours times iterations; `fxBlur.json` below
has about 900 neighbours per point in the last panel and takes 0.66 s for its 20,000 points: keep `radius` a few times the point
spacing, and use more iterations rather than a larger radius for a wide smooth blur.

![raw random values, blurred once, blurred four times](images/fxBlur.png)

`examples/graphs/fx/fxBlur.json`: the same `rand` values drawn through one colour ramp, raw (left), after one pass with
`radius` 0.5 (middle) and after four passes with `radius` 1.2 (right). Blurring pulls every value towards 0.5, so the
panels use narrower `min`/`max` on their `colorRamp` to keep the contrast; the black specks are gaps between the points.

### wave (one input)

Writes a travelling sine wave into an attribute, and can move the points by it.

| Parameter | Default | Meaning |
|---|---|---|
| `name` | `wave` | attribute to write, in `-amplitude..amplitude` |
| `mode` | `radial` | `radial`: rings spreading from the sources. `plane`: straight fronts moving in the direction `angle` |
| `cx`, `cy` | 0 | the source (`radial`), or a point on the front where the phase is zero (`plane`) |
| `cx2`, `cy2`, `sourceCount` | 0, 0, 1 | `radial` only: with `sourceCount` above 1 the sources are spread evenly from (`cx`,`cy`) to (`cx2`,`cy2`) and their waves are averaged; 2 gives interference fringes, many gives a line source (1 to 256) |
| `angle` | 0 | `plane` only: direction of travel in degrees from +x |
| `wavelength` | 2 | distance between crests; must not be 0 |
| `phase` | 0 | radians; animate it with `time * speed` and the wave travels (crests move outwards with a positive `time` coefficient) |
| `amplitude` | 1 | the wave is `amplitude * sin(2 pi r / wavelength - phase)` |
| `falloff` | 0 | multiplies the wave by `exp(-falloff * distance)`; use it so rings fade away from their source |
| `displaceX`, `displaceY` | 0 | each point is also moved by `wave * displaceX` in x and `wave * displaceY` in y, in world units. A plane wave with `displaceX` bunches points into bands (a compression wave); `displaceY` gives a rippling sheet |

The wave is a pure function of the point's position, so there is no state and it works at any frame; `phase` is the only time
dependence.

![two-source interference, and a plane wave that bunches the points](images/fxWave.png)

`examples/graphs/fx/fxWave.json`: left, two sources 5 units apart (`cx`..`cx2`, `sourceCount` 2, `wavelength` 1.4,
`falloff` 0.02) shown with a `colorRamp` on `wave`; right, a plane wave (`angle` 0, `wavelength` 3.5) with
`displaceX` 0.7: the displacement is as large as the wavelength allows, so the points fold into dense bands with empty gaps
between them.

### attractor (one input)

Runs every input point through a chaotic two-dimensional map a number of times and moves it to where it ends up. The input
points are the starting positions, in the map's own units (a `generate` of `width` and `height` 4 is a good seed cloud; the
orbit stays within `1 + |c|` for Clifford, 2 for de Jong and `1 + |d|` for Svensson, in both directions). With enough seeds the final positions trace the attractor, and drawn additively their density shows its
structure: filaments and sheets that are brighter where the orbit passes often.

| Parameter | Default | Meaning |
|---|---|---|
| `type` | `clifford` | `clifford`: `x' = sin(a y) + c cos(a x)`, `y' = sin(b x) + d cos(b y)`. `dejong`: `x' = sin(a y) - cos(b x)`, `y' = sin(c x) - cos(d y)`. `svensson`: `x' = d sin(a x) - sin(b y)`, `y' = c cos(a x) + cos(b y)` |
| `a`, `b`, `c`, `d` | -1.4, 1.6, 1, 0.7 | the map's constants (the defaults are a classic Clifford attractor). Small changes give very different shapes, and some sets collapse to a point or a closed loop |
| `iterations` | 30 | how many times the map is applied (1 to 10000); the first tens of steps carry the seed onto the attractor |
| `scale`, `cx`, `cy` | 3, 0, 0 | world position = (`cx`, `cy`) + map position * `scale` |

Point attributes written: `speed` (length of the last step in map units) and `dir` (direction of the last step, 0..1 around the
circle), both good for a `colorRamp`. Everything else is kept.

Parameter sets that make good pictures:

| Type | a | b | c | d |
|---|---|---|---|---|
| clifford | -1.4 | 1.6 | 1.0 | 0.7 |
| clifford | -1.7 | 1.3 | -0.1 | -1.2 |
| dejong | 1.641 | 1.902 | 0.316 | 1.525 |
| dejong | 1.4 | -2.3 | 2.4 | -2.1 |
| svensson | 1.5 | -1.8 | 1.6 | 0.9 |

The `a`..`d` fields are parameters like any other, so animating them (`sin(time) * 0.1 + 1.5`) morphs the shape smoothly.

![four attractors](images/fxAttractor.png)

`examples/graphs/fx/fxAttractor.json`: four attractors of 400,000 seed points each (the first and second Clifford rows of the table,
the first de Jong row and the Svensson row) placed in the four quarters with `cx`/`cy`, coloured by `dir`, drawn additively with
`radius` 0.02 (under a pixel, so each point adds a faint dot) and `alpha` 0.12. The whole picture, 1.6 million points through 30
iterations, cooks in about 0.3 s.

### flow (one input)

Follows each input point through a noise-driven vector field and outputs the path as an open polyline: streamlines of a flow
field. The input points are the seeds (scatter or generate them); the output has one primitive of `steps + 1` points per seed
(`2 * steps + 1` with `bothWays`).

| Parameter | Default | Meaning |
|---|---|---|
| `mode` | `curl` | `curl`: the curl of a noise potential, a flow that never converges, so trails wind around without piling up. `angle`: the noise value is the heading (`turns` full turns across its range), the classic flow-field look, where trails run together into bright ridges |
| `steps` | 64 | steps per direction, 1 to 4096 |
| `stepLength` | 0.1 | distance moved per step; the vertex spacing, so choose a sprite `radius` of about this size or more to draw a continuous line |
| `bothWays` | 0 | 1: also trace backwards, so the seed is the middle of the trail |
| `normalize` | 1 | 1: every step has length `stepLength`. 0: the step is the field vector times `stepLength` (curl) or `strength` times it (angle), so the trail speeds up and slows down |
| `strength`, `turns` | 1, 1 | the length scale for `normalize` 0, and the number of turns the heading makes in `angle` mode |
| `scale`, `octaves`, `lacunarity`, `gain`, `seed` | as in `noise` | the field |
| `phase`, `drift` | 0, 0 | the field's third dimension. `phase` picks a frozen field (animate it and the whole pattern evolves); `drift` also advances it by that much each step, so trails are paths through a changing field |

Every point attribute of the seed is copied to all vertices of its trail, and these are added: `u` (0 at the start of the trail
to 1 at its end; colour and fade along the trail) and `trail` (int, the seed's index). Cost is seeds times steps: 6,000 seeds of 80
steps (480,000 points) take about 170 ms in curl mode and 55 ms in angle mode.

![curl and angle flow fields](images/fxFlow.png)

`examples/graphs/fx/fxFlow.json`: 6,000 seeds each on the two halves. Left, `curl` (`scale` 0.12, 80 steps of 0.14): the trails curl
into closed eddies around the field's maxima and minima. Right, `angle` with `turns` 1.2: trails bunch into bright ridges and
spread out again. `colorRamp` on `u` colours them and a wrangle fades `alpha` with `u`.

### voronoi (one input)

The Voronoi diagram of the input points: one polygon per point, the region of the plane closer to that point than to any other,
clipped to the bounding box of the points. Each cell is built by cutting a rectangle with the perpendicular bisectors of its
nearest neighbours (found ring by ring in a grid), so the cells are exact and cheap: 5,000 sites take 8 ms and 100,000 sites 40 ms.

| Parameter | Default | Meaning |
|---|---|---|
| `margin` | 0 | grows the clipping rectangle on every side; with 0 the cells of the outermost points end exactly at the bounding box |
| `closed` | 1 | 1: closed polygons (fill them with `scatter count`). 0: open outlines that repeat the first corner (walk them with `scatter lineCount`) |

Each cell is a primitive whose points all carry the attribute values of its site, plus `site` (int, the input point's index) and
`area` (the cell's area). Coincident sites give the same cell twice. A site whose cell is empty produces no primitive. Position
attributes are replaced by the cell corners; there is no per-site point output, so keep the input if you want the sites too.

![voronoi cells filled and as outlines](images/fxVoronoi.png)

`examples/graphs/fx/fxVoronoi.json`: the same 70 random sites; left, closed cells filled by `scatter` (45,000 points) and coloured
by `srcPrim` (the cell's index) in a wrangle; right, the sites spread by `relax` first, outlines walked by `scatter lineCount`.
Cells of relaxed sites are close to hexagons; those of random sites are the usual mix of tiny and huge.

### lloyd (one input)

Lloyd relaxation: every point moves to the centroid (centre of mass) of its Voronoi cell, `iterations` times. Random points
become evenly spread ones with cells of nearly equal size. The cells are clipped to the bounding box of the input points (plus
`margin`), and that box stays fixed during the iterations, so the points spread out to fill it. Unlike `relax`, which only
pushes apart points that are closer than a radius, `lloyd` needs no radius and gives a more regular, hexagon-like pattern.
Each iteration builds the diagram as `voronoi` does: 5,000 points take roughly 8 ms per iteration.

| Parameter | Default | Meaning |
|---|---|---|
| `iterations` | 5 | number of relaxation steps (1 to 200); the pattern converges slowly, most of the change happens in the first few |
| `strength` | 1 | fraction of the way to the centroid that each point moves (0 leaves the points alone, 1 jumps to the centroid) |
| `margin` | 0 | grows the clipping box on every side |

All point attributes are kept; only `x` and `y` change. Points whose cell is empty (coincident duplicates) stay where they are.
Feed the result to `voronoi` to draw the cells.

![lloyd relaxation: random, 2 iterations, 30 iterations](images/fxLloyd.png)

`examples/graphs/fx/fxLloyd.json`: the same 60 random sites three times, left untouched, after 2 iterations and after 30, each
with its Voronoi outlines (`voronoi` with `closed` 0, walked by `scatter lineCount`). The cells go from a mix of tiny and huge to
nearly uniform.

### particles (one input)

A particle simulation. The input points are the particles at `startTime`; the result is where they are at the playhead, after
fixed steps of force integration (semi-implicit Euler at `stepsPerSecond`). It is the one node that reads the playhead itself, so it
recooks as soon as the time changes and needs no `time` expression. Press play and it runs.

| Parameter | Default | Meaning |
|---|---|---|
| `startTime`, `stepsPerSecond`, `seed` | 0, 120, 1 | the particles stay at their start before `startTime`; steps per second (10 to 2000); seed of the random emission |
| `gravityX`, `gravityY` | 0, -2 | constant acceleration |
| `drag` | 0.1 | velocity decays by `exp(-drag * dt)` each step |
| `noiseForce`, `noiseScale`, `noiseSpeed` | 0, 0.2, 0.2 | acceleration from a curl-noise field: strength, size of the features (`scale` as in `noise`) and how fast the field evolves with time |
| `attract`, `attractX`, `attractY`, `attractSoftening` | 0, 0, 0, 0.5 | pull towards a point: `attract * d / (r^2 + softening^2)^1.5`, an inverse-square force that does not blow up at the centre |
| `swirl` | 0 | the same force turned a quarter turn: acceleration around the attractor point |
| `repel`, `repelRadius` | 0, 0.3 | particles closer than `repelRadius` push each other apart (soft collisions); the strongest push is `repel` at distance 0. This builds a grid every step, so it is the expensive force |
| `lifetime` | 0 | seconds a particle lives (0: forever). When it ends the particle restarts from its input position and velocity. Starting ages are staggered, so the respawns are spread over the lifetime instead of all happening at once |
| `initialSpeed`, `speedVariation`, `direction`, `spread` | 0, 0, 90, 360 | emission velocity added to the particle's own: speed (with a random fraction `speedVariation` either way), heading in degrees from +x and the cone width in degrees. A particle gets a new random velocity at each respawn |
| `boundary` | `none` | `none`; `bounce` (reflect at the box with the velocity scaled by `bounce`, and `friction` slows the sliding velocity); `wrap` (leave on one side and come back on the other) |
| `boundsMinX`, `boundsMaxX`, `boundsMinY`, `boundsMaxY` | -10, 10, -6, 6 | the box for `bounce` and `wrap` |
| `bounce`, `friction` | 0.6, 0 | restitution and sliding friction at the box |

Input attributes `vx` and `vy` give each particle a starting velocity (a wrangle can set them, as in the orbit example); the
output has `vx`, `vy`, `age` (seconds since the last respawn), `life` (`age / lifetime`, 0 to 1, 0 without a lifetime) and `speed`,
and keeps every other attribute, so colour and `rand` survive.

**How the simulation relates to time.** The result is a pure function of the input, the parameters and the playhead: the same time
always gives the same points, on any number of threads, and a graph reopened tomorrow gives the same frame. The node remembers the
state of its latest cook. Playing forward only advances the new steps (cheap); scrubbing backwards, changing a parameter or changing
the input restarts from the input and simulates up to the playhead (cost proportional to the elapsed time). The step count is
limited to 1,000,000 (about 2.3 hours at the default rate); a simulation that long is better baked than cooked.

Cost per second of animation is points times `stepsPerSecond` times the work of the forces: the fountain and the orbit below
(14,000 and 12,000 particles, 6 s, 720 steps) take about 45 ms each. `noiseForce` adds about eight noise samples per particle per
step, and `repel` a grid build and neighbour search.

![a fountain and orbiting particles at t = 6 s](images/fxParticles.png)

`examples/graphs/fx/fxParticles.json` rendered with `jitnodesCli ... --time 6`. Left, a fountain: a tiny gaussian `generate` as
the nozzle, `initialSpeed` 11 in a 50 degree cone, gravity -9.8, a bouncing floor and `lifetime` 4 (coloured by `life`). Right, 12,000
particles on a disc around an attractor of strength 30, each given 55% of the circular speed by a wrangle (`vx`, `vy`); by t = 6 the
disc has wound up into concentric shells, coloured by `speed`. In the editor, press play and both move.

### boids (a flock input and any number of obstacle inputs)

A flock. Every input point is a boid that steers by Reynolds' three rules, looking only at the boids within `radius`: **separation**
(move away from those closer than `separationRadius`), **alignment** (match their heading) and **cohesion** (move towards their
centre). A boid's speed always stays between `minSpeed` and `maxSpeed`. It is simulated like `particles`: fixed steps, the state of
the latest cook is cached, and the result is a pure function of input, parameters and playhead (every boid reads the previous step's
state, so the thread count does not matter).

| Parameter | Default | Meaning |
|---|---|---|
| `startTime`, `stepsPerSecond`, `seed` | 0, 60, 1 | as in `particles` (10 to 1000 steps per second); the seed picks the initial headings |
| `radius` | 1 | how far a boid sees; must be greater than 0 |
| `separationRadius` | 0.4 | boids closer than this repel each other (push is stronger the closer they are) |
| `separation`, `alignment`, `cohesion` | 1.5, 1, 1 | weight of each rule; 0 switches a rule off |
| `minSpeed`, `maxSpeed`, `maxForce` | 1, 3, 6 | speed limits and the strongest steering acceleration of a single rule |
| `goal`, `goalX`, `goalY` | 0, 0, 0 | weight of a steering force towards a point (0: none) |
| `avoid`, `avoidDistance`, `obstacleRadius` | 3, 1, 0.5 | steering away from obstacles (see below): weight, how far outside an obstacle's edge it starts, and the radius of obstacles that have no `radius` attribute |
| `wander`, `wanderScale`, `wanderSpeed` | 0, 0.2, 0.2 | a curl-noise acceleration that keeps the flock from settling: strength, feature size and evolution speed |
| `boundary` | `wrap` | `wrap` (leave one side, return on the other), `bounce` (reflect at the box) or `none` |
| `boundsMinX`, `boundsMaxX`, `boundsMinY`, `boundsMaxY` | -10, 10, -6, 6 | the box for `wrap` and `bounce` |

Input attributes `vx` and `vy` give the initial velocities; without them every boid starts at a random heading with the mean of
`minSpeed` and `maxSpeed`. The output has `vx`, `vy`, `speed`, `angle` (heading in radians, -pi to pi: colour by it with
`colorRamp`) and `neighbors` (how many boids it sees), and keeps every other attribute. Cost is boids times the boids within
`radius`, per step: the two flocks below (900 boids, 14 s, 840 steps) take 290 ms for the clustered flock and 150 ms for the sparse one.

**Obstacles.** The first input is the flock. Every further input (a new empty one appears whenever you connect one, as on `merge`)
is a set of obstacles: each of its points is a circle centred on the point, with the radius of its `radius` attribute, or
`obstacleRadius` if the point has none. A boid within `avoidDistance` of a circle's edge steers away from it, more strongly the
closer it is (`avoid` is the weight), and a boid that ends a step inside a circle is moved to its surface and loses the velocity
into it, so nothing ever sits inside a rock. Because `render` also draws a point with its `radius`, the same points can be merged
into the picture as discs, as in the example.

**Moving obstacles.** An obstacle point with `vx` / `vy` attributes drifts at that constant velocity: it sits at its input position
at `startTime` and is at `position + velocity * (time - startTime)` after that. A boid pushed by a moving rock also takes the
rock's velocity into account (it loses only the velocity *relative* to the rock that points into it). The positions follow the
simulation's own clock, so a moving obstacle does not restart the simulation and the result is still a pure function of time. Only
straight-line motion is supported this way; an obstacle input that is itself animated by the playhead (a node that depends on
time upstream) is a new geometry at every time, so it is read when the simulation starts and restarts the simulation at every new
time: the right picture, but a simulation from the start each frame.

![two flocks: swarms and a gas](images/fxBoids.png)

`examples/graphs/fx/fxBoids.json` at `--time 14`. Left, `alignment` 3 and `cohesion` 1.2 gather the boids into several swarms
that circle together (colour is the heading, so a swarm shows the whole colour wheel while it turns). Right, `alignment` and
`cohesion` 0 and `separation` 2.5 keep the boids evenly spread with random headings, like a gas.

![a flock splitting round rocks](images/fxBoidsRocks.png)

`examples/graphs/fx/fxBoidsRocks.json` at `--time 4.8`: 900 boids start on the left and are pulled towards a goal far to the right
(`goal` 1.5). Seven rocks (a `generate` plus a wrangle that sets `radius` from `rand`) are the second input of `boids` (`avoid` 6,
`avoidDistance` 1.2) and are merged into the picture as soft discs. The front of the flock has split in two round the cluster of
rocks and streams through the gaps. Colour is the heading.

### springs (one input)

Ropes, chains, cloth and soft bodies. The points of every input primitive are joined to the next point by a spring whose rest
length is the initial distance (closed primitives also join the last point to the first). With `neighborRadius` greater than 0
every pair of points that start closer than that is joined too, which turns any point cloud into a soft mesh. The solver is
position-based dynamics: Verlet integration, then `iterations` passes that move each pair of points towards its rest length, so it
stays stable however stiff the springs are and never explodes. Cached and stepped like `particles`, and a pure function of time.

| Parameter | Default | Meaning |
|---|---|---|
| `startTime`, `stepsPerSecond` | 0, 120 | as in `particles` (10 to 2000 steps per second) |
| `gravityX`, `gravityY` | 0, -4 | constant acceleration |
| `damping` | 0.2 | velocity decays by `exp(-damping * dt)` each step |
| `stiffness`, `iterations` | 1, 8 | `stiffness` is the fraction (0 to 1) of a spring's error removed per pass; more `iterations` (1 to 100) make the whole body stiffer and cost proportionally more |
| `restScale` | 1 | rest length as a fraction of the initial length (below 1 the springs are tense, above 1 the body sags) |
| `neighborRadius` | 0 | also join points that start closer than this (limit: 20 million springs) |
| `pinEnds` | 0 | 1: the first point of every open primitive stays where it is |
| `output` | `points` | `points`: the input geometry with moved points. `springs`: one two-point line per spring (see below) |
| `windForce`, `windScale`, `windSpeed` | 0, 0.2, 0.3 | a curl-noise acceleration (flutter): strength, feature size and evolution speed |
| `useBounds`, `boundsMinX`, `boundsMaxX`, `boundsMinY`, `boundsMaxY` | 0, -10, 10, -6, 6 | 1 keeps the points inside the box (a floor and walls) |

Points whose `pin` attribute is not 0 never move (a wrangle can set it, for example `pin = y > 3;` for the top edge of a cloth).
Input attributes `vx` and `vy` give the initial velocities. The output keeps the input geometry, so open primitives still draw as
lines, and has `vx`, `vy`, `speed` and `strain` (the average of the relative stretch `(length - rest) / rest` of the springs at a
point: positive stretched, negative squeezed).

With `output` set to `springs` the node returns the springs as geometry instead: one open two-point line per spring (so a mesh made
with `neighborRadius` can be drawn, which the points output cannot show), 2 points per spring, each end carrying all the attributes
of the point it belongs to, and `strain` replaced by the spring's own relative stretch (the same on both ends) plus `spring` (the
spring's index). The same simulation runs either way, so you can switch between the two. Put `curveSample` after it to turn the
lines into points that keep `strain`, `speed` and the rest; `scatter` would drop them.

Springs are solved in order on one thread, so the cost is springs times `iterations` times steps, about 10 ns per spring per pass:
3,000 points joined to their neighbours within 0.5 (45,000 springs) at 8 iterations cost 0.48 s per animated second. Scrubbing
backwards re-simulates from the start, so a body with tens of thousands of springs is better baked than scrubbed.

![a swinging chain and a jelly hitting the floor](images/fxSprings.png)

`examples/graphs/fx/fxSprings.json` at `--time 1.1`. Left, an 80-point `line` pinned at one end with `pinEnds`, released horizontal
under gravity -9, then `curveSample` (spacing 0.025) so the points keep `speed`, which colours it. Right, a disc of 160 random
points joined with `neighborRadius` 1 (1,032 springs) and `stiffness` 0.35, falling at gravity -24 onto a floor at y = -6
(`useBounds`), just after impact. Its `output` is `springs`, `curveSample` (spacing 0.1) puts points along every spring and a
`colorRamp` on `strain` colours them (red squeezed, green stretched).

### reactionDiffusion (no input)

The Gray-Scott reaction-diffusion model on a grid. Two chemicals diffuse and react: `u` is fed in at rate `feed`, and the catalyst
`v` consumes it (`u + 2v -> 3v`) and is removed at rate `feed + kill`. Depending on `feed` and `kill` the grid grows spots, stripes,
mazes, coral or solitons, the patterns of animal skins. The node makes a grid of `width` x `height` points with the attributes `u` and
`v` (both 0 to 1); colour them with `colorRamp` on `v` and give them a `radius` slightly larger than `cellSize`. It starts with `u` = 1
everywhere and `seedCount` squares of catalyst, then runs `stepsPerSecond` explicit steps per second of playhead time. It is cached
and stepped like `particles` and a pure function of time, on any thread count.

| Parameter | Default | Meaning |
|---|---|---|
| `startTime`, `stepsPerSecond` | 0, 400 | before `startTime` the grid is the initial state; steps per second (1 to 20,000) |
| `width`, `height` | 160, 90 | grid size in cells (8 to 2048, at most 4 million cells) |
| `cellSize`, `cx`, `cy` | 0.1, 0, 0 | distance between points and the centre of the grid in the plane |
| `feed`, `kill` | 0.055, 0.062 | the two rates that choose the pattern (see below) |
| `diffuseU`, `diffuseV` | 1, 0.5 | diffusion rates in cells per step; keep `diffuseV` below `diffuseU` or nothing forms |
| `stepSize` | 1 | time step; it is capped at `1.2 / max(diffuseU, diffuseV)` to keep the scheme stable |
| `periodic` | 1 | 1: the grid wraps around (it tiles); 0: the edges reflect |
| `seed`, `seedCount`, `seedSize` | 1, 12, 3 | random catalyst squares to start from: how many and their half-size in cells |

`feed` / `kill` pairs worth trying (after Pearson's classification of the model): 0.055 / 0.062 coral (the default); 0.029 / 0.057
mazes; 0.078 / 0.061 worms; 0.030 / 0.062 solitons; 0.037 / 0.065 spots that divide like cells ("mitosis"). Small changes make a
different pattern or none at all, so change them in steps of 0.001. Patterns take thousands of steps to form: with the default 400
steps per second, 10 s of playhead time is 4,000 steps. Raise `stepsPerSecond` to see the pattern sooner.

The output points are a regular grid in row order (the first row has the lowest `y`), and `u` and `v` are the only attributes, so the
result can be fed to `scatter`, `warp` or `render` like any other points. Cost is cells times steps: 12,100 cells and 12,000 steps (the
example) take about 480 ms, and a 512 x 512 grid costs about 35 ms per 100 steps (0.35 ms per step, including the output points). The step count is limited to 2,000,000.

![coral and spots from the Gray-Scott model](images/fxReaction.png)

`examples/graphs/fx/fxReaction.json` at `--time 30` (12,000 steps of two 110 x 110 grids), the same seed and the same chemistry
except `feed` and `kill`. Left, 0.055 / 0.062 grows branching coral; right, 0.037 / 0.065 gives evenly spaced spots.

### lsystem (no input)

A Lindenmayer system: start from the `axiom` string, rewrite it `iterations` times with the `rules`, then draw the result with a
turtle that starts at (`startX`, `startY`) facing `startAngle` degrees from +x (90: up). Plants, trees, snowflakes and the
Sierpinski triangle all come from a few short rules.

| Symbol | Meaning |
|---|---|
| `F`, `G` | move forward `length` and draw a line |
| `f` | move forward without drawing (starts a new stroke) |
| `+`, `-` | turn left, turn right by `angle` degrees |
| `\|` | turn around (180 degrees) |
| `[`, `]` | save the turtle and start a branch, restore the turtle (the branch ends) |
| anything else (`X`, `A`, ...) | does nothing when drawing; it exists to be rewritten by the rules |

| Parameter | Default | Meaning |
|---|---|---|
| `axiom` | `X` | the start string; spaces are ignored |
| `rules` | `X=F[+X][-X]FX; F=FF` | rewrite rules `S=replacement`, separated by `;` or line breaks. `S` is one character. Several rules for the same `S` make the rewriting stochastic: one is picked at random for every occurrence, from `seed` |
| `iterations` | 5 | rewriting steps (0 to 30); the string is limited to 50 million symbols and the drawing to 20 million points |
| `angle`, `length` | 25, 0.1 | turn angle in degrees and the length of an `F` step |
| `branchScale` | 1 | every `[` multiplies the step length of that branch by this: 0.8 makes each generation of branches shorter |
| `angleJitter`, `lengthJitter` | 0, 0 | random variation of every turn (plus or minus this many degrees) and of every step (plus or minus this fraction), from `seed` |
| `seed` | 1 | seed of the random rule choice and the jitter |
| `startX`, `startY`, `startAngle` | 0, 0, 90 | where the turtle starts and which way it faces |

The output is one open polyline per stroke, that is per run of `F` between a `[`, `]` or `f` (a branch starts a new line at the
branch point, and the trunk continues in a new line after it). Every point has `depth` (the bracket depth, 0 for the trunk, an
integer attribute), `stroke` (the stroke's index) and `u` (0 to 1 in drawing order, so it grows with the plant). Run
`curveSample` on it to get points that keep these and colour them with `colorRamp` on `depth` or `u`. It is deterministic: the same
axiom, rules and seed always give the same drawing. The three plants below take 0.15 ms each to rewrite and draw (about 2,400,
540 and 2,200 points); the string grows by the size of the rules per iteration, so 8 iterations of a rule that doubles it is 256
times the work of none.

![three L-systems: a fern, a tree and the Sierpinski triangle](images/fxLsystem.png)

`examples/graphs/fx/fxLsystem.json`. Left: `X=F+[[X]-X]-F[-FX]+X; F=FF` at 25 degrees, 5 iterations, a fern-like plant coloured by
`depth`. Middle: three alternative rules for `X` at 24 degrees with `angleJitter` 9, `lengthJitter` 0.25 and `branchScale` 0.82, so
every branch is shorter than its parent and no two look alike. Right: `F=F-G+F+G-F; G=GG` at 120 degrees, the Sierpinski
triangle, coloured along its stroke by `u`. Each is sampled with `curveSample` and `spacing` 0.04 to 0.05.

### ribbon (one input)

Gives curves a width. Every input curve (polylines; cubics are flattened first, `tolerance` as in `scatter`) is offset to its left and
right by half the width at each point, and the offsets become new lines: a closed **outline**, two **edges**, or a line across the
curve at every point (**rungs**). Corners are mitred, up to twice the half width, so sharp turns do not pinch or spike.

| Parameter | Default | Meaning |
|---|---|---|
| `width`, `widthEnd` | 0.1, 0.1 | full width at the start and at the end of each curve; it changes linearly in between (by arc length) |
| `taper` | 0 | 0 to 0.5: pinch both ends of an open curve to a point over this fraction of its length (smooth); closed curves are not tapered |
| `widthAttrib` | empty | name of a point attribute that multiplies the width at each point (the node throws if the input has no such attribute) |
| `output` | `outline` | `outline`: one closed polyline per open curve (left side forward, right side back); two closed rings per closed curve. `edges`: two lines per curve, open for an open curve. `rungs`: one two-point line per curve point |
| `tolerance` | 0.1 | curve flattening tolerance |

Every output point keeps the attributes of the curve point it came from, with three added: `side` (1 on the left of the direction of
travel, -1 on the right), `u` (0 to 1 along the curve) and `width` (the width used at that point). The output has no filled area
(the geometry has no polygons), so to see a ribbon in the renderer, run `curveSample` on the outline as in the example. Cost is
linear in the points and the attributes.

![a tree drawn with tapered ribbons](images/fxRibbon.png)

`examples/graphs/fx/fxRibbon.json`: an `lsystem` tree whose strokes get `w = 1 - depth * 0.13` from a `wrangle`; `ribbon` uses it
as `widthAttrib` (thinner towards the tips) with `width` 0.6 and `taper` 0.5, so every stroke becomes a thin leaf-shaped outline.

### trail (one input)

A motion streak behind every point: a polyline of `segments` segments starting at the point and running back along its velocity
(`vx`, `vy`) for `length` seconds. It is an extrapolation of the *current* velocity, a straight line, not a record of where the point
has been (that would need the input at earlier times, which a node cannot ask for). It is exact for points that move in a straight
line, such as `particles` without forces or `boids` over a short `length`, and it is a pure function of the input. The node throws if
the input has neither `vx` nor `vy`.

| Parameter | Default | Meaning |
|---|---|---|
| `length` | 0.3 | how many seconds of velocity the streak covers |
| `segments` | 8 | segments per streak (1 to 1000); each streak has `segments + 1` points |
| `fade` | 1 | 0 to 1: `alpha` is multiplied by `1 - fade * u` along the streak, so the tail disappears at 1 (an input without `alpha` is taken as 1) |
| `maxLength` | 0 | if above 0, longer streaks are shortened to this length in world units |

Every streak point keeps the attributes of its point (position aside), plus `u` (0 at the head, which sits on the point, to 1 at the
tail) and `trail` (the index of the point it belongs to). The output has one open polyline per input point. Put `curveSample` after
it to turn the lines into evenly spaced points that keep `Cd` and the faded `alpha`.

![streaks behind a flock](images/fxTrails.png)

`examples/graphs/fx/fxTrails.json` at `--time 9`: the clustering flock of the boids example, coloured by heading, with `trail`
(`length` 0.45, 6 segments) and `curveSample` (`spacing` 0.04).

### render (one input)

Passes the geometry through unchanged and records how to draw it, as detail values named `render.*`. The editor's
viewport and `jitnodesCli --png` draw geometry that carries them with the splat renderer; anything else is drawn as
before (lines, dots, plain sprites). Choosing a *Color by* attribute in the editor switches back to the normal view.

| Parameter | Default | Meaning |
|---|---|---|
| `additive` | 1 | 1: light adds up (stars, glows, nebulae). 0: sprites are composited in index order over the background, like the plain viewport sprites |
| `exposure` | 1 | brightness of additive images: the summed light `v` becomes `1 - exp(-exposure * v)`, so it saturates smoothly instead of clipping |
| `gamma` | 2.2 | additive images are brightened by `1 / gamma`; 1 keeps the exposure curve as is |
| `bloom`, `bloomRadius` | 0, 12 | additive only: adds a blurred copy of the picture (strength, and a radius in output pixels) for a soft glow |
| `background` | `#000000` | colour of empty space |
| `width`, `height` | 1400, 700 | size of the PNG written by `jitnodesCli` unless `--size WxH` is given |
| `viewCx`, `viewCy`, `viewWidth` | 0, 0, 0 | the world window the PNG shows (its centre and width; the height follows the aspect ratio). `viewWidth` 0 fits the bounds of the points. The editor ignores these and uses its own pan and zoom |

| Additive (`fxRenderAdditive.json`) | Over (`fxRenderOver.json`) |
|---|---|
| ![three clouds drawn additively](images/fxRenderAdditive.png) | ![the same clouds composited over each other](images/fxRenderOver.png) |

The same three clouds (30,000 soft sprites each, `alpha` 0.06) with `additive` 1 and 0. Additive: light adds up, so the
overlaps go towards white and the order of the sprites does not matter. Over: later sprites cover earlier ones, so the
last merged input (blue) wins. In additive mode the `background` is treated as light too and is brightened by `gamma`
along with everything else, so `#000000` is what a dark-sky picture needs; the over mode draws the colour as it is.

## From points to an image

A point is drawn when it has a `radius` attribute greater than 0 (world units) and `alpha` greater than 0. The sprite
attributes are the ones the plain viewport knows: `Cd.x/y/z` (colour, default white), `alpha` (default 1), `softness`
(0 is a hard disc, 1 fades smoothly from the centre; default 1), `aspect` (width over height) and `shape` (0.5 or more
draws a box). See [wrangle.md](wrangle.md#attribute-names-other-nodes-understand).

- **Additive**: every sprite adds `Cd * alpha * falloff` to a floating-point buffer, so a million faint sprites build up
  smooth gradients that a plain alpha-over viewport (limited to 5,000 sprites) cannot show. The sum is tone mapped
  (see `exposure`), optionally blooms, and is dithered when converted to 8 bits so dark gradients do not band.
- **Sub-pixel sprites** (radius under 0.75 pixel) are spread over the four nearest pixels with their energy kept
  (`alpha * area`), so tiny stars stay visible but dim, and do not flicker when the view moves.
- The picture is drawn for the **current view** in the editor (zoom in and the sprites get bigger and sharper) and is
  cached while nothing changes. Rendering runs on a thread pool, in horizontal bands.
- `writePng` (in `jitnodes/geo/raster.h`) writes the result as a PNG (Sub filter, LZ77 with the fixed Huffman code: about 2 to 6 times smaller than raw pixels, no zlib needed).

## The nebula graph

`examples/graphs/nebula.json` has four branches merged into one `render` node. The reference is a star-field nebula
panorama: dark space, coloured gas in wisps, a hot core, blue streaks and stars.

| Branch | Nodes | What it does |
|---|---|---|
| gas | `generate` 300k and `generate` 4k (tagged `puff = 1`), `merge`, `noise` (fbm), `noise` (ridged), `wrangle`, `warp` (curl), `colorRamp`, `wrangle` | Two layers share one chain, so they line up. The 300k small points carry the structure: two noise fields give the large soft masses (`gas`) and the bright creases (`ridge`); a wrangle combines them with a radial glow into a density and a colour index; the curl warp drags the points into wisps (the attributes ride along); the ramp colours them blue to violet to pink. The 4k large puffs (radius 1.4 to 3 units, very low alpha) fill the base glow smoothly under the fine points. The last wrangle sizes each layer by the `puff` flag, and fades the puffs out where `tone` is low so dark gaps stay empty |
| core | `circle`, `wrangle`, `scatter`, `noise`, `warp`, `wrangle`, `colorRamp`, `wrangle` | A circle bent into a lumpy closed shape, filled with 160k points by `scatter`, then the same treatment in warm colours |
| filaments | `line`, `wrangle`, `scatter`, `noise`, `warp`, `colorRamp`, `wrangle` | A line bent into a wave; `scatter` walks along it with some `spread`, producing the blue streak |
| stars | two `generate` + `wrangle` | 12k stars whose radius is `rand^8`-distributed (many tiny, a few big) and tinted by `rand2`, plus 50k sub-pixel dust points |

Wrangles drive the look because they are the cheap, fully animatable part: all of `alpha`, `radius` and the mixing
constants can be edited live. Animate a `noise` node's `phase` or a `warp` node's `phase` with `time` and the whole
nebula evolves.

## Writing your own look

The recipe the sample follows:

1. `generate` or `scatter` for the points. A bigger cloud is smoother, not slower to author.
2. One or two `noise` nodes for the structure (`fbm` for masses, `ridged` for veins). Use different `scale`, `seed`
   and `offsetX`.
3. A wrangle that turns the fields into a **colour index** (`tone`) and a **density** (`dens`): `clamp`, `lerp` and
   products work well, and `exp(0 - (x * x + y * y) * k)` makes a central glow.
4. `warp` with `mode = curl` to break up the regularity of the noise. It is the most expensive node (about 150 ms for
   500k points with 4 iterations; the gas now has 300k); put it after the structure is decided and before the colour ramp so it only runs once.
5. `colorRamp` on `tone`.
6. A wrangle for `radius`, `alpha` and `softness`. The total light is roughly
   `points per pixel * alpha`, so when a picture is too bright lower `alpha` (and `exposure`) rather than the count.
7. `merge` the branches, add `render`, set `viewWidth`, `exposure`, `bloom`.

Wrangle has no `rand`; use the `rand` and `rand2` attributes of `generate` and `scatter` instead, or `noise` at a high
`scale` for spatially coherent randomness.

## Performance and limits

Measured with `nebula.json` (8 threads, release build, `jitnodesCli --profile`): the curl `warp` of the gas is 150 ms,
the two gas `noise` nodes 35 ms each, `scatter` of the core 16 ms and a `generate` 2 ms. The whole
`jitnodesCli ... --png` run, cooking plus drawing the 646,000 sprites at 1400 x 700 plus writing the PNG, takes about
0.35 s.

- `relax` and `attribBlur` cost points times neighbours within `radius` (per iteration); keep `radius` within a few times the
  point spacing. `attractor` costs points times `iterations` times two trig calls: 400,000 points and 30 iterations take
  40 to 120 ms depending on the map. `wave` is as cheap as a `noise` octave.
- Sprites larger than a few hundred pixels across are slow in the additive renderer (cost is proportional to covered
  pixels); use many small sprites rather than a few huge ones, or `bloom` for large soft glows.
- Noise is 3D gradient noise computed in plain C++ (not JIT compiled), so `noise` and `warp` do not take part in kernel
  fusion; they run on the thread pool.
- `scatter` of a very thin closed shape is slow (rejection sampling); thicken the shape or use an open curve with
  `spread`.
- The `render` settings are detail values on the geometry, so put `render` last: a node that builds a new geometry from
  scratch (`generate`, `scatter`) starts without them.
