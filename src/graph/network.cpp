#include "jitnodes/graph/network.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>

#include "jitnodes/jit/fuse.h"
#include "jitnodes/jit/scalarEval.h"

namespace jitnodes {

NodeRegistry& NodeRegistry::instance() {
    static NodeRegistry registry;
    return registry;
}

void NodeRegistry::registerType(const std::string& typeName, Creator creator) {
    creators_[typeName] = std::move(creator);
}

std::unique_ptr<Node> NodeRegistry::create(const std::string& typeName) const {
    auto it = creators_.find(typeName);
    if (it == creators_.end()) throw std::runtime_error("unknown node type '" + typeName + "'");
    return it->second();
}

std::vector<std::string> NodeRegistry::typeNames() const {
    std::vector<std::string> names;
    for (const auto& entry : creators_) names.push_back(entry.first);
    return names;
}

Network::Network(size_t threads) {
    const unsigned hardware = std::thread::hardware_concurrency();
    const size_t workers = threads ? threads - 1 : (hardware > 1 ? std::min<size_t>(hardware - 1, 7) : 0);
    pool_ = std::make_unique<ThreadPool>(workers);
    static std::once_flag once;
    std::call_once(once, registerBuiltinNodes);
}

Network::Slot& Network::slot(NodeId id) {
    auto it = slots_.find(id);
    if (it == slots_.end()) throw std::runtime_error("invalid node id " + std::to_string(id));
    return it->second;
}

const Network::Slot& Network::slot(NodeId id) const {
    auto it = slots_.find(id);
    if (it == slots_.end()) throw std::runtime_error("invalid node id " + std::to_string(id));
    return it->second;
}

NodeId Network::addNode(const std::string& typeName, const std::string& name) {
    const NodeId id = nextId_;
    return addNodeWithId(id, typeName, name.empty() ? typeName + std::to_string(id) : name);
}

NodeId Network::addNodeWithId(NodeId id, const std::string& typeName, const std::string& name) {
    if (id == invalidNodeId || slots_.count(id)) throw std::runtime_error("node id " + std::to_string(id) + " is invalid or already in use");
    std::unique_ptr<Node> n = NodeRegistry::instance().create(typeName);
    n->id_ = id;
    n->network_ = this;
    n->name_ = name;
    Slot s;
    s.inputs.assign(static_cast<size_t>(n->inputCount()), invalidNodeId);
    s.node = std::move(n);
    slots_.emplace(id, std::move(s));
    nextId_ = std::max(nextId_, id + 1);
    return id;
}

void Network::clear() {
    slots_.clear();
    nextId_ = 1;
    displayNode_ = invalidNodeId;
    evaluating_.clear();
}

std::vector<NodeId> Network::nodeIds() const {
    std::vector<NodeId> ids;
    for (const auto& entry : slots_) ids.push_back(entry.first);
    return ids;
}

void Network::removeNode(NodeId id) {
    slot(id);
    for (auto& [otherId, s] : slots_) {
        for (size_t i = 0; i < s.inputs.size(); ++i) {
            if (s.inputs[i] == id) {
                s.inputs[i] = invalidNodeId;
                markDirty(otherId);
            }
        }
    }
    slots_.erase(id);
    if (displayNode_ == id) displayNode_ = invalidNodeId;
}

Node& Network::node(NodeId id) { return *slot(id).node; }
const Node& Network::node(NodeId id) const { return *slot(id).node; }

NodeId Network::findNode(const std::string& name) const {
    for (const auto& [id, s] : slots_)
        if (s.node->name() == name) return id;
    return invalidNodeId;
}

bool Network::reaches(NodeId from, NodeId target) const {
    if (from == target) return true;
    for (NodeId in : slot(from).inputs)
        if (in != invalidNodeId && reaches(in, target)) return true;
    return false;
}

void Network::connect(NodeId from, NodeId to, int inputIndex) {
    slot(from);
    Slot& dst = slot(to);
    if (inputIndex < 0 || static_cast<size_t>(inputIndex) >= dst.inputs.size())
        throw std::runtime_error("input index out of range on " + dst.node->name());
    if (reaches(from, to)) throw std::runtime_error("connection would create a cycle");
    dst.inputs[static_cast<size_t>(inputIndex)] = from;
    markDirty(to);
}

void Network::disconnect(NodeId to, int inputIndex) {
    Slot& dst = slot(to);
    if (inputIndex < 0 || static_cast<size_t>(inputIndex) >= dst.inputs.size())
        throw std::runtime_error("input index out of range on " + dst.node->name());
    dst.inputs[static_cast<size_t>(inputIndex)] = invalidNodeId;
    markDirty(to);
}

NodeId Network::inputOf(NodeId to, int inputIndex) const {
    const Slot& s = slot(to);
    return s.inputs.at(static_cast<size_t>(inputIndex));
}

void Network::markDirty(NodeId id) {
    slot(id);
    std::set<NodeId> seen;
    std::vector<NodeId> pending{id};
    while (!pending.empty()) {
        NodeId current = pending.back();
        pending.pop_back();
        if (!seen.insert(current).second) continue;
        Node& n = *slot(current).node;
        n.dirty_ = true;
        n.cooked_.reset();
        const std::string& currentName = n.name();
        for (const auto& [otherId, other] : slots_) {
            const bool wired = std::find(other.inputs.begin(), other.inputs.end(), current) != other.inputs.end();
            if (wired || other.node->referencedNodes_.count(currentName)) pending.push_back(otherId);
        }
    }
}

void Network::setTime(double t) {
    if (t == time_) return;
    time_ = t;
    std::vector<NodeId> affected;
    for (const auto& entry : slots_)
        if (dependsOnTime(entry.first)) affected.push_back(entry.first);
    for (NodeId id : affected) markDirty(id);
}

float Network::resolveSymbol(NodeId owner, const std::string& symbol) {
    if (symbol == "time") return static_cast<float>(time_);
    if (symbol == "frame") return static_cast<float>(time_ * fps_);
    if (symbol == "pi") return 3.14159265358979f;
    const size_t dot = symbol.find('.');
    if (dot != std::string::npos) {
        const NodeId target = findNode(symbol.substr(0, dot));
        if (target == invalidNodeId) throw std::runtime_error("expression references unknown node '" + symbol.substr(0, dot) + "'");
        return paramValue(target, symbol.substr(dot + 1));
    }
    if (!node(owner).hasFloat(symbol)) throw std::runtime_error("unknown symbol '" + symbol + "' in expression on " + node(owner).name());
    return paramValue(owner, symbol);
}

float Network::paramValue(NodeId id, const std::string& name) {
    Node& n = node(id);
    Node::FloatParam& p = n.floatParam(name);
    if (!p.animated()) return p.value;

    const auto key = std::make_pair(id, name);
    if (!evaluating_.insert(key).second) throw std::runtime_error("parameter expression cycle at " + n.name() + "." + name);
    struct Guard {
        std::set<std::pair<NodeId, std::string>>& set;
        std::pair<NodeId, std::string> key;
        ~Guard() { set.erase(key); }
    } guard{evaluating_, key};

    p.resolved = !p.keys.empty() ? evaluateKeyframes(p.keys, time_)
                                 : evaluateScalar(p.expressionSpec, [&](const std::string& s) { return resolveSymbol(id, s); });
    return p.resolved;
}

void Network::evaluateParams(Node& n) {
    for (const auto& entry : n.floats_)
        if (entry.second.animated()) paramValue(n.id_, entry.first);
}

bool Network::paramDependsOnTime(NodeId id, const std::string& name, std::set<std::pair<NodeId, std::string>>& visiting) const {
    const Node& n = node(id);
    const Node::FloatParam& p = n.floatParam(name);
    if (!p.keys.empty()) return true;
    if (p.expression.empty() || !visiting.insert({id, name}).second) return false;
    for (const std::string& symbol : p.expressionSpec.channels) {
        if (symbol == "time" || symbol == "frame") return true;
        if (symbol == "pi") continue;
        const size_t dot = symbol.find('.');
        if (dot != std::string::npos) {
            const NodeId target = findNode(symbol.substr(0, dot));
            const std::string param = symbol.substr(dot + 1);
            if (target != invalidNodeId && node(target).hasFloat(param) && paramDependsOnTime(target, param, visiting)) return true;
        } else if (n.hasFloat(symbol) && paramDependsOnTime(id, symbol, visiting)) {
            return true;
        }
    }
    return false;
}

bool Network::dependsOnTime(NodeId id) const {
    std::set<std::pair<NodeId, std::string>> visiting;
    for (const auto& entry : node(id).floats_)
        if (paramDependsOnTime(id, entry.first, visiting)) return true;
    return false;
}

size_t Network::consumerCount(NodeId id) const {
    size_t count = 0;
    for (const auto& entry : slots_)
        count += static_cast<size_t>(std::count(entry.second.inputs.begin(), entry.second.inputs.end(), id));
    return count;
}

void Network::planCook(NodeId id, std::map<NodeId, CookTask>& tasks) {
    Node& n = node(id);
    if ((!n.dirty_ && n.cooked_) || tasks.count(id)) return;

    CookTask task;
    task.tail = id;
    if (fusionEnabled_ && n.isPointwise()) {
        task.cls = n.attribClass();
        task.chain.push_back(id);
        for (;;) {
            const NodeId up = slot(task.chain.back()).inputs[0];
            if (up == invalidNodeId) break;
            const Node& upNode = *slot(up).node;
            const bool fusable = upNode.isPointwise() && upNode.attribClass() == task.cls &&
                                 (upNode.dirty_ || !upNode.cooked_) && up != displayNode_ && consumerCount(up) == 1;
            if (!fusable) break;
            task.chain.push_back(up);
        }
        const NodeId baseId = slot(task.chain.back()).inputs[0];
        if (baseId == invalidNodeId) throw std::runtime_error(node(task.chain.back()).name() + ": input 0 is not connected");
        task.inputs.push_back(baseId);
    } else {
        const Slot& s = slot(id);
        for (size_t i = 0; i < s.inputs.size(); ++i) {
            if (s.inputs[i] == invalidNodeId)
                throw std::runtime_error(n.name() + ": input " + std::to_string(i) + " is not connected");
            task.inputs.push_back(s.inputs[i]);
        }
    }

    for (NodeId in : task.inputs) {
        planCook(in, tasks);
        auto it = tasks.find(in);
        if (it != tasks.end()) task.level = std::max(task.level, it->second.level + 1);
    }
    tasks.emplace(id, std::move(task));
}

void Network::runTask(const CookTask& task) {
    using Clock = std::chrono::steady_clock;
    auto millis = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };

