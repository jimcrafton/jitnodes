# Generating points and drawing pictures with them

Six nodes turn jitnodes into a small particle-image tool: two make points, three shape them with noise and colour, and
one (`render`) says how the points become an image. Together they draw pictures such as the nebula in
`examples/graphs/nebula.json` (about 650,000 sprites; cooking and drawing it takes about 0.35 s).

```
jitnodesCli examples/graphs/nebula.json --png nebula.png
jitnodesEditor examples/graphs/nebula.json
```

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

### warp (one input)

Moves the points through a noise field. Attributes (including ones computed by `noise` earlier) travel with the points,
so the pattern is carried into the new shapes.

| Parameter | Default | Meaning |
|---|---|---|
| `mode` | `noise` | `noise`: offset by two independent noise values (a domain warp, gives billowy distortion). `curl`: follow the curl of a noise potential, a flow that neither compresses nor stretches, which pulls clouds of points into swirls and wisps |
| `amount` | 1 | distance moved, in world units per unit of noise slope (`curl`) or per noise value (`noise`), over all iterations |
| `scale`, `octaves`, `lacunarity`, `gain`, `seed`, `phase` | as in `noise` | the field that moves the points |
| `iterations` | 1 | the move is split in this many steps and the field is re-sampled at each new position (and phase); more steps give longer, more curled streaks. 1 to 64 |

### colorRamp (one input)

Maps a float attribute onto a gradient and writes `Cd.x`, `Cd.y`, `Cd.z`.

| Parameter | Default | Meaning |
|---|---|---|
| `attribute` | `noise` | point attribute to read (float or int) |
| `ramp` | `0 #000000, 1 #ffffff` | stops separated by commas, semicolons or new lines; each is a position then a colour, either `#rgb`, `#rrggbb` or three numbers 0..1 (`0.5 1 0.2 0.1`). Colours are interpolated linearly in RGB; positions outside the stops clamp to the end colours |
| `min`, `max` | 0, 1 | attribute values that map to ramp positions 0 and 1 (an inverted range reverses the ramp) |
| `gamma` | 1 | the position is raised to this power first; above 1 favours the dark end |

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
- `writePng` (in `jitnodes/geo/raster.h`) writes the result as an uncompressed PNG.

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

- Sprites larger than a few hundred pixels across are slow in the additive renderer (cost is proportional to covered
  pixels); use many small sprites rather than a few huge ones, or `bloom` for large soft glows.
- Noise is 3D gradient noise computed in plain C++ (not JIT compiled), so `noise` and `warp` do not take part in kernel
  fusion; they run on the thread pool.
- `scatter` of a very thin closed shape is slow (rejection sampling); thicken the shape or use an open curve with
  `spread`.
- The `render` settings are detail values on the geometry, so put `render` last: a node that builds a new geometry from
  scratch (`generate`, `scatter`) starts without them.
