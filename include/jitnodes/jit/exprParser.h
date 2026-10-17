#pragma once

#include <string>
#include <vector>

#include "jitnodes/jit/expr.h"

namespace jitnodes {

// Parses a small VEX-like wrangle language into a KernelSpec.
//
//   program   := (assign ';')*
//   assign    := identifier '=' expr
//   expr      := ternary with || && == != < > <= >= + - * / unary - ! and
//                min(a,b) max(a,b) abs(a) sqrt(a) clamp(v,lo,hi) lerp(a,b,t)
//
// Identifiers listed in paramNames read the runtime parameter block; every other identifier is
// a point attribute (x, y, or any custom channel). Statements run in order, so later statements
// see earlier assignments. Throws std::runtime_error with a message on syntax errors.
KernelSpec parseWrangle(const std::string& source, const std::vector<std::string>& paramNames);

}  // namespace jitnodes