    const auto start = Clock::now();
    Node& tail = node(task.tail);
    CookTimings timings;
    GeometryHandle result;

    if (!task.chain.empty()) {
        GeometryHandle base = node(task.inputs[0]).cooked_;
        PointKernel fused;
        for (size_t i = task.chain.size(); i-- > 0;) {
            PointKernel k;
            node(task.chain[i]).pointKernel(k);
            if (i + 1 == task.chain.size()) {
                fused = std::move(k);
            } else {
                fused.spec = composeKernels(fused.spec, k.spec);
                fused.params.insert(fused.params.end(), k.params.begin(), k.params.end());
            }
        }

        auto geo = std::make_shared<Geometry>(*base);
        const auto compileStart = Clock::now();
        auto kernel = compiler_.compileFor(fused.spec, *geo, task.cls);
        const auto runStart = Clock::now();
        kernel->run(*geo, fused.params, task.cls, pool_.get());
        const auto runEnd = Clock::now();
        timings.compileMs = millis(compileStart, runStart);
        timings.kernelMs = millis(runStart, runEnd);
        result = geo;

        if (task.chain.size() > 1) ++fusedChainCount_;
        for (size_t i = 1; i < task.chain.size(); ++i) node(task.chain[i]).stats_.fusedInto = task.tail;
    } else {
        std::vector<GeometryHandle> inputs;
        inputs.reserve(task.inputs.size());
        for (NodeId in : task.inputs) inputs.push_back(node(in).cooked_);
        CookContext ctx{compiler_, time_, pool_.get(), &timings};
        result = tail.cook(ctx, inputs);
    }

