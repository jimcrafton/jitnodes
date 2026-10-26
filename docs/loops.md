# Loops: foreach and solver

Both nodes are subnets. Double-click to enter, put the work between a `subnetInput` and the display node, and the node runs that child
graph several times. Nothing is special about the graph inside, so wrangles, noise, nested loops and assets all work there.

## foreach (one input)

Runs the child graph once per piece of the input and merges the results in order (`merge` rules: attributes missing on one side are zero
filled, the output primitives follow the pieces).

| parameter | meaning |
|---|---|
| `mode` | `primitives` (default), `points`, `copies` or `feedback` |
| `pieceAttrib` | `primitives` only: a primitive attribute (float, int or string); primitives with equal values form one piece, pieces in order of first appearance. Empty = one piece per primitive |
| `count` | `copies` and `feedback`: number of iterations |
| `parallel` | `primitives`, `points` and `copies`: 1 cooks the pieces on all threads (default 0, see below) |
| `iteration`, `numIterations` | set while the node cooks (zero afterwards); read inside as `parent.iteration` and `parent.numIterations` |

- `primitives`: subnetInput 0 is the piece, with its points, attributes and the input's detail values. Points that belong to no primitive
  are dropped. Curves stay curves.
- `points`: every point is a piece (no primitives).
- `copies`: subnetInput 0 is the whole input every time; only `parent.iteration` differs. The results are merged.
- `feedback`: the result of one iteration is subnetInput 0 of the next, and subnetInput 1 is always the original input. The output is
  the last result, or the input when `count` is 0.

The iteration count is limited to 100000. A `foreach` with many tiny pieces is dominated by per-node overhead; for a per-point formula a
`wrangle` is much cheaper. `feedback` is always serial, since each iteration needs the previous result.

By default the iterations run one after the other in the node's own child graph. With `parallel` on, every thread gets a private copy of
the child graph (made from its saved text, so `parent.` parameters and expressions work the same) and takes pieces as they come; the
result is identical to the serial one, merged in piece order. The copies are kept between cooks and rebuilt when the child graph or a
promoted parameter changes. Use it when each piece is real work (a nested simulation, a heavy chain). Two things differ from serial mode:
state that a node inside keeps between cooks (a `solver`, `particles`) is per copy, so it should not be relied on across iterations; and
a child graph that reads other nodes of the outer graph by name does not resolve them in a copy. With one thread, or one piece, it falls
back to serial.

## solver (one input)

Carries a state from step to step, so a graph built from ordinary nodes can be a simulation. The input is the state at `startTime`; the
child graph runs `stepsPerSecond` times a second of playhead time, and its result is the state of the next step. Inside:

- subnetInput 0 is the previous state, subnetInput 1 the original input (for values that should not drift, such as rest positions);
- `parent.step` is the step number (1 for the first step) and `parent.dt` the step length in seconds;
- `time` and `frame` are the time of the step, not of the playhead, so keyframes and `time` expressions inside follow the simulation.

Like `particles`, the node keeps the latest state: playing forward runs only the new steps, scrubbing back to before the kept step, or
changing the input, a parameter or anything inside the child graph, restarts from the input. The same time always gives the same result.
At or before `startTime` the output is the input. The step count is limited to 100000. Cooking is serial and the child graph is
compared by its saved text on every cook, so keep solver graphs modest; the heavy simulations are dedicated nodes (`particles`, `fluid`,
`gasField`).

An example, drift with a changing wind: input points; inside `subnetInput` -> `wrangle` with code `x = x + a;` and the expression
`0.05 * cos(time)` on `a`.

## Examples

- `examples/graphs/loopsForeach.json`: a ring copied 12 times around a circle, each copy scaled by `parent.iteration` (`copies` mode,
  `parallel` on). `jitnodesCli examples/graphs/loopsForeach.json --svg loopsForeach.svg`
- `examples/graphs/loopsSolver.json`: a ring that spirals inwards while a time-varying wind pushes it sideways.
  `jitnodesCli examples/graphs/loopsSolver.json --time 3 --svg loopsSolver.svg`
