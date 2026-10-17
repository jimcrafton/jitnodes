#pragma once

#include <functional>
#include <string>

#include "jitnodes/jit/expr.h"

namespace jitnodes {

// Evaluates the first store of a spec once, resolving every channel read as a named scalar symbol.
// Used for parameter expressions.
float evaluateScalar(const KernelSpec& spec, const std::function<float(const std::string&)>& resolveSymbol);

}  // namespace jitnodes
