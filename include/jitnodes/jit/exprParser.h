#pragma once

#include <map>
#include <string>
#include <vector>

#include "jitnodes/jit/expr.h"

namespace jitnodes {

// A parameter declared in wrangle code: `| gain = 1.5, radius = 2(0, 10), count(1,) |` (the default, 0 when left out, then the minimum and
// the maximum in parentheses, either of which may be empty). `slot` is the index in the kernel's parameter block, which is not the
// declaration's position because string literals take slots too.
struct WrangleParam {
    std::string name;
    float value = 0.0f;
    bool hasMinimum = false;
    bool hasMaximum = false;
    float minimum = 0.0f;
    float maximum = 0.0f;
    int slot = 0;
};

// Parses a small VEX-like wrangle language into a KernelSpec.
//
//   program   := (assign ';')*
//   assign    := identifier '=' expr
//   expr      := ternary with || && == != < > <= >= + - * / unary - ! and
//                min(a,b) max(a,b) abs(a) sqrt(a) clamp(v,lo,hi) lerp(a,b,t)
//                sin cos exp log floor ceil pow(a,b)
//
// A string attribute can be compared with a literal on either side of == and !=: `label == "tree"`, `"rock" != kind`. Nothing else
// takes a string (see KernelSpec::StringParam, and docs/wrangle.md).
//
// Identifiers listed in paramNames read the runtime parameter block; identifiers listed in constants
// compile to that value (the kernel is specific to it); `sampleIndex` is the element's position in the
// run; a name starting with a single '_' is a temporary local to the kernel; every other identifier is an attribute channel (x, y, N.x, group.top, ...). Statements run in
// order, so later statements see earlier assignments. A '#' starts a comment that runs to the end of the line. Throws std::runtime_error
// with a message on syntax errors.
//
// With `declared` given, a list `| name [= value] [(min, max)], ... |` before the first assignment declares runtime parameters: each name
// reads like a, b, c, d (it is appended to `declared` in order, and cannot be assigned). Without `declared` (parameter expressions,
// transform) a '|' is not part of the language.
KernelSpec parseWrangle(const std::string& source, const std::vector<std::string>& paramNames,
                        const std::map<std::string, float>& constants = {}, std::vector<WrangleParam>* declared = nullptr);

}  // namespace jitnodes
