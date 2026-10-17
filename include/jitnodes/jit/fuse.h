#pragma once

#include "jitnodes/jit/expr.h"

namespace jitnodes {

// Sequential composition: the result behaves like running `first` and then `second` on the same
// points, with intermediate values staying in the expression graph. Parameters are concatenated
// (first's, then second's).
KernelSpec composeKernels(const KernelSpec& first, const KernelSpec& second);

}  // namespace jitnodes
