#include "jitnodes/graph/node.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>

#include "jitnodes/graph/network.h"
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

void Node::tagParam(const std::string& name, const std::string& tag) {
    std::vector<std::string>& tags = floatParam(name).tags;
    if (!tag.empty() && std::find(tags.begin(), tags.end(), tag) == tags.end()) tags.push_back(tag);
}

const std::vector<std::string>& Node::paramTags(const std::string& name) const { return floatParam(name).tags; }

bool Node::hasParamTag(const std::string& name, const std::string& tag) const {
    const std::vector<std::string>& tags = floatParam(name).tags;
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

std::optional<std::string> Node::paramTagValue(const std::string& name, const std::string& key) const {
    const std::string prefix = key + ":";
    for (const std::string& tag : floatParam(name).tags)
        if (tag.compare(0, prefix.size(), prefix) == 0) return tag.substr(prefix.size());
    return std::nullopt;
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
    if (text.empty()) {
        p.clear();
        p.resolved = p.value;
    } else {
        FloatParam::Animation& a = p.edit();
        a.expression = text;
        a.expressionSpec = std::move(spec);
        a.keys.clear();
    }
    rebuildReferences();
    markChanged();
}

const std::string& Node::expression(const std::string& name) const { return floatParam(name).expression(); }

void Node::setKeyframes(const std::string& name, std::vector<Keyframe> keys) {
    FloatParam& p = floatParam(name);
    sortKeyframes(keys);
    if (keys.empty()) {
        p.clear();
        p.resolved = p.value;
    } else {
        FloatParam::Animation& a = p.edit();
        a.keys = std::move(keys);
        a.expression.clear();
        a.expressionSpec = KernelSpec();
    }
    rebuildReferences();
    markChanged();
}

const std::vector<Keyframe>& Node::keyframes(const std::string& name) const { return floatParam(name).keys(); }

void Node::clearAnimation(const std::string& name) {
    FloatParam& p = floatParam(name);
    if (!p.animated()) return;
    p.clear();
    p.resolved = p.value;
    rebuildReferences();
    markChanged();
}

bool Node::isAnimated(const std::string& name) const { return floatParam(name).animated(); }

void Node::rebuildReferences() {
    const std::set<std::string> before = std::move(referencedNodes_);
    referencedNodes_.clear();
    for (const auto& [paramName, p] : floats_) {
        for (const std::string& symbol : p.expressionSpec().channels) {
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
    bypassed_ = bypassed;
    markChanged();
}

void Node::setFrozenGeometry(GeometryHandle geometry) {
    if (geometry == frozen_ || (geometry && frozen_ && sameGeometry(*geometry, *frozen_))) return;
    frozen_ = std::move(geometry);
    markChanged();
}

void Node::setTags(std::vector<std::string> tags) {
    if (tags == tags_) return;
    tags_ = std::move(tags);
    touch();
}

bool Node::addTag(const std::string& tag) {
    if (tag.empty() || hasTag(tag)) return false;
    tags_.push_back(tag);
    touch();
    return true;
}

bool Node::removeTag(const std::string& tag) {
    const auto it = std::find(tags_.begin(), tags_.end(), tag);
    if (it == tags_.end()) return false;
    tags_.erase(it);
    touch();
    return true;
}

bool Node::hasTag(const std::string& tag) const { return std::find(tags_.begin(), tags_.end(), tag) != tags_.end(); }

std::optional<std::string> Node::tagValue(const std::string& key) const {
    const std::string prefix = key + ":";
    for (const std::string& tag : tags_)
        if (tag.compare(0, prefix.size(), prefix) == 0) return tag.substr(prefix.size());
    return std::nullopt;
}

uint64_t Node::freshRevision() { return ++revisionCounter; }

void Node::touch() {
    revision_ = ++revisionCounter;
    if (network_) network_->noteTouched(id_, revision_);
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
