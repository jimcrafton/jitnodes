#include "editorModel.h"

#include "graphLayout.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

namespace jitnodes::editor {

namespace {

std::string formatFloat(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
    return buf;
}

bool parseNumber(const std::string& text, float& out) {
    const char* begin = text.c_str();
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end == begin) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != '\0') return false;
    out = static_cast<float>(v);
    return true;
}

std::string trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

bool isStringParam(const Node& n, const std::string& name) {
    const auto names = n.stringNames();
    return std::find(names.begin(), names.end(), name) != names.end();
}

}  // namespace

EditorModel::EditorModel() { resetHistory(); }

void EditorModel::resetHistory() {
    history_ = std::make_unique<History>(net_);
    saved_ = snapshotNetwork(net_);
    lastCook_ = CookResult{};
    geometry_.reset();
}

void EditorModel::rebaseHistory() {
    history_ = std::make_unique<History>(net_);
    validatePath();
}

void EditorModel::newGraph() {
    net_.clear();
    path_.clear();
    subnetPath_.clear();
    selection_.clear();
    resetHistory();
}

void EditorModel::open(const std::string& path) {
    loadNetworkFromFile(net_, path);
    placeNewNodes(net_);
    path_ = path;
    subnetPath_.clear();
    selection_.clear();
    resetHistory();
}

void EditorModel::save(const std::string& path) {
    saveNetworkToFile(net_, path);
    path_ = path;
    saved_ = snapshotNetwork(net_);
}

bool EditorModel::modified() const { return !sameGraph(snapshotNetwork(net_), saved_); }

std::vector<NodeId> EditorModel::selection() const {
    std::vector<NodeId> out;
    const auto ids = current().nodeIds();
    for (NodeId id : selection_)
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) out.push_back(id);
    return out;
}

NodeId EditorModel::selected() const {
    const auto ids = selection();
    return ids.size() == 1 ? ids.front() : invalidNodeId;
}

bool EditorModel::isSelected(NodeId id) const { return selection_.count(id) != 0; }

void EditorModel::select(NodeId id) {
    selection_.clear();
    if (id != invalidNodeId) selection_.insert(id);
}

void EditorModel::setSelection(const std::vector<NodeId>& ids) { selection_ = std::set<NodeId>(ids.begin(), ids.end()); }

void EditorModel::toggleSelected(NodeId id) {
    if (!selection_.erase(id)) selection_.insert(id);
}

void EditorModel::selectAll() {
    const auto ids = current().nodeIds();
    selection_ = std::set<NodeId>(ids.begin(), ids.end());
}

NodeId EditorModel::addNode(const std::string& typeName) {
    const NodeId previous = selected();
    const NodeId id = current().addNode(typeName);
    if (previous != invalidNodeId && current().node(id).inputCount() > 0) current().connect(previous, id, 0);
    current().setDisplayNode(id);
    select(id);
    placeNewNodes(current());
    history_->commit("add " + typeName);
    return id;
}

void EditorModel::removeNode(NodeId id) {
    const std::string name = current().node(id).name();
    current().removeNode(id);
    selection_.erase(id);
    history_->commit("delete " + name);
}

bool EditorModel::removeSelected() {
    const std::vector<NodeId> ids = selection();
    if (ids.empty()) return false;
    const std::string label = ids.size() == 1 ? "delete " + current().node(ids.front()).name() : "delete " + std::to_string(ids.size()) + " nodes";
    for (NodeId id : ids) current().removeNode(id);
    selection_.clear();
    history_->commit(label);
    return true;
}

