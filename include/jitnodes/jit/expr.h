#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
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
    sin,     // sin/cos/exp/log/pow: JIT uses single-precision polynomial approximations, see docs/design.md
    cos,
    exp,
    log,
    pow,
    floor,
    ceil,
    index,  // element position within the run, as a float (exact below 2^24)
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
    int index();
    int unary(Op op, int a);
    int binary(Op op, int a, int b);
    int select(int mask, int whenTrue, int whenFalse);

    const std::vector<ExprNode>& nodes() const { return nodes_; }
    size_t size() const { return nodes_.size(); }

private:
    int intern(const ExprNode& n);

    // Identity of a node for hash-consing: every field, with the constant compared by its bits.
    struct Key {
        Op op;
        int a, b, c, index;
        uint32_t valueBits;
        bool operator==(const Key& o) const { return op == o.op && a == o.a && b == o.b && c == o.c && index == o.index && valueBits == o.valueBits; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            uint64_t h = static_cast<uint64_t>(k.op);
            for (uint64_t v : {static_cast<uint64_t>(static_cast<uint32_t>(k.a)), static_cast<uint64_t>(static_cast<uint32_t>(k.b)),
                               static_cast<uint64_t>(static_cast<uint32_t>(k.c)), static_cast<uint64_t>(static_cast<uint32_t>(k.index)),
                               static_cast<uint64_t>(k.valueBits)})
                h = (h ^ v) * 1099511628211ull + 0x9E3779B97F4A7C15ull;
            return static_cast<size_t>(h ^ (h >> 29));
        }
    };

    std::vector<ExprNode> nodes_;
    std::unordered_map<Key, int, KeyHash> index_;  // Key -> position in nodes_, so interning is O(1) instead of a scan
};

// A compilable unit: an expression graph plus which channels it reads and which it overwrites.
struct KernelSpec {
    ExprGraph graph;
    std::vector<std::string> channels;       // channel index -> attribute name
    std::vector<AttribType> channelTypes;    // parallel to channels; int channels convert at load/store
    std::vector<std::pair<int, int>> stores; // (channel index, expression node) written back
    int paramCount = 0;

    // A literal compared with a string attribute (`label == "tree"`) is a parameter whose value is the literal's id in that attribute's
    // table, which only the geometry knows: resolveStringParams fills the slot before every run. It is not part of signature(), because the
    // compiled code is the same whatever the text is.
    struct StringParam {
        int paramIndex;
        int channel;
        std::string text;
    };
    std::vector<StringParam> stringParams;

    int channelIndex(const std::string& name);  // adds the channel when new

    // Canonical text of the spec (structure, constants, channel types and names); used as the kernel cache key.
    std::string signature() const;
};

}  // namespace jitnodes
