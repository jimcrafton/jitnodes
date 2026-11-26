#include "jitnodes/graph/node.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <stdexcept>
#include <variant>

#include "jitnodes/graph/network.h"
#include "jitnodes/jit/exprParser.h"

namespace jitnodes {

namespace {
std::atomic<uint64_t> revisionCounter{0};
}

Node::Node(std::string typeName, int inputCount) : revision_(++revisionCounter), typeName_(std::move(typeName)), inputCount_(inputCount) {}

Node::~Node() = default;

GeometryHandle ValueNode::cook(CookContext&, const std::vector<GeometryHandle>&) {
    throw std::runtime_error(name() + ": a " + typeName() + " node cooks through cookValue");
}

Value Node::cookValue(CookContext& ctx, const std::vector<Value>& inputs) {
    std::vector<GeometryHandle> geometries;
    geometries.reserve(inputs.size());
    for (const Value& input : inputs) {
        geometries.push_back(input.geometry);
    }
    Value result;
    result.geometry = cook(ctx, geometries);
    return result;
}

std::string Node::uidText() const {
    if (uid_ == 0) return "0";
    std::string text;
    for (uint32_t v = uid_; v != 0; v /= 36) text.insert(text.begin(), "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36]);
    return text;
}

void Node::setUid(uint32_t uid) {
    if (uid == 0 && network_ != nullptr) uid = network_->takeUid();
    else if (network_ != nullptr) network_->reserveUid(uid);
    if (uid == uid_) return;
    uid_ = uid;
    touch();
}

void Node::declareFloat(const std::string& name, float defaultValue) {
    FloatParam& p = floats_[name];
    p.value = p.resolved = defaultValue;
}

void Node::addSpareFloat(const std::string& name, float defaultValue) {
    declareFloat(name, defaultValue);
    spare_.insert(name);
    touch();
}

bool Node::removeSpareFloat(const std::string& name) {
    if (spare_.erase(name) == 0) {
        return false;
    }
    floats_.erase(name);
    rebuildReferences();
    touch();
    return true;
}

void Node::renameSpareFloat(const std::string& from, const std::string& to) {
    if (spare_.count(from) == 0) {
        throw std::runtime_error("'" + from + "' is not a spare parameter of '" + name_ + "'");
    }
    if (from == to) {
        return;
    }
    if (floats_.count(to) != 0) {
        throw std::runtime_error("parameter '" + to + "' already exists on '" + name_ + "'");
    }
    auto entry = floats_.extract(from);
    entry.key() = to;
    floats_.insert(std::move(entry));
    spare_.erase(from);
    spare_.insert(to);
    rebuildReferences();
    touch();
}

bool Node::floatMinimum(const std::string& name, float& value) const {
    (void)name;
    (void)value;
    return false;
}

bool Node::floatMaximum(const std::string& name, float& value) const {
    (void)name;
    (void)value;
    return false;
}

void Node::declareString(const std::string& name, const std::string& defaultValue) { strings_[name] = defaultValue; }

void Node::declareRamp(const std::string& name, const std::string& defaultText) {
    const std::string problem = Ramp::check(defaultText);
    if (!problem.empty()) {
        throw std::logic_error("declareRamp '" + name + "': " + problem);
    }
    strings_[name] = defaultText;
    rampParams_.insert(name);
}

Ramp Node::getRamp(const std::string& name) const {
    if (!isRampParam(name)) {
        throw std::runtime_error("'" + name + "' is not a ramp parameter on " + typeName_);
    }
    try {
        return Ramp::parse(rampText(name));
    } catch (const std::exception& e) {
        throw std::runtime_error(typeName_ + " " + name + ": " + e.what());
    }
}

std::string Node::rampText(const std::string& name) const {
    const std::string& text = getString(name);
    const std::string target = Ramp::parentReference(text);
    if (target.empty()) {
        return text;
    }
    const Network* parent = network_ != nullptr ? network_->parent() : nullptr;
    if (parent == nullptr) {
        throw std::runtime_error("'" + text + "' is only valid inside a subnet");
    }
    const Node& owner = parent->node(network_->ownerNode());
    if (!owner.isRampParam(target)) {
        throw std::runtime_error("'" + text + "': " + owner.name() + " has no ramp parameter '" + target + "'");
    }
    return owner.rampText(target);
}

void Node::addSpareRamp(const std::string& name, const std::string& defaultText) {
    if (hasFloat(name) || hasString(name)) {
        throw std::runtime_error("parameter '" + name + "' already exists on " + typeName_);
    }
    const std::string problem = Ramp::parentReference(defaultText).empty() ? Ramp::check(defaultText) : std::string();
    if (!problem.empty()) {
        throw std::runtime_error("ramp '" + name + "': " + problem);
    }
    strings_[name] = defaultText;
    rampParams_.insert(name);
    spareRamps_.insert(name);
    rebuildReferences();
    markChanged();
}

bool Node::removeSpareRamp(const std::string& name) {
    if (spareRamps_.erase(name) == 0) {
        return false;
    }
    strings_.erase(name);
    rampParams_.erase(name);
    rebuildReferences();
    markChanged();
    return true;
}

void Node::renameSpareRamp(const std::string& from, const std::string& to) {
    if (spareRamps_.count(from) == 0) {
        throw std::runtime_error("'" + from + "' is not a spare ramp of '" + name_ + "'");
    }
    if (from == to) {
        return;
    }
    if (hasFloat(to) || hasString(to)) {
        throw std::runtime_error("parameter '" + to + "' already exists on '" + name_ + "'");
    }
    auto entry = strings_.extract(from);
    entry.key() = to;
    strings_.insert(std::move(entry));
    rampParams_.erase(from);
    rampParams_.insert(to);
    spareRamps_.erase(from);
    spareRamps_.insert(to);
    markChanged();
}

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

const std::vector<std::string>& Node::userParamTags(const std::string& name) const { return floatParam(name).userTags; }

void Node::setParamTags(const std::string& name, std::vector<std::string> tags) {
    FloatParam& p = floatParam(name);
    std::vector<std::string> kept;
    for (const std::string& tag : p.tags)
        if (std::find(p.userTags.begin(), p.userTags.end(), tag) == p.userTags.end()) kept.push_back(tag);   // the declared ones
    std::vector<std::string> user;
    for (std::string& tag : tags)
        if (!tag.empty() && std::find(kept.begin(), kept.end(), tag) == kept.end() && std::find(user.begin(), user.end(), tag) == user.end()) user.push_back(std::move(tag));
    if (user == p.userTags) return;
    p.tags = std::move(kept);
    p.tags.insert(p.tags.end(), user.begin(), user.end());
    p.userTags = std::move(user);
    touch();   // the saved state changed; the cook did not
}

void Node::addParamTag(const std::string& name, const std::string& tag) {
    std::vector<std::string> user = floatParam(name).userTags;
    user.push_back(tag);
    setParamTags(name, std::move(user));
}

std::optional<std::string> Node::paramTagValue(const std::string& name, const std::string& key) const {
    const std::string prefix = key + ":";
    for (const std::string& tag : floatParam(name).tags)
        if (tag.compare(0, prefix.size(), prefix) == 0) return tag.substr(prefix.size());
    return std::nullopt;
}

ParamKind Node::paramKind(const std::string& name) const {
    if (isRampParam(name)) {
        return ParamKind::ramp;
    }
    if (!hasFloat(name)) {
        return strings_.count(name) != 0 ? ParamKind::text : ParamKind::none;
    }
    if (const std::optional<std::string> kind = paramTagValue(name, "kind")) {
        if (*kind == "scalar") {
            return ParamKind::scalar;
        }
        return *kind == "color" ? ParamKind::color : ParamKind::number;
    }
    return paramTagValue(name, "choices") ? ParamKind::choice : ParamKind::scalar;
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
    for (const std::string& ramp : rampParams_) {
        if (!Ramp::parentReference(strings_.at(ramp)).empty()) {
            referencedNodes_.insert("parent");
        }
    }
    if (network_) network_->referencesChanged(id_, before, referencedNodes_);
}

void Node::setString(const std::string& name, const std::string& value) {
    auto it = strings_.find(name);
    if (it == strings_.end()) throw std::runtime_error("unknown string parameter '" + name + "' on " + typeName_);
    if (it->second == value) return;
    it->second = value;
    if (isRampParam(name)) {
        rebuildReferences();
    }
    stringChanged(name);
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

namespace {

// Carries a node type's own fields from one node's save to another's load, so no node type needs a copy of its own. The standard body is
// copied member by member by copyStateFrom, so both standard() calls do nothing.
class FieldTransfer : public NodeWriter, public NodeReader {
public:
    void standard(const Node&) override {}
    void standard(Node&) override {}
    void number(const std::string& name, double value) override { fields_[name] = value; }
    void text(const std::string& name, const std::string& value) override { fields_[name] = value; }
    void geometry(const std::string& name, const GeometryHandle& value) override {
        if (value) fields_[name] = value;
    }
    bool number(const std::string& name, double& value) const override { return take(name, value); }
    bool text(const std::string& name, std::string& value) const override { return take(name, value); }
    bool geometry(const std::string& name, GeometryHandle& value) const override { return take(name, value); }

    bool sameAs(const FieldTransfer& other) const {
        if (fields_.size() != other.fields_.size()) {
            return false;
        }
        for (auto a = fields_.begin(), b = other.fields_.begin(); a != fields_.end(); ++a, ++b) {
            if (a->first != b->first || a->second.index() != b->second.index()) {
                return false;
            }
            if (const double* number = std::get_if<double>(&a->second)) {
                if (!sameNumber(*number, std::get<double>(b->second))) {
                    return false;
                }
            } else if (const GeometryHandle* shape = std::get_if<GeometryHandle>(&a->second)) {
                if (!sameShape(*shape, std::get<GeometryHandle>(b->second))) {
                    return false;
                }
            } else if (std::get<std::string>(a->second) != std::get<std::string>(b->second)) {
                return false;
            }
        }
        return true;
    }

    static bool sameNumber(double a, double b) { return a == b || (a != a && b != b); }
    static bool sameShape(const GeometryHandle& a, const GeometryHandle& b) { return a == b || (a && b && sameGeometry(*a, *b)); }

private:
    template <typename T>
    bool take(const std::string& name, T& value) const {
        const auto found = fields_.find(name);
        if (found == fields_.end()) return false;
        const T* held = std::get_if<T>(&found->second);
        if (held == nullptr) return false;
        value = *held;
        return true;
    }

    std::map<std::string, std::variant<double, std::string, GeometryHandle>> fields_;
};

}  // namespace

std::unique_ptr<Node> Node::clone() const {
    std::unique_ptr<Node> copy = NodeRegistry::instance().create(typeName_);
    copy->name_ = name_;
    copy->inputCount_ = inputCount_;
    copy->copyStateFrom(*this);
    return copy;
}

void Node::copyStateFrom(const Node& source) {
    if (source.typeName_ != typeName_) {
        throw std::runtime_error("cannot copy the state of a " + source.typeName_ + " node onto a " + typeName_ + " node");
    }
    if (&source == this) {
        return;
    }
    if (network_ != nullptr && source.uid_ != 0) {
        network_->reserveUid(source.uid_);
    }
    uid_ = source.uid_;
    posX_ = source.posX_;
    posY_ = source.posY_;
    hasPosition_ = source.hasPosition_;
    bypassed_ = source.bypassed_;
    frozen_ = source.frozen_;
    tags_ = source.tags_;
    // Only a change to what drives the cook through time or other nodes (expressions, keys, spare parameters, text) is structural; a plain value
    // change must not drop the time caches of the whole network.
    bool structural = spare_ != source.spare_;
    for (const auto& entry : floats_) {
        structural = structural || entry.second.animated();
    }
    for (const auto& entry : source.floats_) {
        structural = structural || entry.second.animated();
    }
    structural = structural || spareRamps_ != source.spareRamps_;
    spare_ = source.spare_;
    spareRamps_ = source.spareRamps_;
    rampParams_ = source.rampParams_;
    floats_ = source.floats_;
    std::vector<std::string> changedStrings;
    for (const auto& [name, value] : source.strings_) {
        const auto mine = strings_.find(name);
        if (mine == strings_.end() || mine->second != value) {
            changedStrings.push_back(name);
        }
    }
    structural = structural || !changedStrings.empty();
    strings_ = source.strings_;
    geometries_ = source.geometries_;
    for (const std::string& name : changedStrings) {
        stringChanged(name);   // a node type derives state from its text (a wrangle's compiled code and declared parameters)
    }
    FieldTransfer fields;
    source.save(fields);
    load(fields);
    rebuildReferences();
    markChanged(structural);
}

bool Node::hasSameStateAs(const Node& other) const {
    if (&other == this) {
        return true;
    }
    if (other.typeName_ != typeName_ || other.uid_ != uid_ || other.hasPosition_ != hasPosition_ || other.bypassed_ != bypassed_ ||
        other.tags_ != tags_ || other.spare_ != spare_ || other.spareRamps_ != spareRamps_ || other.strings_ != strings_ ||other.floats_.size() != floats_.size() ||
        other.geometries_.size() != geometries_.size()) {
        return false;
    }
    if (hasPosition_ && (other.posX_ != posX_ || other.posY_ != posY_)) {
        return false;
    }
    if (!FieldTransfer::sameShape(frozen_, other.frozen_)) {
        return false;
    }
    for (auto a = floats_.begin(), b = other.floats_.begin(); a != floats_.end(); ++a, ++b) {
        const FloatParam& x = a->second;
        const FloatParam& y = b->second;
        if (a->first != b->first || !FieldTransfer::sameNumber(x.value, y.value) || x.userTags != y.userTags || x.expression() != y.expression() ||
            x.keys().size() != y.keys().size()) {
            return false;
        }
        for (size_t i = 0; i < x.keys().size(); ++i) {
            const Keyframe& k = x.keys()[i];
            const Keyframe& l = y.keys()[i];
            if (k.time != l.time || !FieldTransfer::sameNumber(k.value, l.value) || k.interp != l.interp) {
                return false;
            }
        }
    }
    for (auto a = geometries_.begin(), b = other.geometries_.begin(); a != geometries_.end(); ++a, ++b) {
        if (a->first != b->first || !FieldTransfer::sameShape(a->second, b->second)) {
            return false;
        }
    }
    FieldTransfer mine, theirs;
    save(mine);
    other.save(theirs);
    return mine.sameAs(theirs);
}

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