std::string EditorModel::renameNode(NodeId id, const std::string& name) {
    const std::string old = current().node(id).name();
    if (old == name) return {};
    try {
        current().renameNode(id, name);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    history_->commit("rename " + old);
    return {};
}

bool EditorModel::copySelected() {
    const std::vector<NodeId> ids = selection();
    if (ids.empty()) return false;
    clip_.clear();
    for (NodeId id : ids) clip_.push_back(copyNode(current(), id));
    pasteCount_ = 0;
    return true;
}

bool EditorModel::cutSelected() {
    if (!copySelected()) return false;
    const std::vector<NodeId> ids = selection();
    const std::string label = ids.size() == 1 ? "cut " + current().node(ids.front()).name() : "cut " + std::to_string(ids.size()) + " nodes";
    for (NodeId id : ids) current().removeNode(id);
    selection_.clear();
    history_->commit(label);
    return true;
}

std::string EditorModel::paste() {
    if (clip_.empty()) return "nothing to paste";
    std::vector<NodeId> ids;
    try {
        ids = pasteNodes(current(), clip_);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    ++pasteCount_;
    const float shift = 30.0f * static_cast<float>(pasteCount_);
    for (NodeId id : ids) {
        Node& n = current().node(id);
        if (n.hasPosition()) n.setPosition(n.posX() + shift, n.posY() + shift);
    }
    setSelection(ids);
    placeNewNodes(current());
    history_->commit(ids.size() == 1 ? "paste " + current().node(ids.front()).name() : "paste " + std::to_string(ids.size()) + " nodes");
    return {};
}

void EditorModel::setDisplay(NodeId id) {
    current().setDisplayNode(id);
    history_->commit("set display");
}

void EditorModel::ensurePositions() { placeNewNodes(current()); }

namespace {

void collectProfile(const Network& net, int depth, std::vector<ProfileRow>& out) {
    for (NodeId id : net.nodeIds()) {
        const Node& n = net.node(id);
        const NodeStats& st = n.stats();
        ProfileRow row;
        row.id = id;
        row.depth = depth;
        row.name = n.name();
        row.type = n.typeName();
        row.cooks = st.cooks;
        row.lastMs = st.lastCookMs;
        row.totalMs = st.totalCookMs;
        if (st.fusedInto != invalidNodeId) row.note = "fused into " + net.node(st.fusedInto).name();
        else if (st.lastChainLength > 1) row.note = "fused chain of " + std::to_string(st.lastChainLength);
        if (st.cooks > 0 && st.lastCompileMs > 0.0) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "compiled %.2f ms", st.lastCompileMs);
            row.note += (row.note.empty() ? "" : ", ") + std::string(buf);
        }
        out.push_back(std::move(row));
        if (const Network* child = n.childNetwork()) collectProfile(*child, depth + 1, out);
    }
}

}  // namespace

std::vector<ProfileRow> EditorModel::profile() const {
    std::vector<ProfileRow> rows;
    collectProfile(net_, 0, rows);
    return rows;
}

std::vector<ParamInfo> EditorModel::parameters(NodeId id) {
    std::vector<ParamInfo> out;
    Node& n = current().node(id);
    for (const std::string& name : n.floatNames()) {
        ParamInfo p;
        p.name = name;
        p.expression = !n.expression(name).empty();
        p.keyed = !n.keyframes(name).empty();
        p.text = p.expression ? n.expression(name) : formatFloat(p.keyed ? current().paramValue(id, name) : n.baseFloat(name));
        out.push_back(std::move(p));
    }
    for (const std::string& name : n.stringNames()) {
        ParamInfo p;
        p.name = name;
        p.isText = true;
        p.multiline = name == "code";
        p.text = n.getString(name);
        out.push_back(std::move(p));
    }
    return out;
}

