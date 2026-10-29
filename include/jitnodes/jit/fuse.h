#pragma once

#include <functional>
#include <string>
#include <vector>

#include "jitnodes/jit/expr.h"

namespace jitnodes {

// Sequential composition: the result behaves like running `first` and then `second` on the same
// points, with intermediate values staying in the expression graph. Parameters are concatenated
// (first's, then second's).
//
// An int attribute truncates toward zero when it is stored, so a value `first` writes into one must be truncated the same
// way before `second` reads it, or fused and unfused cooking would disagree. `channelType` tells which type the attribute of a
// given name has (from the geometry the chain runs on); without it every channel is taken to be float.
KernelSpec composeKernels(const KernelSpec& first, const KernelSpec& second,
                          const std::function<AttribType(const std::string&)>& channelType = {});

// In-place composeKernels: appends `next` to `acc`. Linear in the size of `next` instead of the accumulated graph, so a chain of
// k nodes is composed in O(k) rather than O(k^2).
void appendKernel(KernelSpec& acc, const KernelSpec& next,
                  const std::function<AttribType(const std::string&)>& channelType = {});

// Parallel composition: every branch runs on its own copy of the channels (branch i's channel `n` becomes `i.n`, a name no wrangle
// can produce), and the result stores the sum of the branches' final `channel` into `channel`. Parameters are concatenated in branch
// order. A branch that never writes `channel` contributes the value it loads.
KernelSpec mixKernels(const std::vector<KernelSpec>& branches, const std::string& channel);

}  // namespace jitnodes
