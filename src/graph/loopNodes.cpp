#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "jitnodes/core/threadPool.h"
#include "jitnodes/geo/topology.h"
#include "jitnodes/graph/network.h"
#include "jitnodes/graph/serialize.h"
#include "jitnodes/graph/subnet.h"
#include "nodeUtil.h"

namespace jitnodes {

namespace {

constexpr size_t maxIterations = 100'000;
constexpr int64_t maxSolverSteps = 100'000;

enum class LoopMode { primitives, points, copies, feedback };

LoopMode loopModeOf(const std::string& text) {
    if (text == "primitives") return LoopMode::primitives;
    if (text == "points") return LoopMode::points;
    if (text == "copies") return LoopMode::copies;
    if (text == "feedback") return LoopMode::feedback;
    throw std::runtime_error("foreach: mode must be 'primitives', 'points', 'copies' or 'feedback', got '" + text + "'");
}

// Primitives with equal values of a primitive attribute form one piece; pieces keep the order of their first primitive.
std::vector<std::vector<uint32_t>> piecesByAttribute(const Geometry& in, const std::string& name) {
    const AttribArray* array = in.findAttrib(AttribClass::primitive, name);
    if (!array) throw std::runtime_error("foreach: the input has no primitive attribute '" + name + "'");
    std::unordered_map<uint32_t, size_t> pieceOf;
    std::vector<std::vector<uint32_t>> pieces;
    for (uint32_t i = 0; i < in.primitiveCount(); ++i) {
        uint32_t key;
        if (array->type() == AttribType::float32) std::memcpy(&key, &array->floats().data()[i], sizeof key);
        else key = static_cast<uint32_t>(array->ints().data()[i]);
        auto [it, inserted] = pieceOf.emplace(key, pieces.size());
        if (inserted) pieces.emplace_back();
        pieces[it->second].push_back(i);
    }
    return pieces;
}

// A subnet whose child graph runs once per piece of the input. Inside, subnetInput 0 is the piece (or, in feedback mode, the previous
// result, with subnetInput 1 the original input), and `parent.iteration` / `parent.numIterations` tell the iteration apart.
class ForeachNode : public SubnetNode {
public:
    ForeachNode() : SubnetNode("foreach", 1) {
        declareString("mode", "primitives");
        declareString("pieceAttrib", "");
        declareFloat("count", 3.0f);
        declareFloat("parallel", 0.0f);
        declareFloat("iteration", 0.0f);
        declareFloat("numIterations", 0.0f);
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& inputs) override {
        if (!ctx.network) throw std::runtime_error("foreach cooked outside a network");
        const LoopMode mode = loopModeOf(getString("mode"));
        const Geometry& in = *inputs[0];

        std::vector<GeometryHandle> pieces;
        if (mode == LoopMode::primitives) {
            std::vector<std::vector<uint32_t>> groups;
            if (!getString("pieceAttrib").empty()) groups = piecesByAttribute(in, getString("pieceAttrib"));
            else
                for (uint32_t i = 0; i < in.primitiveCount(); ++i) groups.push_back({i});
            requireCount(groups.size());
            for (const auto& g : groups) pieces.push_back(std::make_shared<const Geometry>(extractPrimitives(in, g)));
        } else if (mode == LoopMode::points) {
            requireCount(in.pointCount());
            for (uint32_t i = 0; i < in.pointCount(); ++i) pieces.push_back(std::make_shared<const Geometry>(extractPoints(in, {i})));
        } else {
            const float requested = getFloat("count");
            if (!(requested >= 0.0f)) throw std::runtime_error("foreach: count must not be negative");
            const size_t count = static_cast<size_t>(requested);
            requireCount(count);
            pieces.assign(count, inputs[0]);
        }

        const size_t total = pieces.size();
        if (mode == LoopMode::feedback) {
            GeometryHandle state = inputs[0];
            for (size_t i = 0; i < total; ++i) state = runIteration(*ctx.network, ctx.time, i, total, {state, inputs[0]});
            return state;
        }

        std::vector<GeometryHandle> results(total);
        if (getFloat("parallel") > 0.5f && total > 1 && ctx.pool && ctx.pool->concurrency() > 1) {
            prepareWorkers();
            ctx.pool->parallelFor(total, [&](size_t i) {
                WorkerLease lease(*this);                lease.worker().net.setFps(ctx.network->fps());
                results[i] = lease.worker().node->runIteration(lease.worker().net, ctx.time, i, total, {pieces[i]});
            });
        } else {
            for (size_t i = 0; i < total; ++i) results[i] = runIteration(*ctx.network, ctx.time, i, total, {pieces[i]});
        }
        if (results.empty()) return std::make_shared<const Geometry>();
        std::vector<const Geometry*> parts;
        for (const GeometryHandle& r : results) parts.push_back(r.get());
        return std::make_shared<const Geometry>(Geometry::concat(parts, ctx.pool));
    }

private:
    // Parallel cooking gives every thread a private copy of the child graph (a one-node network holding a foreach), because the child graph
    // and the iteration counter are shared state. The copies keep their caches between cooks while the child graph and parameters stay put.
    struct Worker {
        Network net{1};
        ForeachNode* node = nullptr;
    };

    class WorkerLease {
    public:
        explicit WorkerLease(ForeachNode& owner) : owner_(owner) {
            {
                std::lock_guard<std::mutex> lock(owner_.workersMutex_);
                if (!owner_.idleWorkers_.empty()) {
                    worker_ = std::move(owner_.idleWorkers_.back());
                    owner_.idleWorkers_.pop_back();
                }
            }
            if (!worker_) worker_ = owner_.makeWorker();
        }
        ~WorkerLease() {
            std::lock_guard<std::mutex> lock(owner_.workersMutex_);
            owner_.idleWorkers_.push_back(std::move(worker_));
        }
        Worker& worker() { return *worker_; }

