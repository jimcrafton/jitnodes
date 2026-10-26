# String attributes

A string attribute (`AttribType::string`) holds text per point or primitive. The data are int32 ids into a per-attribute table
(`StringTable`, id 0 is always `""`), shared copy-on-write like the data, so copying, merging and kernels cost what int attributes
cost. Merging geometries whose tables differ remaps the ids; saving keeps the text (`geometryFiles.md`).

## Making and using them
| Node | Parameters | Result |
|---|---|---|
| `attribCreate` | `type = string`, `text`, `name`, `class` | every element gets `text` |
| `attribString` | `name`, `class`, `source` | creates or overwrites a string attribute |
| `groupByString` | `class`, `attrib`, `match`, `text`, `group` | group `group.<group>` of elements whose text matches |

`attribString` sources:
- `pattern`: `text` with `$P` replaced by the element index (`"pt$P"` gives `pt0`, `pt1`, ...). Every element gets its own string,
  so the table grows with the element count.
- `ints`: the int attribute `from` selects a name from the comma separated list `names` (`"rock, tree, bush"`: 0 is `rock`, 1 is
  `tree`); values outside the list give `""`. Names are trimmed.
- `group`: elements in group `group` get `text`; the others keep what they had (`""` for a new attribute).

`groupByString` `match` is `equals`, `startsWith` or `contains` (case sensitive). An empty text matches only empty strings for
`equals` and everything for `startsWith` and `contains`.

A wrangle compares a string attribute with a literal (`label == "tree"`, see `wrangle.md`) but cannot assign it; use the nodes above
to make the text and a wrangle to act on it.

## SVG export
A primitive string attribute named `id` or `class` becomes the `id` / `class` of that primitive's SVG element (XML escaped, empty
text left out), for example `<polyline id="road7" class="main" points="...">`.

Point attributes are not exported. `attribString` and `groupByString` (palette label `groupByStr`) have palette buttons.
