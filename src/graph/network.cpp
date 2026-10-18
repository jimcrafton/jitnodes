#include "jitnodes/graph/network.h"

#include <algorithm>
#include <cctype>
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
    slots_.at(id).node->onAttached(*this);
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

void Network::fitInputs(Slot& s) {
    if (!s.node->hasVariableInputs()) return;
    size_t wanted = 2;
    for (size_t i = 0; i < s.inputs.size(); ++i)
        if (s.inputs[i] != invalidNodeId) wanted = std::max(wanted, i + 2);
    s.inputs.resize(wanted, invalidNodeId);
    s.node->setInputCount(static_cast<int>(wanted));
}

void Network::removeNode(NodeId id) {
    slot(id);
    for (auto& [otherId, s] : slots_) {
        bool changed = false;
        for (size_t i = 0; i < s.inputs.size(); ++i) {
            if (s.inputs[i] == id) {
                s.inputs[i] = invalidNodeId;
                changed = true;
            }
        }
        if (changed) {
            fitInputs(s);
            markDirty(otherId);
        }
    }
    slots_.erase(id);
    if (displayNode_ == id) displayNode_ = invalidNodeId;
}

void Network::replaceNode(NodeId id, std::unique_ptr<Node> fresh, const std::vector<NodeId>* inputs) {
    Slot& s = slot(id);
    if (inputs) {
        for (NodeId in : *inputs) {
            if (in == invalidNodeId) continue;
            slot(in);
            if (reaches(in, id)) throw std::runtime_error("new inputs of " + s.node->name() + " would create a cycle");
        }
        s.inputs = *inputs;
    }
    fresh->id_ = id;
    fresh->network_ = this;
    fresh->name_ = s.node->name_;
    if (s.node->hasPosition() && !fresh->hasPosition()) fresh->setPosition(s.node->posX(), s.node->posY());
    if (fresh->hasVariableInputs()) fresh->setInputCount(static_cast<int>(std::max<size_t>(s.inputs.size(), 2)));
    s.inputs.resize(static_cast<size_t>(fresh->inputCount()), invalidNodeId);
    s.node = std::move(fresh);
    fitInputs(s);
    s.node->onAttached(*this);
    markDirty(id);
}

Node& Network::node(NodeId id) { return *slot(id).node; }
const Node& Network::node(NodeId id) const { return *slot(id).node; }

NodeId Network::findNode(const std::string& name) const {
    for (const auto& [id, s] : slots_)
        if (s.node->name() == name) return id;
    return invalidNodeId;
}

std::string Network::uniqueName(const std::string& base) const {
    if (!base.empty() && findNode(base) == invalidNodeId) return base;
    size_t letters = base.size();
    while (letters > 0 && std::isdigit(static_cast<unsigned char>(base[letters - 1]))) --letters;
    const std::string stem = letters == 0 ? std::string("node") : base.substr(0, letters);
    for (size_t n = 1;; ++n) {
        const std::string candidate = stem + std::to_string(n);
        if (findNode(candidate) == invalidNodeId) return candidate;
    }
}

namespace {

bool identifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Replaces the node part of `oldName.param` symbols; other identifiers and numbers are left alone.
std::string renameReferences(const std::string& text, const std::map<std::string, std::string>& renames) {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            while (i < text.size() && (identifierChar(text[i]) || text[i] == '.')) out += text[i++];
        } else if (identifierChar(c)) {
            size_t end = i;
            while (end < text.size() && identifierChar(text[end])) ++end;
            const std::string word = text.substr(i, end - i);
            const auto it = renames.find(word);
            out += it != renames.end() && end < text.size() && text[end] == '.' ? it->second : word;
            i = end;
        } else {
            out += text[i++];
        }
    }
    return out;
}

}  // namespace

void Network::rewriteReferences(NodeId id, const std::map<std::string, std::string>& renames) {
    Node& n = *slot(id).node;
    for (const std::string& param : n.floatNames()) {
        const std::string& expression = n.expression(param);
        if (expression.empty()) continue;
        const std::string rewritten = renameReferences(expression, renames);
        if (rewritten != expression) n.setExpression(param, rewritten);
    }
}

void Network::renameNode(NodeId id, const std::string& newName) {
    Node& target = *slot(id).node;
    if (newName == target.name_) return;
    const bool valid = !newName.empty() && !std::isdigit(static_cast<unsigned char>(newName[0])) &&
                       std::all_of(newName.begin(), newName.end(), identifierChar);
    if (!valid) throw std::runtime_error("a node name uses letters, digits and underscores and does not start with a digit");
    if (findNode(newName) != invalidNodeId) throw std::runtime_error("a node named '" + newName + "' already exists");
    const std::map<std::string, std::string> renames{{target.name_, newName}};
    for (auto& entry : slots_) rewriteReferences(entry.first, renames);
    target.name_ = newName;
    for (auto& entry : slots_) markDirty(entry.first);
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
    if (inputIndex < 0 || (static_cast<size_t>(inputIndex) >= dst.inputs.size() && !dst.node->hasVariableInputs()))
        throw std::runtime_error("input index out of range on " + dst.node->name());
    if (reaches(from, to)) throw std::runtime_error("connection would create a cycle");
    if (static_cast<size_t>(inputIndex) >= dst.inputs.size()) dst.inputs.resize(static_cast<size_t>(inputIndex) + 1, invalidNodeId);
    dst.inputs[static_cast<size_t>(inputIndex)] = from;
    fitInputs(dst);
    markDirty(to);
}

