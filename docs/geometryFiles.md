# Geometry files (`.jngeo`)

A `.jngeo` file stores one cooked `Geometry`: points, primitives, point and primitive attributes (float and int) and detail
values. It exists so an expensive cook (`reactionDiffusion`, `boids`, see "Load and cook" in `performance.md`) can be written
once and read back at memory bandwidth. Graphs stay JSON5; only geometry is binary. The plan and the node/CLI steps that build
on the format are in `design.md` ("Binary geometry files").

```cpp
#include "jitnodes/geo/geoFile.h"
std::vector<uint8_t> bytes = jitnodes::saveGeometry(geo);
jitnodes::Geometry back = jitnodes::loadGeometry(bytes.data(), bytes.size());
jitnodes::saveGeometryToFile(geo, "sim.jngeo");   // writes sim.jngeo.tmp, then renames
jitnodes::Geometry fromDisk = jitnodes::loadGeometryFromFile("sim.jngeo");
```

Loading throws `std::runtime_error` ("invalid .jngeo file: ...") for anything that is not a complete, in-bounds file. Floats are
copied bit for bit (NaN payloads and -0 included), and saving the loaded geometry gives the same bytes.

## Nodes
Both nodes are in `src/graph/fileNodes.cpp` and are palette buttons in the editor (`fileRead`, `fileCache`).

| Node | Inputs | Parameters | Behaviour |
|---|---|---|---|
| `fileRead` | 0 | `path` | Loads the file. A missing or corrupt file is a cook error that names the node and the path. |
| `fileCache` | 1 | `path`, `mode` (`auto`, `read`, `write`; default `auto`) | Passes its input through. `write` cooks the input and saves it. `read` loads the file and **never cooks the input**, which need not even be connected. `auto` reads the file when it exists and loads cleanly, otherwise cooks the input and saves it. |

`$F` in a path is the frame number, `round(time * fps)`, and `$F4` pads it with zeros to 4 digits (any width). A path with a
token makes the node depend on the playhead, so a baked sequence `sim_$F4.jngeo` plays back as the playhead moves; a path without
one does not. Relative paths are relative to the process's working directory.

The saving is real only because skipping is real: `Node::needsInputs()` (asked while planning a cook) returns false for a
`fileCache` that will read, so upstream nodes are never planned, cooked or required to be wired. The file does not record what
produced it. If you change the upstream graph, delete the file or switch to `write` for one cook; `auto` cannot tell it is stale.

## Tools
- `jitnodesCli graph.json --geo-out sim.jngeo [--node NAME] [--time T]` writes the cooked geometry.
- `jitnodesCli --geo-in sim.jngeo --png out.png` (or `--svg`) renders a file without a graph; the image is identical to
  rendering the cook it came from, because `render.*` details are saved too.
- The editor's viewport menu (right click) has **Save geometry...**, which writes the displayed geometry.

## Layout (little endian, version 1.0)
```
Header (64 bytes)  0 magic "JNGEO1\0\0"   8 u32 version (major << 16 | minor)   12 u32 0x01020304   16 u32 flags (0)
                   20 u32 sectionCount     24 u64 fileSize    32 u64 XXH64 of bytes [64, fileSize)   40..63 reserved (0)
Section table      {u32 id, u32 flags (0), u64 offset, u64 byteSize} per section, offsets 64-byte aligned
1 strings          u32 count, u32 offsets[count + 1], UTF-8 bytes (attribute and detail names)
2 counts           u64 pointCount, u64 primitiveCount
3 prims            u64 count, then {u32 start, u32 count, u8 closed, u8 kind, 2 pad} each
4 attribs          u64 count, then {u32 nameIndex, u8 class, u8 type, 2 pad, u64 count, u64 dataOffset, u64 dataBytes} each
5 details          u64 count, then {u32 nameIndex, f32 value} each
6 payload          every attribute as stored in memory: padToLanes(count) elements, zero padded, 64-byte aligned
```
`dataOffset` is relative to the start of section 6. Attributes are listed point class first, each class in name order, so the file
is deterministic.

## What the loader checks
Checksum, magic, byte-order marker, exact file size, zero flags and reserved bytes, every section inside the file and aligned,
no duplicate sections, string offsets in order, primitive and attribute counts matching `counts`, primitives inside the point
range, attribute blocks inside the payload and not overlapping (so a small file cannot make many attributes share one large
block), no duplicate attribute names, float point attributes `x` and `y` present (which also bounds `pointCount` by the file
size). Unknown section ids are skipped and a newer minor version loads; a different major version is an error.
Tests: `tests/testMain.cpp` (`geoFile*`) round-trip bit for bit, reject every truncation and every flipped byte, and load
thousands of mutations that keep a valid checksum (they must throw or give a consistent geometry); the whole set also runs clean
under the `x64-asan` preset.