    tail.cooked_ = result;
    tail.dirty_ = false;
    ++tail.cookCount_;

    const double total = millis(start, Clock::now());
    NodeStats& stats = tail.stats_;
    ++stats.cooks;
    stats.lastCookMs = total;
    stats.totalCookMs += total;
    stats.lastCompileMs = timings.compileMs;
    stats.lastKernelMs = timings.kernelMs;
    stats.lastChainLength = task.chain.empty() ? 1 : task.chain.size();
    stats.fusedInto = invalidNodeId;
}

GeometryHandle Network::cook(NodeId id) {
    Node& n = node(id);
    if (!n.dirty_ && n.cooked_) return n.cooked_;

    std::map<NodeId, CookTask> tasks;
    planCook(id, tasks);

    int maxLevel = 0;
    for (const auto& [tail, task] : tasks) {
        maxLevel = std::max(maxLevel, task.level);
        if (task.chain.empty()) evaluateParams(node(tail));
        for (NodeId member : task.chain) evaluateParams(node(member));
    }

    std::vector<std::vector<const CookTask*>> levels(static_cast<size_t>(maxLevel) + 1);
    for (const auto& entry : tasks) levels[static_cast<size_t>(entry.second.level)].push_back(&entry.second);
    for (const auto& level : levels) pool_->parallelFor(level.size(), [&](size_t i) { runTask(*level[i]); });
    return node(id).cooked_;
}