    private:
        ForeachNode& owner_;
        std::unique_ptr<Worker> worker_;
    };

    std::string workerKey() const {
        std::string key = saveNetwork(network(), false, true);
        for (const std::string& name : spareNames()) key += "\n" + name + "=" + std::to_string(getFloat(name));
        return key;
    }

    void prepareWorkers() {
        std::string key = workerKey();
        std::lock_guard<std::mutex> lock(workersMutex_);
        if (key != workersKey_) {
            idleWorkers_.clear();
            workersKey_ = std::move(key);
        }
    }

    std::unique_ptr<Worker> makeWorker() const {
        auto worker = std::make_unique<Worker>();
        const NodeId id = worker->net.addNode("foreach");
        worker->node = static_cast<ForeachNode*>(&worker->net.node(id));
        loadNetwork(worker->node->network(), saveNetwork(network(), false, true));
        for (const std::string& name : spareNames()) worker->node->addSpareFloat(name, getFloat(name));
        return worker;
    }

    // The counters go back to zero afterwards so the saved graph does not change with every cook (a solver compares saved text).
    GeometryHandle runIteration(const Network& outer, double time, size_t index, size_t total, const std::vector<GeometryHandle>& externals) {
        struct Reset {
            ForeachNode& node;
            ~Reset() {
                node.publishFloat("iteration", 0.0f);
                node.publishFloat("numIterations", 0.0f);
            }
        } reset{*this};
        publishFloat("iteration", static_cast<float>(index));
        publishFloat("numIterations", static_cast<float>(total));
        return cookInner(outer, time, externals, true);
    }

    static void requireCount(size_t n) {
        if (n > maxIterations) throw std::runtime_error("foreach: " + std::to_string(n) + " iterations; the limit is " + std::to_string(maxIterations));
    }

    std::mutex workersMutex_;
    std::vector<std::unique_ptr<Worker>> idleWorkers_;
    std::string workersKey_;
};

// A subnet that carries its result from one step to the next. The input is the state at `startTime`; each step cooks the child graph with
// subnetInput 0 = the previous state and subnetInput 1 = the original input, and the result becomes the next state. The latest state is kept,
// so playing forward only runs the new steps; scrubbing back or changing anything (parameters, the input, the child graph) restarts from
// the input. `parent.step` and `parent.dt` are available inside, and `time` there is the time of the step.
class SolverNode : public SubnetNode {
public:
    SolverNode() : SubnetNode("solver", 1) {
        declareFloat("startTime", 0.0f);
        declareFloat("stepsPerSecond", 24.0f);
        declareFloat("step", 0.0f);
        declareFloat("dt", 0.0f);
    }

    bool dependsOnPlayhead() const override { return true; }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& inputs) override {
        if (!ctx.network) throw std::runtime_error("solver cooked outside a network");
        const float stepsPerSecond = std::clamp(getFloat("stepsPerSecond"), 1.0f, 2000.0f);
        const float startTime = getFloat("startTime");
        const double dt = 1.0 / stepsPerSecond;
        const double elapsed = ctx.time - static_cast<double>(startTime);
        const int64_t target = elapsed <= 0.0 ? 0 : static_cast<int64_t>(std::floor(elapsed * stepsPerSecond + 1e-4));
        if (target > maxSolverSteps) throw std::runtime_error("solver: " + std::to_string(target) + " steps to simulate; the limit is " + std::to_string(maxSolverSteps));

        std::string fingerprint = saveNetwork(network(), false, true);
        for (const std::string& name : floatNames())
            if (name != "step" && name != "dt") fingerprint += "\n" + name + "=" + std::to_string(getFloat(name));
        const bool changed = step_ < 0 || source_ != inputs[0] || fingerprint != fingerprint_;
        if (changed) checkpoints_.clear();
        const auto* saved = !changed ? checkpoints_.resumeFor(step_, target) : nullptr;
        if (saved) {
            step_ = saved->step;
            state_ = saved->state;
        } else if (changed || target < step_) {
            step_ = 0;
            state_ = inputs[0];
            source_ = inputs[0];
            fingerprint_ = std::move(fingerprint);
            checkpoints_.offer(0, state_, geometryBytes(state_));
        }
        struct Reset {
            SolverNode& node;
            ~Reset() {
                node.publishFloat("step", 0.0f);
                node.publishFloat("dt", 0.0f);
            }
        } reset{*this};
        try {
            for (int64_t s = step_ + 1; s <= target; ++s) {
                publishFloat("step", static_cast<float>(s));
                publishFloat("dt", static_cast<float>(dt));
                state_ = cookInner(*ctx.network, static_cast<double>(startTime) + static_cast<double>(s) * dt, {state_, source_}, true);
                checkpoints_.offer(s, state_, geometryBytes(state_));
            }
        } catch (...) {
            step_ = -1;
            throw;
        }
        step_ = target;
        return state_;
    }

private:
    // Geometry is immutable once cooked, so a checkpoint is only a handle; the bytes are what it keeps alive.
    static size_t geometryBytes(const GeometryHandle& g) { return g ? g->pointCount() * 4 * sizeof(float) : 0; }

    int64_t step_ = -1;
    GeometryHandle state_;
    GeometryHandle source_;
    std::string fingerprint_;
    StateCheckpoints<GeometryHandle> checkpoints_;
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerLoopNodes() {
    add<ForeachNode>("foreach");
    add<SolverNode>("solver");
}

}  // namespace jitnodes