std::string EditorModel::setParameter(NodeId id, const std::string& name, const std::string& text) {
    try {
        Node& n = current().node(id);
        if (isStringParam(n, name)) {
            n.setString(name, text);
        } else {
            const std::string t = trimmed(text);
            float v = 0.0f;
            if (t.empty()) {
                n.clearAnimation(name);
            } else if (parseNumber(t, v)) {
                n.clearAnimation(name);
                n.setFloat(name, v);
            } else {
                n.setExpression(name, t);
            }
        }
        history_->commit("edit " + n.name() + "." + name);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

std::string EditorModel::toggleExpression(NodeId id, const std::string& name) {
    try {
        Node& n = current().node(id);
        if (isStringParam(n, name)) return name + " is a text parameter";
        const float v = current().paramValue(id, name);
        if (!n.expression(name).empty()) {
            n.clearAnimation(name);
            n.setFloat(name, v);
        } else {
            n.setExpression(name, formatFloat(v));
        }
        history_->commit("expression " + n.name() + "." + name);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

std::vector<Keyframe> EditorModel::keyframes(NodeId id, const std::string& name) const {
    const Node& n = current().node(id);
    return n.hasFloat(name) ? n.keyframes(name) : std::vector<Keyframe>{};
}

std::string EditorModel::setKeyframes(NodeId id, const std::string& name, std::vector<Keyframe> keys) {
    try {
        Node& n = current().node(id);
        if (isStringParam(n, name)) return name + " is a text parameter";
        if (keys.empty()) {
            const float v = current().paramValue(id, name);
            n.clearAnimation(name);
            n.setFloat(name, v);
        } else {
            n.setKeyframes(name, std::move(keys));
        }
        history_->commit("keys " + n.name() + "." + name);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

std::string EditorModel::keyframeNow(NodeId id, const std::string& name) {
    try {
        Node& n = current().node(id);
        if (isStringParam(n, name)) return name + " is a text parameter";
        const float v = current().paramValue(id, name);
        std::vector<Keyframe> keys = n.keyframes(name);
        const double t = current().time();
        auto same = std::find_if(keys.begin(), keys.end(), [t](const Keyframe& k) { return std::fabs(k.time - t) < 1e-6; });
        if (same != keys.end()) same->value = v;
        else keys.push_back({t, v, KeyInterp::linear});
        n.setKeyframes(name, std::move(keys));
        history_->commit("key " + n.name() + "." + name);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

bool EditorModel::undo() {
    const bool ok = history_->undo();
    validatePath();
    return ok;
}

bool EditorModel::redo() {
    const bool ok = history_->redo();
    validatePath();
    return ok;
}

Network& EditorModel::current() {
    Network* n = &net_;
    for (NodeId id : subnetPath_) n = n->node(id).childNetwork();
    return *n;
}

const Network& EditorModel::current() const {
    const Network* n = &net_;
    for (NodeId id : subnetPath_) n = n->node(id).childNetwork();
    return *n;
}

void EditorModel::validatePath() {
    Network* n = &net_;
    size_t keep = 0;
    for (NodeId id : subnetPath_) {
        const auto ids = n->nodeIds();
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) break;
        Network* child = n->node(id).childNetwork();
        if (!child || n->node(id).isAssetInstance()) break;
        n = child;
        ++keep;
    }
    if (keep != subnetPath_.size()) {
        subnetPath_.resize(keep);
        selection_.clear();
    }
}

bool EditorModel::canEnter(NodeId id) const {
    const auto ids = current().nodeIds();
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) return false;
    const Node& n = current().node(id);
    return n.childNetwork() != nullptr && !n.isAssetInstance();
}

std::string EditorModel::enter(NodeId id) {
    const auto ids = current().nodeIds();
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) return "no such node";
    const Node& n = current().node(id);
    if (!n.childNetwork()) return n.name() + " is not a subnet";
    if (n.isAssetInstance()) return n.name() + " is an asset instance; its contents come from the asset definition";
    subnetPath_.push_back(id);
    selection_.clear();
    return {};
}

bool EditorModel::up() {
    if (subnetPath_.empty()) return false;
    const NodeId left = subnetPath_.back();
    subnetPath_.pop_back();
    select(left);
    return true;
}

std::string EditorModel::pathText() const {
    std::string text = "/";
    const Network* n = &net_;
    for (size_t i = 0; i < subnetPath_.size(); ++i) {
        const Node& owner = n->node(subnetPath_[i]);
        text += (i ? "/" : "") + owner.name();
        n = owner.childNetwork();
    }
    return text;
}

std::vector<std::string> EditorModel::assetNames() const { return NodeRegistry::instance().assetNames(); }

bool EditorModel::isAssetInstance(NodeId id) const {
    const auto ids = current().nodeIds();
    return std::find(ids.begin(), ids.end(), id) != ids.end() && current().node(id).isAssetInstance();
}

std::map<std::string, size_t> EditorModel::assetRevisions() const {
    std::map<std::string, size_t> revisions;
    for (const std::string& name : NodeRegistry::instance().assetNames()) revisions[name] = NodeRegistry::instance().assetRevision(name);
    return revisions;
}

size_t EditorModel::updateChangedAssets(const std::map<std::string, size_t>& before) {
    size_t total = 0;
    for (const std::string& name : NodeRegistry::instance().assetNames()) {
        const auto it = before.find(name);
        if (it == before.end() || it->second != NodeRegistry::instance().assetRevision(name)) total += updateAssetInstances(net_, name);
    }
    if (total) rebaseHistory();
    return total;
}

std::string EditorModel::loadAssetFile(const std::string& path, size_t* updated) {
    if (updated) *updated = 0;
    try {
        const auto before = assetRevisions();
        registerAssetFromFile(path);
        const size_t n = updateChangedAssets(before);
        if (updated) *updated = n;
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

std::string EditorModel::loadAssetsFrom(const std::string& directory, size_t* loaded) {
    if (loaded) *loaded = 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) return {};
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.size() > 11 && name.compare(name.size() - 11, 11, ".asset.json") == 0) files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    std::string firstError;
    for (const std::string& file : files) {
        const std::string error = loadAssetFile(file);
        if (error.empty()) {
            if (loaded) ++*loaded;
        } else if (firstError.empty()) {
            firstError = std::filesystem::path(file).filename().string() + ": " + error;
        }
    }
    return firstError;
}

std::string EditorModel::createAsset(NodeId id, const std::string& typeName, const std::string& path, bool replace, size_t* updated) {
    if (updated) *updated = 0;
    try {
        if (!canEnter(id)) return "select a subnet (not an asset instance) first";
        bool valid = !typeName.empty() && (std::isalpha(static_cast<unsigned char>(typeName[0])) || typeName[0] == '_');
        for (char c : typeName) valid = valid && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
        if (!valid) return "an asset name starts with a letter and holds letters, digits and underscores";
        NodeRegistry& registry = NodeRegistry::instance();
        if (registry.has(typeName) && !registry.assetText(typeName)) return typeName + " is the name of a built-in node type";
        const int version = registry.assetText(typeName) ? registry.assetVersion(typeName) + 1 : 1;

        const auto before = assetRevisions();
        saveAssetToFile(current(), id, typeName, path, version);
        registerAssetFromFile(path);

        if (replace) {
            const Node& old = current().node(id);
            std::unique_ptr<Node> fresh = registry.create(typeName);
            for (const std::string& name : old.floatNames()) {
                if (!fresh->hasFloat(name)) continue;
                fresh->setFloat(name, old.baseFloat(name));
                if (!old.expression(name).empty()) fresh->setExpression(name, old.expression(name));
                else if (!old.keyframes(name).empty()) fresh->setKeyframes(name, old.keyframes(name));
            }
            std::vector<NodeId> inputs;
            for (int i = 0; i < old.inputCount(); ++i) inputs.push_back(current().inputOf(id, i));
            inputs.resize(static_cast<size_t>(fresh->inputCount()), invalidNodeId);
            const float x = old.posX(), y = old.posY();
            const bool placed = old.hasPosition();
            current().replaceNode(id, std::move(fresh), &inputs);
            if (placed) current().node(id).setPosition(x, y);
        }
        const size_t n = updateChangedAssets(before);
        if (updated) *updated = n;
        if (n == 0) history_->commit("create asset " + typeName);
        validatePath();
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

std::string EditorModel::unlockAsset(NodeId id) {
    try {
        if (!isAssetInstance(id)) return "select an asset instance first";
        const std::string name = current().node(id).name();
        unlockAssetInstance(current(), id);
        history_->commit("unlock " + name);
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

std::string EditorModel::addSpareParameter(NodeId id, const std::string& name) {
    try {
        Node& n = current().node(id);
        if (!n.childNetwork() || n.isAssetInstance()) return "only subnets can have promoted parameters";
        bool valid = !name.empty() && (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_');
        for (char c : name) valid = valid && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
        if (!valid) return "a parameter name starts with a letter and holds letters, digits and underscores";
        static const char* reserved[] = {"time", "frame", "pi", "parent"};
        for (const char* r : reserved)
            if (name == r) return name + " is reserved";
        if (n.hasFloat(name) || isStringParam(n, name)) return n.name() + " already has a parameter named " + name;
        n.addSpareFloat(name, 0.0f);
        history_->commit("add parameter " + n.name() + "." + name);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

CookResult EditorModel::cook() {
    CookResult r;
    const auto start = std::chrono::steady_clock::now();
    geometry_.reset();
    try {
        // Cook every subnet on the way in first, so each inner network has received its inputs.
        Network* n = &net_;
        for (NodeId owner : subnetPath_) {
            n->cook(owner);
            n = n->node(owner).childNetwork();
        }
        if (n->displayNode() != invalidNodeId) {
            geometry_ = n->cookDisplay();
            r.points = geometry_->pointCount();
        }
    } catch (const std::exception& e) {
        geometry_.reset();
        r.ok = false;
        r.error = e.what();
    }
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    lastCook_ = r;
    return r;
}

void EditorModel::setPlayheadFraction(double fraction) {
    net_.setTime(std::clamp(fraction, 0.0, 1.0) * duration_);
}

void EditorModel::advance(double seconds) { net_.setTime(std::fmod(net_.time() + seconds, duration_)); }

std::string EditorModel::statusLine() const {
    std::string text = std::to_string(current().nodeIds().size()) + (current().nodeIds().size() == 1 ? " node" : " nodes");
    if (current().displayNode() != invalidNodeId) text += ", display: " + current().node(current().displayNode()).name();
    return text;
}

}  // namespace jitnodes::editor
