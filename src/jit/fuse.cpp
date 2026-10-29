#include "jitnodes/jit/fuse.h"

#include <map>
#include <stdexcept>

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
            case Op::index: map[i] = dst.index(); break;
            case Op::select: map[i] = dst.select(m(n.a), m(n.b), m(n.c)); break;
            case Op::neg:
            case Op::abs:
            case Op::sqrt:
            case Op::logicalNot:
            case Op::sin:
            case Op::cos:
            case Op::exp:
            case Op::log:
            case Op::floor:
            case Op::ceil: map[i] = dst.unary(n.op, m(n.a)); break;
            default: map[i] = dst.binary(n.op, m(n.a), m(n.b)); break;
        }
    }
    return map;
}

// What storing `value` into an int32 attribute and loading it back gives: truncation toward zero, and INT_MIN for NaN and for
// values outside the int range (the semantics of cvttps2dq and of the interpreter's toInt32).
int storedAsInt(ExprGraph& g, int value) {
    const int zero = g.constant(0.0f);
    const int low = g.constant(-2147483648.0f);
    const int high = g.constant(2147483648.0f);
    const int truncated = g.select(g.binary(Op::ge, value, zero), g.unary(Op::floor, value), g.unary(Op::ceil, value));
    const int inRange = g.binary(Op::logicalAnd, g.binary(Op::gt, value, low), g.binary(Op::lt, value, high));
    return g.select(inRange, truncated, low);
}

}  // namespace

void appendKernel(KernelSpec& acc, const KernelSpec& next, const std::function<AttribType(const std::string&)>& channelType) {
    const int paramOffset = acc.paramCount;
    acc.paramCount += next.paramCount;

    std::vector<int> nextChannels;
    for (size_t i = 0; i < next.channels.size(); ++i) {
        int c = acc.channelIndex(next.channels[i]);
        if (next.channelTypes[i] != AttribType::float32) acc.channelTypes[static_cast<size_t>(c)] = next.channelTypes[i];
        nextChannels.push_back(c);
    }

    std::map<int, int> produced(acc.stores.begin(), acc.stores.end());  // output channel -> latest expression

    std::map<int, int> override;  // next's channel index -> expression produced earlier
    for (size_t c = 0; c < next.channels.size(); ++c) {
        auto it = produced.find(nextChannels[c]);
        if (it == produced.end()) continue;
        const bool isInt = isIntegerStorage(channelType ? channelType(next.channels[c]) : next.channelTypes[c]);
        override[static_cast<int>(c)] = isInt ? storedAsInt(acc.graph, it->second) : it->second;
    }
    std::vector<int> nextMap = copyGraph(acc.graph, next, nextChannels, paramOffset, override);
    for (const auto& [channel, expr] : next.stores) produced[nextChannels[static_cast<size_t>(channel)]] = nextMap[static_cast<size_t>(expr)];

    acc.stores.assign(produced.begin(), produced.end());
    for (const KernelSpec::StringParam& sp : next.stringParams)
        acc.stringParams.push_back({sp.paramIndex + paramOffset, nextChannels[static_cast<size_t>(sp.channel)], sp.text});
}

KernelSpec composeKernels(const KernelSpec& first, const KernelSpec& second, const std::function<AttribType(const std::string&)>& channelType) {
    KernelSpec out;
    out.paramCount = first.paramCount;
    std::vector<int> firstChannels;
    for (size_t i = 0; i < first.channels.size(); ++i) {
        int c = out.channelIndex(first.channels[i]);
        if (first.channelTypes[i] != AttribType::float32) out.channelTypes[static_cast<size_t>(c)] = first.channelTypes[i];
        firstChannels.push_back(c);
    }
    std::vector<int> firstMap = copyGraph(out.graph, first, firstChannels, 0, {});
    for (const auto& [channel, expr] : first.stores) out.stores.emplace_back(firstChannels[static_cast<size_t>(channel)], firstMap[static_cast<size_t>(expr)]);
    std::map<int, int> sorted(out.stores.begin(), out.stores.end());
    out.stores.assign(sorted.begin(), sorted.end());
    for (const KernelSpec::StringParam& sp : first.stringParams)
        out.stringParams.push_back({sp.paramIndex, firstChannels[static_cast<size_t>(sp.channel)], sp.text});
    appendKernel(out, second, channelType);
    return out;
}

KernelSpec mixKernels(const std::vector<KernelSpec>& branches, const std::string& channel) {
    if (branches.empty()) throw std::invalid_argument("mixKernels: no branches");
    KernelSpec out;
    int sum = -1;
    for (size_t i = 0; i < branches.size(); ++i) {
        const KernelSpec& branch = branches[i];
        const std::string prefix = std::to_string(i) + ".";
        const int paramOffset = out.paramCount;
        out.paramCount += branch.paramCount;

        std::vector<int> channels;
        int own = -1;
        for (size_t c = 0; c < branch.channels.size(); ++c) {
            const int mapped = out.channelIndex(prefix + branch.channels[c]);
            if (branch.channelTypes[c] != AttribType::float32) out.channelTypes[static_cast<size_t>(mapped)] = branch.channelTypes[c];
            channels.push_back(mapped);
            if (branch.channels[c] == channel) own = static_cast<int>(c);
        }
        const std::vector<int> map = copyGraph(out.graph, branch, channels, paramOffset, {});

        int value = -1;
        if (own >= 0)
            for (const auto& [stored, expr] : branch.stores)
                if (stored == own) value = map[static_cast<size_t>(expr)];
        if (value < 0) value = out.graph.load(own >= 0 ? channels[static_cast<size_t>(own)] : out.channelIndex(prefix + channel));
        sum = sum < 0 ? value : out.graph.binary(Op::add, sum, value);

        for (const KernelSpec::StringParam& sp : branch.stringParams)
            out.stringParams.push_back({sp.paramIndex + paramOffset, channels[static_cast<size_t>(sp.channel)], sp.text});
    }
    out.stores.emplace_back(out.channelIndex(channel), sum);
    return out;
}

}  // namespace jitnodes