void Network::disconnect(NodeId to, int inputIndex) {
    Slot& dst = slot(to);
    if (inputIndex < 0 || static_cast<size_t>(inputIndex) >= dst.inputs.size())
        throw std::runtime_error("input index out of range on " + dst.node->name());
    dst.inputs[static_cast<size_t>(inputIndex)] = invalidNodeId;
    fitInputs(dst);
    markDirty(to);
}

NodeId Network::inputOf(NodeId to, int inputIndex) const {
    const Slot& s = slot(to);
    if (s.node->hasVariableInputs() && inputIndex >= 0 && static_cast<size_t>(inputIndex) >= s.inputs.size()) return invalidNodeId;
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
    notifyOwner();
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
    if (dot != std::string::npos && symbol.compare(0, dot, "parent") == 0) {
        if (!parent_) throw std::runtime_error("'parent.' references are only valid inside a subnet");
        return parent_->paramValue(owner_, symbol.substr(dot + 1));
    }
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
        if (dot != std::string::npos && symbol.compare(0, dot, "parent") == 0) {
            const std::string param = symbol.substr(dot + 1);
            std::set<std::pair<NodeId, std::string>> parentVisiting;
            if (parent_ && parent_->node(owner_).hasFloat(param) && parent_->paramDependsOnTime(owner_, param, parentVisiting)) return true;
            continue;
        }
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
    return node(id).childDependsOnTime();
}

bool Network::anyDependsOnTime() const {
    for (const auto& entry : slots_)
        if (dependsOnTime(entry.first)) return true;
    return false;
}

void Network::setExternalInputs(const std::vector<GeometryHandle>& inputs) {
    auto at = [](const std::vector<GeometryHandle>& v, int i) { return i >= 0 && static_cast<size_t>(i) < v.size() ? v[static_cast<size_t>(i)] : GeometryHandle(); };
    std::vector<NodeId> changed;
    for (const auto& [id, s] : slots_) {
        if (s.node->typeName() != "subnetInput") continue;
        const int index = static_cast<int>(s.node->baseFloat("index"));
        if (at(externalInputs_, index).get() != at(inputs, index).get()) changed.push_back(id);
    }
    externalInputs_ = inputs;
    for (NodeId id : changed) markDirty(id);
}

GeometryHandle Network::externalInput(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= externalInputs_.size() || !externalInputs_[static_cast<size_t>(index)])
        throw std::runtime_error("subnet input " + std::to_string(index) + " is not connected");
    return externalInputs_[static_cast<size_t>(index)];
}

void Network::invalidateReferencesTo(const std::string& nodeName) {
    std::vector<NodeId> affected;
    for (const auto& [id, s] : slots_)
        if (s.node->referencedNodes_.count(nodeName)) affected.push_back(id);
    for (NodeId id : affected) markDirty(id);
}

size_t Network::consumerCount(NodeId id) const {
    size_t count = 0;
    for (const auto& entry : slots_)
        count += static_cast<size_t>(std::count(entry.second.inputs.begin(), entry.second.inputs.end(), id));
    return count;
}

void Network::collectMergeInputs(NodeId id, CookTask& task) {
    const Slot& s = slot(id);
    for (size_t i = 0; i < s.inputs.size(); ++i) {
        const NodeId up = s.inputs[i];
        if (up == invalidNodeId) continue;
        const Node& upNode = node(up);
        const bool absorbable = upNode.isMerge() && (upNode.dirty_ || !upNode.cooked_) && up != displayNode_ && consumerCount(up) == 1;
        if (absorbable) {
            task.absorbed.push_back(up);
            collectMergeInputs(up, task);
        } else {
            task.inputs.push_back(up);
        }
    }
}

