#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "jitnodes/geo/attribute.h"

namespace jitnodes {

// Per-element operations of the kernel IR. Values are 32-bit floats; comparison results are
// masks (all ones / all zeros per lane) so conditionals compile to branchless selects.
enum class Op : uint8_t {
    constant,
    param,  // runtime parameter: read from the parameter block, no recompile when it changes
    load,   // point attribute channel
    add,
    sub,
    mul,
    div,
    min,
    max,
    neg,
    abs,
    sqrt,
    lt,
    gt,
    le,
    ge,
    eq,
    logicalAnd,
    logicalOr,
    logicalNot,
    select,  // select(mask, whenTrue, whenFalse)
    sin,     // sin/cos/exp/log/pow are interpreter-only (no vector JIT yet)
    cos,
    exp,
    log,
    pow,
    floor,
    ceil,
};

struct ExprNode {
    Op op = Op::constant;
    int a = -1;
    int b = -1;
    int c = -1;
    int index = 0;      // channel index for load, parameter index for param
    float value = 0.0f; // constant value
};

// Hash-consed expression DAG: structurally identical subexpressions share one node, which gives
// common subexpression elimination for free, and operands always precede their users.
class ExprGraph {
public:
    int constant(float v);
    int param(int index);
    int load(int channel);
    int unary(Op op, int a);
    int binary(Op op, int a, int b);
    int select(int mask, int whenTrue, int whenFalse);

    const std::vector<ExprNode>& nodes() const { return nodes_; }
    size_t size() const { return nodes_.size(); }

private:
    int intern(const ExprNode& n);

    std::vector<ExprNode> nodes_;
};

// A compilable unit: an expression graph plus which channels it reads and which it overwrites.
struct KernelSpec {
    ExprGraph graph;
    std::vector<std::string> channels;       // channel index -> attribute name
    std::vector<AttribType> channelTypes;    // parallel to channels; int channels convert at load/store
    std::vector<std::pair<int, int>> stores; // (channel index, expression node) written back
    int paramCount = 0;

    int channelIndex(const std::string& name);  // adds the channel when new

    // Canonical text of the spec; used as the kernel cache key.
    std::string signature() const;
};

}  // namespace jitnodes