void Network::setThreadCount(size_t threads) { pool_ = std::make_unique<ThreadPool>(threads > 1 ? threads - 1 : 0); }

void Network::resetProfile() {
    for (auto& entry : slots_) entry.second.node->stats_ = NodeStats{};
}

std::string Network::profileReport() const {
    std::string out = "node                 type          cooks  last ms  total ms  compile  kernel  note\n";
    char line[256];
    for (const auto& [id, s] : slots_) {
        const Node& n = *s.node;
        const NodeStats& st = n.stats_;
        std::string note;
        if (st.fusedInto != invalidNodeId) note = "fused into " + node(st.fusedInto).name();
        else if (st.lastChainLength > 1) note = "fused chain of " + std::to_string(st.lastChainLength);
        std::snprintf(line, sizeof(line), "%-20.20s %-12.12s %6zu %8.3f %9.3f %8.3f %7.3f  %s\n", n.name().c_str(),
                      n.typeName().c_str(), st.cooks, st.lastCookMs, st.totalCookMs, st.lastCompileMs, st.lastKernelMs, note.c_str());
        out += line;
    }
    std::snprintf(line, sizeof(line), "kernels compiled: %zu (%.3f ms), threads: %zu\n", compiler_.compileCount(),
                  compiler_.compileMillis(), pool_->concurrency());
    return out + line;
}

std::vector<NodeId> Network::executionOrder(NodeId id) const {
    std::vector<NodeId> order;
    std::set<NodeId> seen;
    std::function<void(NodeId)> visit = [&](NodeId n) {
        if (!seen.insert(n).second) return;
        for (NodeId in : slot(n).inputs)
            if (in != invalidNodeId) visit(in);
        order.push_back(n);
    };
    visit(id);
    return order;
}

}  // namespace jitnodes