void Network::planCook(NodeId id, std::map<NodeId, CookTask>& tasks) {
    Node& n = node(id);
    if ((!n.dirty_ && n.cooked_) || tasks.count(id)) return;

    CookTask task;
    task.tail = id;
    if (fusionEnabled() && n.isPointwise()) {
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
    } else if (fusionEnabled() && n.isMerge()) {
        collectMergeInputs(id, task);
        if (task.inputs.empty()) throw std::runtime_error(n.name() + ": no input is connected");
    } else {
        const Slot& s = slot(id);
        for (size_t i = 0; i < s.inputs.size(); ++i) {
            if (s.inputs[i] == invalidNodeId) {
                if (n.hasVariableInputs()) continue;
                throw std::runtime_error(n.name() + ": input " + std::to_string(i) + " is not connected");
            }
            task.inputs.push_back(s.inputs[i]);
        }
        if (task.inputs.empty() && n.hasVariableInputs()) throw std::runtime_error(n.name() + ": no input is connected");
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

    if (cancelRequested()) throw CookCancelled();
    const auto start = Clock::now();
    Node& tail = node(task.tail);
    CookTimings timings;
    GeometryHandle result;

    if (!task.chain.empty()) {
        GeometryHandle base = node(task.inputs[0]).cooked_;
        PointKernel fused;
        const AttribClass chainClass = task.cls;
        auto channelType = [&](const std::string& name) {
            const AttribArray* a = base->findAttrib(chainClass, name);
            return a ? a->type() : AttribType::float32;
        };
        for (size_t i = task.chain.size(); i-- > 0;) {
            PointKernel k;
            node(task.chain[i]).pointKernel(k);
            if (i + 1 == task.chain.size()) {
                fused = std::move(k);
            } else {
                appendKernel(fused.spec, k.spec, channelType);
                fused.params.insert(fused.params.end(), k.params.begin(), k.params.end());
            }
        }

        auto geo = std::make_shared<Geometry>(*base);
        const auto compileStart = Clock::now();
        auto kernel = compiler().compileFor(fused.spec, *geo, task.cls);
        const auto runStart = Clock::now();
        kernel->run(*geo, fused.params, task.cls, pool());
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
        CookContext ctx{compiler(), time_, pool(), &timings, this};
        result = tail.cook(ctx, inputs);
        for (NodeId a : task.absorbed) node(a).stats_.fusedInto = task.tail;
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

    struct CookScope {
        Network& net;
        explicit CookScope(Network& n) : net(n) {
            if (!net.parent_) net.cooking_ = true;
        }
        ~CookScope() {
            if (!net.parent_) {
                net.cooking_ = false;
                net.cancelRequested_ = false;
            }
        }
    } scope(*this);

    std::map<NodeId, CookTask> tasks;
    planCook(id, tasks);

    int maxLevel = 0;
    for (const auto& [tail, task] : tasks) {
        maxLevel = std::max(maxLevel, task.level);
        if (task.chain.empty()) evaluateParams(node(tail));
        for (NodeId member : task.chain) evaluateParams(node(member));
        for (NodeId member : task.absorbed) evaluateParams(node(member));
    }

    std::vector<std::vector<const CookTask*>> levels(static_cast<size_t>(maxLevel) + 1);
    for (const auto& entry : tasks) levels[static_cast<size_t>(entry.second.level)].push_back(&entry.second);
    for (const auto& level : levels) pool()->parallelFor(level.size(), [&](size_t i) { runTask(*level[i]); });
    return node(id).cooked_;
}

void Network::cancel() {
    Network* root = this;
    while (root->parent_) root = root->parent_;
    if (root->cooking_) root->cancelRequested_ = true;
}

void Network::setThreadCount(size_t threads) { pool_ = std::make_unique<ThreadPool>(threads > 1 ? threads - 1 : 0); }

void Network::resetProfile() {
    for (auto& entry : slots_) {
        entry.second.node->stats_ = NodeStats{};
        if (Network* child = entry.second.node->childNetwork()) child->resetProfile();
    }
}

void Network::appendProfileRows(std::string& out, int depth) const {
    char line[256];
    for (const auto& [id, s] : slots_) {
        const Node& n = *s.node;
        const NodeStats& st = n.stats_;
        std::string note;
        if (st.fusedInto != invalidNodeId) note = "fused into " + node(st.fusedInto).name();
        else if (st.lastChainLength > 1) note = "fused chain of " + std::to_string(st.lastChainLength);
        const std::string label = std::string(static_cast<size_t>(depth) * 2, ' ') + n.name();
        std::snprintf(line, sizeof(line), "%-20.20s %-12.12s %6zu %8.3f %9.3f %8.3f %7.3f  %s\n", label.c_str(), n.typeName().c_str(), st.cooks,
                      st.lastCookMs, st.totalCookMs, st.lastCompileMs, st.lastKernelMs, note.c_str());
        out += line;
        if (const Network* child = n.childNetwork()) child->appendProfileRows(out, depth + 1);
    }
}

std::string Network::profileReport() const {
    std::string out = "node                 type          cooks  last ms  total ms  compile  kernel  note\n";
    appendProfileRows(out, 0);
    char line[256];
    std::snprintf(line, sizeof(line), "kernels compiled: %zu (%.3f ms), threads: %zu\n", compiler().compileCount(),
                  compiler().compileMillis(), pool()->concurrency());
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
