#include "jitnodes/graph/node.h"

#include <atomic>
#include <cstring>
#include <stdexcept>

#include "jitnodes/graph/network.h"
#include "jitnodes/core/xxhash.h"
#include "jitnodes/jit/exprParser.h"

namespace jitnodes {

namespace {
std::atomic<uint64_t> revisionCounter{0};
}

Node::Node(std::string typeName, int inputCount) : revision_(++revisionCounter), typeName_(std::move(typeName)), inputCount_(inputCount) {}

Node::~Node() = default;

void Node::declareFloat(const std::string& name, float defaultValue) {
    FloatParam& p = floats_[name];
    p.value = p.resolved = defaultValue;
}

void Node::addSpareFloat(const std::string& name, float defaultValue) {
    declareFloat(name, defaultValue);
    spare_.insert(name);
    touch();
}

void Node::declareString(const std::string& name, const std::string& defaultValue) { strings_[name] = defaultValue; }

void Node::declareGeometry(const std::string& name) { geometries_[name] = std::make_shared<const Geometry>(); }

Node::FloatParam& Node::floatParam(const std::string& name) {
    auto it = floats_.find(name);
    if (it == floats_.end()) throw std::runtime_error("unknown float parameter '" + name + "' on " + typeName_);
    return it->second;
}

const Node::FloatParam& Node::floatParam(const std::string& name) const {
    auto it = floats_.find(name);
    if (it == floats_.end()) throw std::runtime_error("unknown float parameter '" + name + "' on " + typeName_);
    return it->second;
}

void Node::setFloat(const std::string& name, float value) {
    FloatParam& p = floatParam(name);
    if (p.value == value) return;
    p.value = value;
    if (p.animated()) {
        touch();
        return;
    }
    p.resolved = value;
    markChanged(false);
}

void Node::publishFloat(const std::string& name, float value) {
    FloatParam& p = floatParam(name);
    if (p.value != value) touch();
    p.value = p.resolved = value;
}

float Node::getFloat(const std::string& name) const { return floatParam(name).resolved; }

float Node::baseFloat(const std::string& name) const { return floatParam(name).value; }

std::vector<std::string> Node::floatNames() const {
    std::vector<std::string> names;
    for (const auto& entry : floats_) names.push_back(entry.first);
    return names;
}

std::vector<std::string> Node::stringNames() const {
    std::vector<std::string> names;
    for (const auto& entry : strings_) names.push_back(entry.first);
    return names;
}

void Node::setExpression(const std::string& name, const std::string& text) {
    FloatParam& p = floatParam(name);
    KernelSpec spec;
    if (!text.empty()) spec = parseWrangle("__result = " + text + ";", {});
    p.expression = text;
    p.expressionSpec = std::move(spec);
    p.keys.clear();
    if (text.empty()) p.resolved = p.value;
    rebuildReferences();
    markChanged();
}

const std::string& Node::expression(const std::string& name) const { return floatParam(name).expression; }

void Node::setKeyframes(const std::string& name, std::vector<Keyframe> keys) {
    FloatParam& p = floatParam(name);
    sortKeyframes(keys);
    p.keys = std::move(keys);
    p.expression.clear();
    p.expressionSpec = KernelSpec();
    if (p.keys.empty()) p.resolved = p.value;
    rebuildReferences();
    markChanged();
}

const std::vector<Keyframe>& Node::keyframes(const std::string& name) const { return floatParam(name).keys; }

void Node::clearAnimation(const std::string& name) {
    FloatParam& p = floatParam(name);
    if (!p.animated()) return;
    p.expression.clear();
    p.expressionSpec = KernelSpec();
    p.keys.clear();
    p.resolved = p.value;
    rebuildReferences();
    markChanged();
}

bool Node::isAnimated(const std::string& name) const { return floatParam(name).animated(); }

void Node::rebuildReferences() {
    const std::set<std::string> before = std::move(referencedNodes_);
    referencedNodes_.clear();
    for (const auto& [paramName, p] : floats_) {
        for (const std::string& symbol : p.expressionSpec.channels) {
            const size_t dot = symbol.find('.');
            if (dot != std::string::npos) referencedNodes_.insert(symbol.substr(0, dot));
        }
    }
    if (network_) network_->referencesChanged(id_, before, referencedNodes_);
}

void Node::setString(const std::string& name, const std::string& value) {
    auto it = strings_.find(name);
    if (it == strings_.end()) throw std::runtime_error("unknown string parameter '" + name + "' on " + typeName_);
    if (it->second == value) return;
    it->second = value;
    markChanged();
}

const std::string& Node::getString(const std::string& name) const {
    auto it = strings_.find(name);
    if (it == strings_.end()) throw std::runtime_error("unknown string parameter '" + name + "' on " + typeName_);
    return it->second;
}

void Node::setGeometryParam(const std::string& name, GeometryHandle geometry) {
    auto it = geometries_.find(name);
    if (it == geometries_.end()) throw std::runtime_error("unknown geometry parameter '" + name + "' on " + typeName_);
    if (!geometry) geometry = std::make_shared<const Geometry>();
    if (it->second == geometry || sameGeometry(*it->second, *geometry)) return;
    it->second = std::move(geometry);
    markChanged();
}

const GeometryHandle& Node::geometryParam(const std::string& name) const {
    auto it = geometries_.find(name);
    if (it == geometries_.end()) throw std::runtime_error("unknown geometry parameter '" + name + "' on " + typeName_);
    return it->second;
}

std::vector<std::string> Node::geometryNames() const {
    std::vector<std::string> names;
    for (const auto& entry : geometries_) names.push_back(entry.first);
    return names;
}

void Node::setBypassed(bool bypassed) {
    if (bypassed_ == bypassed) return;
    if (bypassed && inputCount_ > 0 && inputType(0) != outputType())
        throw std::runtime_error("cannot bypass " + name_ + ": its input is " + dataKindName(inputType(0)) + " but its output is " +
                                 dataKindName(outputType()));
    bypassed_ = bypassed;
    markChanged();
}

void Node::setFrozenGeometry(GeometryHandle geometry) {
    if (geometry == frozen_ || (geometry && frozen_ && sameGeometry(*geometry, *frozen_))) return;
    frozen_ = std::move(geometry);
    markChanged();
}

void Node::touch() { revision_ = ++revisionCounter; }

DataHandle Node::cookData(CookContext& ctx, const std::vector<DataHandle>& inputs) {
    std::vector<GeometryHandle> geo;
    geo.reserve(inputs.size());
    for (const DataHandle& in : inputs) geo.push_back(asGeometry(in));
    return cook(ctx, geo);
}

uint64_t Node::paramsHash() const {
    auto mix = [](uint64_t a, uint64_t b) { return a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2)); };
    uint64_t h = 0;
    for (const auto& [key, p] : floats_) {
        uint32_t bits;
        std::memcpy(&bits, &p.resolved, sizeof bits);
        h = mix(h, xxh64(key.data(), key.size()));
        h = mix(h, bits);
    }
    for (const auto& [key, value] : strings_) {
        h = mix(h, xxh64(key.data(), key.size()));
        h = mix(h, xxh64(value.data(), value.size()));
    }
    return h;
}

void Node::markChanged(bool structural) {
    touch();
    if (network_) {
        network_->markDirty(id_, structural);
        network_->noteEdited(id_);
    }
    else dirty_ = true;
}

}  // namespace jitnodes
