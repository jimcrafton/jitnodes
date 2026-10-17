#include "jitnodes/jit/fuse.h"

#include <map>

namespace jitnodes {

namespace {

std::vector<int> copyGraph(ExprGraph& dst, const KernelSpec& src, const std::vector<int>& channelMap, int paramOffset,
                           const std::map<int, int>& loadOverride) {
    const auto& nodes = src.graph.nodes();
    std::vector<int> map(nodes.size(), -1);
    for (size_t i = 0; i < nodes.size(); ++i) {
        const ExprNode& n = nodes[i];
        auto m = [&](int operand) { return map[static_cast<size_t>(operand)]; };
        switch (n.op) {
            case Op::constant: map[i] = dst.constant(n.value); break;
            case Op::param: map[i] = dst.param(n.index + paramOffset); break;
            case Op::load: {
                auto it = loadOverride.find(n.index);
                map[i] = it != loadOverride.end() ? it->second : dst.load(channelMap[static_cast<size_t>(n.index)]);
                break;
            }
            case Op::select: map[i] = dst.select(m(n.a), m(n.b), m(n.c)); break;
            case Op::neg:
            case Op::abs:
            case Op::sqrt:
            case Op::logicalNot: map[i] = dst.unary(n.op, m(n.a)); break;
            default: map[i] = dst.binary(n.op, m(n.a), m(n.b)); break;
        }
    }
    return map;
}

}  // namespace

KernelSpec composeKernels(const KernelSpec& first, const KernelSpec& second) {
    KernelSpec out;
    out.paramCount = first.paramCount + second.paramCount;

    std::vector<int> firstChannels, secondChannels;
    for (const auto& name : first.channels) firstChannels.push_back(out.channelIndex(name));
    for (const auto& name : second.channels) secondChannels.push_back(out.channelIndex(name));

    std::vector<int> firstMap = copyGraph(out.graph, first, firstChannels, 0, {});
    std::map<int, int> produced;  // output channel -> latest expression
    for (const auto& [channel, expr] : first.stores) produced[firstChannels[static_cast<size_t>(channel)]] = firstMap[static_cast<size_t>(expr)];

    std::map<int, int> override;  // second's channel index -> expression produced by first
    for (size_t c = 0; c < second.channels.size(); ++c) {
        auto it = produced.find(secondChannels[c]);
        if (it != produced.end()) override[static_cast<int>(c)] = it->second;
    }
    std::vector<int> secondMap = copyGraph(out.graph, second, secondChannels, first.paramCount, override);
    for (const auto& [channel, expr] : second.stores) produced[secondChannels[static_cast<size_t>(channel)]] = secondMap[static_cast<size_t>(expr)];

    for (const auto& [channel, expr] : produced) out.stores.emplace_back(channel, expr);
    return out;
}

}  // namespace jitnodes
