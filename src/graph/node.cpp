#include "jitnodes/graph/node.h"

#include <stdexcept>

#include "jitnodes/graph/network.h"
#include "jitnodes/jit/exprParser.h"

namespace jitnodes {

Node::Node(std::string typeName, int inputCount) : typeName_(std::move(typeName)), inputCount_(inputCount) {}

Node::~Node() = default;

void Node::declareFloat(const std::string& name, float defaultValue) {
    FloatParam& p = floats_[name];
    p.value = p.resolved = defaultValue;
}

void Node::addSpareFloat(const std::string& name, float defaultValue) {
    declareFloat(name, defaultValue);
    spare_.insert(name);
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
    if (p.animated()) return;
    p.resolved = value;
    markChanged();
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

void Node::markChanged() {
    if (network_) network_->markDirty(id_);
    else dirty_ = true;
}

}  // namespace jitnodes
