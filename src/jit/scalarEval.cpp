#include "jitnodes/jit/scalarEval.h"

#include <stdexcept>

#include "jitnodes/jit/opEval.h"

namespace jitnodes {

float evaluateScalar(const KernelSpec& spec, const std::function<float(const std::string&)>& resolveSymbol) {
    if (spec.stores.empty()) throw std::runtime_error("expression has no result");
    const auto& nodes = spec.graph.nodes();
    const size_t root = static_cast<size_t>(spec.stores.front().second);
    std::vector<float> v(root + 1, 0.0f);
    for (size_t i = 0; i <= root; ++i) {
        const ExprNode& n = nodes[i];
        auto operand = [&](int idx) { return idx >= 0 ? v[static_cast<size_t>(idx)] : 0.0f; };
        switch (n.op) {
            case Op::constant: v[i] = n.value; break;
            case Op::param: v[i] = 0.0f; break;
            case Op::load: v[i] = resolveSymbol(spec.channels[static_cast<size_t>(n.index)]); break;
            default: v[i] = evalOp(n.op, operand(n.a), operand(n.b), operand(n.c)); break;
        }
    }
    return v[root];
}

}  // namespace jitnodes
