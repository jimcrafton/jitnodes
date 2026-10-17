#include "jitnodes/jit/expr.h"

#include <cstring>

namespace jitnodes {

int ExprGraph::intern(const ExprNode& n) {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const ExprNode& o = nodes_[i];
        if (o.op == n.op && o.a == n.a && o.b == n.b && o.c == n.c && o.index == n.index &&
            std::memcmp(&o.value, &n.value, sizeof(float)) == 0)
            return static_cast<int>(i);
    }
    nodes_.push_back(n);
    return static_cast<int>(nodes_.size() - 1);
}

int ExprGraph::constant(float v) {
    ExprNode n;
    n.op = Op::constant;
    n.value = v;
    return intern(n);
}

int ExprGraph::param(int index) {
    ExprNode n;
    n.op = Op::param;
    n.index = index;
    return intern(n);
}

int ExprGraph::load(int channel) {
    ExprNode n;
    n.op = Op::load;
    n.index = channel;
    return intern(n);
}

int ExprGraph::unary(Op op, int a) {
    ExprNode n;
    n.op = op;
    n.a = a;
    return intern(n);
}

int ExprGraph::binary(Op op, int a, int b) {
    // Canonical operand order for commutative ops improves sharing.
    const bool commutative = op == Op::add || op == Op::mul || op == Op::min || op == Op::max ||
                             op == Op::eq || op == Op::logicalAnd || op == Op::logicalOr;
    if (commutative && a > b) std::swap(a, b);
    ExprNode n;
    n.op = op;
    n.a = a;
    n.b = b;
    return intern(n);
}

int ExprGraph::select(int mask, int whenTrue, int whenFalse) {
    ExprNode n;
    n.op = Op::select;
    n.a = mask;
    n.b = whenTrue;
    n.c = whenFalse;
    return intern(n);
}

int KernelSpec::channelIndex(const std::string& name) {
    for (size_t i = 0; i < channels.size(); ++i)
        if (channels[i] == name) return static_cast<int>(i);
    channels.push_back(name);
    channelTypes.push_back(AttribType::float32);
    return static_cast<int>(channels.size() - 1);
}

std::string KernelSpec::signature() const {
    std::string s;
    for (const ExprNode& n : graph.nodes()) {
        s += std::to_string(static_cast<int>(n.op)) + ',' + std::to_string(n.a) + ',' +
             std::to_string(n.b) + ',' + std::to_string(n.c) + ',' + std::to_string(n.index) + ',';
        uint32_t bits;
        std::memcpy(&bits, &n.value, sizeof(bits));
        s += std::to_string(bits) + ';';
    }
    s += '|';
    for (const auto& [channel, expr] : stores) s += std::to_string(channel) + ':' + std::to_string(expr) + ',';
    for (AttribType t : channelTypes) s += static_cast<char>('0' + static_cast<int>(t));
    s += '|' + std::to_string(channels.size()) + '|' + std::to_string(paramCount);
    return s;
}

}  // namespace jitnodes
