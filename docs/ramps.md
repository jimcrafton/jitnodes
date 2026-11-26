# Ramps

A ramp is a float curve over a position: a list of points, each with a position, a value and an interpolation, as in Houdini's float ramp
parameter. `Ramp` (`graph/ramp.h`) holds it; a node keeps one in a **ramp parameter**.

## The text form

Points are separated by commas, semicolons or new lines. Each point is `position value [interpolation]`:

```
0 0 monotone, 0.3 0.9 monotone, 1 1 linear
```

- Points are sorted by position. Positions may lie outside 0..1. Outside the first and last point the end values hold.
- The interpolation belongs to the point it leaves, and runs to the next point (the last point's is not used). Omitted: `linear`.
- `constant` holds the value until the next point; `linear`; `smooth` (smoothstep, flat at both points); `catmullRom` (a cubic through the points with
  finite-difference tangents, may overshoot); `monotone` (a cubic that never overshoots the data, so a rising ramp stays rising). Names ignore case and `-`.
- Two points at one position make a step.
- Text that is not a ramp is an error that names the point (`ramp: point 2 ('0.5 oops') is not two numbers`). `Ramp::check(text)` returns it as a string for
  an editor to show before storing the value.

## Ramp parameters

A node declares one with `declareRamp(name, defaultText)`; it is a string parameter, so it is saved, undone, copied, embedded in asset files and
reached from Python as that string. `Node::isRampParam(name)` and `rampNames()` tell an editor to show a curve instead of a text field, and
`getRamp(name)` parses it (throwing the node's name and the parameter with the reason when the text is bad). A bad text is stored as typed and
fails the cook, not the edit.

`Node::paramKind(name)` says what kind of value any parameter is: `scalar`, `number`, `color`, `choice`, `ramp`, `text` or `none` (no such
parameter). An editor lets only a `scalar` be changed by dragging. A float is a scalar unless its tags say otherwise: `kind:color`,
`choices:...`, or another `kind:` than `kind:scalar`, which makes it a `number` (typed only).

A subnet or asset can have ramps of its own (spare ramps: `addSpareRamp`, `removeSpareRamp`, `renameSpareRamp`; `AssetParam::ramp`). A
ramp parameter inside it whose text is `parent.<name>` follows that ramp (`rampText` gives the followed text; `renameSpareParameter`
rewrites such references). A wrangle declares and reads ramps with `ramp()` (see [wrangle.md](wrangle.md)).

## Nodes that use ramps

### remap (one input)

Maps a float point attribute through a curve: `u = (attribute - inMin) / (inMax - inMin)`, then `ramp(u)`, written as `outMin + value * (outMax - outMin)`.

| Parameter | Default | Meaning |
|---|---|---|
| `attribute` | `pressure` | the point attribute to read (float or int; an error when missing) |
| `output` | `w` | where to write the result (always a float); empty overwrites `attribute` |
| `ramp` | `0 0 linear, 1 1 linear` | the curve |
| `inMin`, `inMax` | 0, 1 | the attribute values that map to ramp positions 0 and 1 (they must differ; inverted reverses the direction) |
| `outMin`, `outMax` | 0, 1 | what ramp values 0 and 1 become |

Values beyond `inMin` and `inMax` clamp to the ramp's end values. A pressure curve for a brush is `remap pressure -> w`; speed to width is
`remap speed -> w` with `inMax` set to the speed that should count as fast.

### oscillator

`waveform = ramp` uses the oscillator's `ramp` parameter as the wave: the curve over one cycle, positions 0..1 (default `0 -1 smooth, 0.5 1 smooth, 1 -1 smooth`).
Amplitude and offset apply to it like to the other waves.
