#include "jitnodes/graph/serialize.h"

#include <json5/json5.hpp>
#include <json5/json5_builder.hpp>
#include <json5/json5_input.hpp>
#include <json5/json5_output.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "jitnodes/graph/subnet.h"

namespace jitnodes {

namespace {

constexpr int formatVersion = 1;
constexpr const char* graphFormat = "jitnodes";
constexpr const char* assetFormat = "jitnodes-asset";

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("graph file: " + message); }

void transferParameters(const Node& from, Node& to);

// Shortest decimal that round-trips the float, widened to double, so files show 0.1 and not 0.100000001.
json5::value floatValue(float f) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf), f);
    double d = static_cast<double>(f);
    std::from_chars(buf, r.ptr, d);
    return json5::value(d);
}

const char* interpName(KeyInterp i) {
    switch (i) {
        case KeyInterp::constant: return "constant";
        case KeyInterp::linear: return "linear";
        case KeyInterp::smooth: return "smooth";
    }
    return "linear";
}

KeyInterp parseInterp(const std::string& s) {
    if (s == "constant") return KeyInterp::constant;
    if (s == "linear") return KeyInterp::linear;
    if (s == "smooth") return KeyInterp::smooth;
    fail("unknown keyframe interpolation '" + s + "'");
}

std::string dump(const json5::document& doc) {
    json5::writer_params params;
    params.json_compatible = true;
    std::ostringstream os;
    os << std::setprecision(9);
    json5::to_stream(os, doc, params);
    return os.str();
}

std::string dumpValue(const json5::value& v) {
    json5::writer_params params;
    params.json_compatible = true;
    std::ostringstream os;
    os << std::setprecision(9);
    json5::to_stream(os, v, params, 0);
    return os.str();
}

// Copies a value (possibly from another document) into the builder's document. `skipKey`, when set, is
// omitted from the top-level object.
json5::value copyValue(json5::builder& b, const json5::value& v, const char* skipKey = nullptr) {
    if (v.is_object()) {
        b.push_object();
        for (auto [key, child] : json5::object_view(v)) {
            if (skipKey && std::string_view(key) == skipKey) continue;
            b[key] = copyValue(b, child);
        }
        return b.pop();
    }
    if (v.is_array()) {
        b.push_array();
        for (const json5::value& child : json5::array_view(v)) b += copyValue(b, child);
        return b.pop();
    }
    if (v.is_string()) return b.new_string(v.get_c_str());
    return v;
}

void collectAssets(const Network& net, std::set<std::string>& types) {
    for (NodeId id : net.nodeIds()) {
        const Node& n = net.node(id);
        if (NodeRegistry::instance().assetText(n.typeName())) types.insert(n.typeName());
        if (const Network* child = n.childNetwork()) collectAssets(*child, types);
    }
}

json5::value writeAssets(json5::builder& b, const std::set<std::string>& types) {
    b.push_array();
    for (const std::string& type : types) {
        json5::document doc;
        const json5::error err = json5::from_string(*NodeRegistry::instance().assetText(type), doc);
        if (err) fail("stored definition of asset '" + type + "' is corrupt");
        b += copyValue(b, doc, "assets");
    }
    return b.pop();
}

json5::value writeNode(json5::builder& b, const Network& net, NodeId id, bool withInputs = true);

// Adds fps/time/display/nodes to the currently open object.
void writeGraphFields(json5::builder& b, const Network& net, bool includeTime) {
    b["fps"] = json5::value(net.fps());
    if (includeTime) b["time"] = json5::value(net.time());
    b["display"] = json5::value(static_cast<double>(net.displayNode()));
    b.push_array();
    for (NodeId id : net.nodeIds()) b += writeNode(b, net, id);
    b["nodes"] = b.pop();
}

json5::value writeNode(json5::builder& b, const Network& net, NodeId id, bool withInputs) {
    const Node& n = net.node(id);
    b.push_object();
    b["id"] = json5::value(static_cast<double>(id));
    b["type"] = b.new_string(n.typeName());
    b["name"] = b.new_string(n.name());
    if (const auto* asset = dynamic_cast<const AssetNode*>(&n)) b["assetVersion"] = json5::value(asset->version());
    if (n.hasPosition()) {
        b.push_array();
        b += floatValue(n.posX());
        b += floatValue(n.posY());
        b["pos"] = b.pop();
    }

    if (!n.spareNames().empty()) {
        b.push_array();
        for (const std::string& name : n.spareNames()) b += b.new_string(name);
        b["spare"] = b.pop();
    }

    b.push_object();
    for (const std::string& name : n.floatNames()) b[name] = floatValue(n.baseFloat(name));
    b["floats"] = b.pop();

    b.push_object();
    for (const std::string& name : n.stringNames()) b[name] = b.new_string(n.getString(name));
    b["strings"] = b.pop();

    bool hasExpressions = false, hasKeys = false;
    for (const std::string& name : n.floatNames()) {
        hasExpressions |= !n.expression(name).empty();
        hasKeys |= !n.keyframes(name).empty();
    }
    if (hasExpressions) {
        b.push_object();
        for (const std::string& name : n.floatNames())
            if (!n.expression(name).empty()) b[name] = b.new_string(n.expression(name));
        b["expressions"] = b.pop();
    }
    if (hasKeys) {
        b.push_object();
        for (const std::string& name : n.floatNames()) {
            if (n.keyframes(name).empty()) continue;
            b.push_array();
            for (const Keyframe& k : n.keyframes(name)) {
                b.push_object();
                b["time"] = json5::value(k.time);
                b["value"] = floatValue(k.value);
                b["interp"] = b.new_string(interpName(k.interp));
                b += b.pop();
            }
            b[name] = b.pop();
        }
        b["keyframes"] = b.pop();
    }

    if (withInputs) {
        b.push_array();
        for (int i = 0; i < n.inputCount(); ++i) b += json5::value(static_cast<double>(net.inputOf(id, i)));
        b["inputs"] = b.pop();
    }

    if (const Network* child = n.childNetwork(); child && !n.isAssetInstance()) {
        b.push_object();
        writeGraphFields(b, *child, false);
        b["graph"] = b.pop();
    }
    return b.pop();
}

double requireNumber(const json5::value& v, const std::string& what) {
    if (!v.is_number()) fail(what + " must be a number");
    return v.get<double>();
}

std::string requireString(const json5::value& v, const std::string& what) {
    if (!v.is_string()) fail(what + " must be a string");
    return v.get_c_str();
}

// json5::value's default value is a zeroed bit pattern, which reads as the number 0, so a missing
// key must be detected through the object view instead of by inspecting the returned value.
bool member(const json5::value& obj, const char* key, json5::value& out) {
    const json5::object_view view(obj);
    const auto it = view.find(key);
    if (it == view.end()) return false;
    out = (*it).second;
    return true;
}

double requireNumberMember(const json5::value& obj, const char* key, const std::string& what) {
    json5::value v;
    if (!member(obj, key, v)) fail(what + " is missing");
    return requireNumber(v, what);
}

std::string requireStringMember(const json5::value& obj, const char* key, const std::string& what) {
    json5::value v;
    if (!member(obj, key, v)) fail(what + " is missing");
    return requireString(v, what);
}

void loadGraphBody(Network& net, const json5::value& body);
void registerAssetValue(const json5::value& def, const std::string& text);

// Applies the saved parameters (and child graph) of a node to n. Existing nodes keep their spare parameters.
void applyNodeBody(Node& n, const json5::value& nv, bool declareSpare) {
    {
        json5::value pos;
        if (member(nv, "pos", pos)) {
            const json5::array_view xy(pos);
            if (!xy.is_valid() || xy.size() != 2) fail("pos must be an array of two numbers");
            n.setPosition(static_cast<float>(requireNumber(xy[0], "pos x")), static_cast<float>(requireNumber(xy[1], "pos y")));
        }
        json5::value spare;
        if (declareSpare && member(nv, "spare", spare)) {
            const json5::array_view names(spare);
            if (!names.is_valid()) fail("spare must be an array of names");
            for (const json5::value& s : names) n.addSpareFloat(requireString(s, "spare parameter name"), 0.0f);
        }
        for (auto [key, value] : json5::object_view(nv["floats"])) n.setFloat(key, static_cast<float>(requireNumber(value, std::string("floats.") + key)));
        for (auto [key, value] : json5::object_view(nv["strings"])) n.setString(key, requireString(value, std::string("strings.") + key));
        for (auto [key, value] : json5::object_view(nv["expressions"])) n.setExpression(key, requireString(value, std::string("expressions.") + key));
        for (auto [key, value] : json5::object_view(nv["keyframes"])) {
            std::vector<Keyframe> keys;
            const json5::array_view arr(value);
            if (!arr.is_valid()) fail(std::string("keyframes.") + key + " must be an array");
            for (const json5::value& kv : arr) {
                Keyframe k;
                k.time = requireNumberMember(kv, "time", "keyframe time");
                k.value = static_cast<float>(requireNumberMember(kv, "value", "keyframe value"));
                json5::value interp;
                if (member(kv, "interp", interp)) k.interp = parseInterp(requireString(interp, "keyframe interp"));
                keys.push_back(k);
            }
            n.setKeyframes(key, std::move(keys));
        }
        json5::value graph;
        if (member(nv, "graph", graph)) {
            Network* child = n.childNetwork();
            if (!child || n.isAssetInstance()) fail("only subnets store a graph (asset instances take theirs from the definition)");
            loadGraphBody(*child, graph);
        }
    }
}

void loadNode(Network& net, const json5::value& nv, size_t index, std::vector<std::pair<NodeId, std::vector<NodeId>>>& wiring) {
    const std::string where = "node #" + std::to_string(index);
    if (!nv.is_object()) fail(where + " must be an object");
    const NodeId id = static_cast<NodeId>(requireNumberMember(nv, "id", where + ".id"));
    const std::string type = requireStringMember(nv, "type", where + ".type");
    const std::string name = requireStringMember(nv, "name", where + ".name");

    try {
        net.addNodeWithId(id, type, name);
        Node& n = net.node(id);
        const json5::array_view saved(nv["inputs"]);
        const std::string inputsWhere = "node '" + name + "' inputs";
        if (!saved.is_valid()) fail(inputsWhere + " must be an array of ids");
        std::vector<NodeId> inputs;
        for (size_t i = 0; i < saved.size(); ++i) inputs.push_back(static_cast<NodeId>(requireNumber(saved[i], inputsWhere)));

        auto* asset = dynamic_cast<AssetNode*>(&n);
        json5::value savedVersion;
        const int fromVersion = asset && member(nv, "assetVersion", savedVersion) ? static_cast<int>(requireNumber(savedVersion, "assetVersion")) : 1;
        if (asset && fromVersion != asset->version()) {
            AssetNode old(type, static_cast<int>(inputs.size()));
            applyNodeBody(old, nv, true);
            transferParameters(old, n);
            AssetUpgrade upgrade{fromVersion, asset->version(), old, n, inputs};
            if (const AssetMigrationFn* hook = NodeRegistry::instance().assetMigration(type)) (*hook)(upgrade);
            inputs = std::move(upgrade.inputs);
            inputs.resize(static_cast<size_t>(n.inputCount()), invalidNodeId);
        } else {
            if (!n.hasVariableInputs() && inputs.size() != static_cast<size_t>(n.inputCount())) fail(inputsWhere + " must be an array of " + std::to_string(n.inputCount()) + " ids");
            applyNodeBody(n, nv, true);
        }
        wiring.emplace_back(id, std::move(inputs));
    } catch (const std::runtime_error& e) {
        throw std::runtime_error("graph file: " + where + " ('" + name + "', " + type + "): " + e.what());
    }
}

// Loads fps/time/display/nodes into net (used for both the root graph and subnet children).
void loadGraphBody(Network& net, const json5::value& body) {
    if (!body.is_object()) fail("a graph must be an object");
    const json5::array_view nodes(body["nodes"]);
    if (!nodes.is_valid()) fail("nodes must be an array");

    net.clear();
    json5::value optional;
    if (member(body, "fps", optional)) net.setFps(requireNumber(optional, "fps"));

    std::vector<std::pair<NodeId, std::vector<NodeId>>> wiring;
    for (size_t i = 0; i < nodes.size(); ++i) loadNode(net, nodes[i], i, wiring);

    for (const auto& [id, inputs] : wiring) {
        const Node& n = net.node(id);
        for (size_t i = 0; i < inputs.size(); ++i) {
            const NodeId from = inputs[i];
            if (from == invalidNodeId) continue;
            try {
                net.connect(from, id, static_cast<int>(i));
            } catch (const std::runtime_error& e) {
                fail("node '" + n.name() + "' input " + std::to_string(i) + ": " + e.what());
            }
        }
    }

    if (member(body, "display", optional)) {
        const NodeId display = static_cast<NodeId>(requireNumber(optional, "display"));
        if (display != invalidNodeId) {
            const auto ids = net.nodeIds();
            if (std::find(ids.begin(), ids.end(), display) == ids.end()) fail("display refers to a missing node");
            net.setDisplayNode(display);
        }
    }
    if (member(body, "time", optional)) net.setTime(requireNumber(optional, "time"));
}

int assetVersionOf(const json5::value& def) {
    json5::value v;
    return member(def, "assetVersion", v) ? static_cast<int>(requireNumber(v, "assetVersion")) : 1;
}

void registerEmbeddedAssets(const json5::value& root) {
    json5::value assets;
    if (!member(root, "assets", assets)) return;
    const json5::array_view list(assets);
    if (!list.is_valid()) fail("assets must be an array");
    for (const json5::value& def : list) {
        const std::string type = requireStringMember(def, "type", "asset type");
        const NodeRegistry& registry = NodeRegistry::instance();
        if (registry.assetText(type) && assetVersionOf(def) <= registry.assetVersion(type)) continue;  // registered wins unless the file's copy is newer
        registerAssetValue(def, dumpValue(def));
    }
}

void loadFromValue(Network& net, const json5::value& root) {
    if (!root.is_object()) fail("the root must be an object");
    if (requireStringMember(root, "format", "format") != graphFormat) fail("not a jitnodes graph (format is not 'jitnodes')");
    const int version = static_cast<int>(requireNumberMember(root, "version", "version"));
    if (version != formatVersion) fail("unsupported version " + std::to_string(version) + " (this build reads version " + std::to_string(formatVersion) + ")");
    registerEmbeddedAssets(root);
    loadGraphBody(net, root);
}

// Creates a fresh instance of an asset from its definition.
std::unique_ptr<Node> instantiateAsset(const std::string& type, const std::string& text) {
    json5::document doc;
    if (json5::from_string(text, doc)) fail("stored definition of asset '" + type + "' is corrupt");
    auto node = std::make_unique<AssetNode>(type, static_cast<int>(requireNumberMember(doc, "inputs", "asset inputs")));
    for (auto [name, value] : json5::object_view(doc["promote"])) node->addSpareFloat(name, static_cast<float>(requireNumber(value, std::string("promote.") + name)));
    json5::value graph;
    if (!member(doc, "graph", graph)) fail("asset '" + type + "' has no graph");
    loadGraphBody(node->network(), graph);
    node->setRevision(NodeRegistry::instance().assetRevision(type));
    node->setVersion(NodeRegistry::instance().assetVersion(type));
    return node;
}

void registerAssetValue(const json5::value& def, const std::string& text) {
    if (!def.is_object()) fail("an asset definition must be an object");
    if (requireStringMember(def, "format", "asset format") != assetFormat) fail("not a jitnodes asset (format is not 'jitnodes-asset')");
    const int version = static_cast<int>(requireNumberMember(def, "version", "asset version"));
    if (version != formatVersion) fail("unsupported asset version " + std::to_string(version));
    const std::string type = requireStringMember(def, "type", "asset type");
    if (type.empty()) fail("asset type must not be empty");
    const int inputs = static_cast<int>(requireNumberMember(def, "inputs", "asset inputs"));
    if (inputs < 0 || inputs > 16) fail("asset inputs must be between 0 and 16");
    json5::value graph;
    if (!member(def, "graph", graph) || !graph.is_object()) fail("asset '" + type + "' needs a graph object");

    NodeRegistry& registry = NodeRegistry::instance();
    registerEmbeddedAssets(def);
    if (registry.has(type) && !registry.assetText(type)) fail("asset type '" + type + "' conflicts with a built-in node type");

    // Validate by building one instance before replacing anything.
    instantiateAsset(type, text);
    registry.registerType(type, [type, shared = std::make_shared<const std::string>(text)] { return instantiateAsset(type, *shared); });
    registry.setAssetText(type, text, assetVersionOf(def));
}

}  // namespace

std::string saveNetwork(const Network& net, bool includeTime) {
    json5::document doc;
    json5::builder b(doc);
    b.push_object();
    b["format"] = b.new_string(graphFormat);
    b["version"] = json5::value(formatVersion);
    std::set<std::string> assets;
    collectAssets(net, assets);
    if (!assets.empty()) b["assets"] = writeAssets(b, assets);
    writeGraphFields(b, net, includeTime);
    b.pop();
    return dump(doc);
}

void loadNetwork(Network& net, const std::string& text) {
    json5::document doc;
    const json5::error err = json5::from_string(text, doc);
    if (err) {
        fail(std::string("parse error at line ") + std::to_string(err.line) + ", column " + std::to_string(err.column) + ": " +
             json5::error::type_string[err.type]);
    }
    Network scratch(1);
    loadFromValue(scratch, doc);  // validates everything before touching net
    loadFromValue(net, doc);
}

std::string saveAsset(const Network& net, NodeId subnet, const std::string& typeName, int assetVersion) {
    const Node& n = net.node(subnet);
    const Network* child = n.childNetwork();
    if (!child) throw std::runtime_error("node '" + n.name() + "' is not a subnet");
    if (typeName.empty()) throw std::runtime_error("asset type name must not be empty");

    json5::document doc;
    json5::builder b(doc);
    b.push_object();
    b["format"] = b.new_string(assetFormat);
    b["version"] = json5::value(formatVersion);
    b["type"] = b.new_string(typeName);
    b["assetVersion"] = json5::value(assetVersion);
    b["inputs"] = json5::value(n.inputCount());
    b.push_object();
    for (const std::string& name : n.spareNames()) b[name] = floatValue(n.baseFloat(name));
    b["promote"] = b.pop();
    std::set<std::string> assets;
    collectAssets(*child, assets);
    assets.erase(typeName);
    if (!assets.empty()) b["assets"] = writeAssets(b, assets);
    b.push_object();
    writeGraphFields(b, *child, false);
    b["graph"] = b.pop();
    b.pop();
    return dump(doc);
}

void registerAsset(const std::string& definitionText) {
    json5::document doc;
    const json5::error err = json5::from_string(definitionText, doc);
    if (err) {
        fail(std::string("asset parse error at line ") + std::to_string(err.line) + ", column " + std::to_string(err.column) + ": " +
             json5::error::type_string[err.type]);
    }
    registerAssetValue(doc, definitionText);
}

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open '" + path + "'");
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot open '" + path + "' for writing");
    out << text;
    if (!out) throw std::runtime_error("failed writing '" + path + "'");
}

}  // namespace

void saveAssetToFile(const Network& net, NodeId subnet, const std::string& typeName, const std::string& path, int assetVersion) {
    writeFile(path, saveAsset(net, subnet, typeName, assetVersion));
}

void registerAssetFromFile(const std::string& path) { registerAsset(readFile(path)); }

namespace {

void transferParameters(const Node& from, Node& to) {
    for (const std::string& name : from.floatNames()) {
        if (!to.hasFloat(name)) continue;
        to.setFloat(name, from.baseFloat(name));
        if (!from.expression(name).empty()) to.setExpression(name, from.expression(name));
        else if (!from.keyframes(name).empty()) to.setKeyframes(name, from.keyframes(name));
    }
    const std::vector<std::string> kept = to.stringNames();
    for (const std::string& name : from.stringNames())
        if (std::find(kept.begin(), kept.end(), name) != kept.end()) to.setString(name, from.getString(name));
}

bool isOutdated(const Node& n, const std::string& typeName) {
    const auto* asset = dynamic_cast<const AssetNode*>(&n);
    return asset && (typeName.empty() || n.typeName() == typeName) && asset->revision() != NodeRegistry::instance().assetRevision(n.typeName());
}

size_t updateAssetsIn(Network& net, const std::string& typeName) {
    size_t count = 0;
    for (NodeId id : net.nodeIds()) {
        Node& n = net.node(id);
        if (isOutdated(n, typeName)) {
            std::unique_ptr<Node> fresh = NodeRegistry::instance().create(n.typeName());
            transferParameters(n, *fresh);
            std::vector<NodeId> inputs;
            for (int i = 0; i < n.inputCount(); ++i) inputs.push_back(net.inputOf(id, i));
            inputs.resize(static_cast<size_t>(fresh->inputCount()), invalidNodeId);

            const int fromVersion = dynamic_cast<const AssetNode&>(n).version();
            const int toVersion = dynamic_cast<const AssetNode&>(*fresh).version();
            const AssetMigrationFn* hook = NodeRegistry::instance().assetMigration(n.typeName());
            if (hook && fromVersion != toVersion) {
                AssetUpgrade upgrade{fromVersion, toVersion, n, *fresh, std::move(inputs)};
                try {
                    (*hook)(upgrade);
                } catch (const std::runtime_error& e) {
                    throw std::runtime_error("asset migration of '" + n.name() + "' (" + n.typeName() + "): " + e.what());
                }
                inputs = std::move(upgrade.inputs);
            }
            net.replaceNode(id, std::move(fresh), &inputs);
            ++count;
        } else if (Network* child = n.childNetwork()) {
            count += updateAssetsIn(*child, typeName);
        }
    }
    return count;
}

size_t countOutdated(const Network& net, const std::string& typeName) {
    size_t count = 0;
    for (NodeId id : net.nodeIds()) {
        const Node& n = net.node(id);
        if (isOutdated(n, typeName)) ++count;
        else if (const Network* child = n.childNetwork()) count += countOutdated(*child, typeName);
    }
    return count;
}

}  // namespace

size_t updateAssetInstances(Network& net, const std::string& typeName) { return updateAssetsIn(net, typeName); }

void unlockAssetInstance(Network& net, NodeId id) {
    const Node& old = net.node(id);
    if (!dynamic_cast<const AssetNode*>(&old)) throw std::runtime_error("node '" + old.name() + "' is not an asset instance");
    static const char* subnetTypes[] = {"subnet0", "subnet", "subnet2", "subnet3", "subnet4"};
    if (old.inputCount() < 0 || old.inputCount() > 4) throw std::runtime_error("subnets have at most four inputs; '" + old.name() + "' has " + std::to_string(old.inputCount()));

    std::unique_ptr<Node> fresh = NodeRegistry::instance().create(subnetTypes[old.inputCount()]);
    auto* subnet = dynamic_cast<SubnetNode*>(fresh.get());
    json5::document doc;
    if (json5::from_string(saveAsset(net, id, "unlocked"), doc)) fail("could not copy the asset contents");
    json5::value graph;
    if (!member(doc, "graph", graph)) fail("the asset has no graph");
    loadGraphBody(subnet->network(), graph);
    for (const std::string& name : old.spareNames()) subnet->addSpareFloat(name, 0.0f);
    transferParameters(old, *fresh);

    std::vector<NodeId> inputs;
    for (int i = 0; i < old.inputCount(); ++i) inputs.push_back(net.inputOf(id, i));
    net.replaceNode(id, std::move(fresh), &inputs);
}

size_t outdatedAssetInstances(const Network& net, const std::string& typeName) { return countOutdated(net, typeName); }

GraphSnapshot snapshotNetwork(const Network& net, const GraphSnapshot* previous) {
    GraphSnapshot snap;
    snap.display = net.displayNode();
    snap.fps = net.fps();
    for (NodeId id : net.nodeIds()) {
        const Node& n = net.node(id);
        NodeSnapshot s;
        s.type = n.typeName();
        s.name = n.name();
        s.spare = n.spareNames();
        for (int i = 0; i < n.inputCount(); ++i) s.inputs.push_back(net.inputOf(id, i));

        json5::document doc;
        json5::builder b(doc);
        writeNode(b, net, id, false);
        std::string text = dump(doc);
        const NodeSnapshot* old = nullptr;
        if (previous) {
            auto it = previous->nodes.find(id);
            if (it != previous->nodes.end()) old = &it->second;
        }
        if (old && *old->text == text) s.text = old->text;
        else s.text = std::make_shared<const std::string>(std::move(text));
        snap.nodes.emplace(id, std::move(s));
    }
    return snap;
}

NodeClip copyNode(const Network& net, NodeId id) {
    const Node& n = net.node(id);
    NodeClip clip;
    clip.type = n.typeName();
    clip.name = n.name();
    for (int i = 0; i < n.inputCount(); ++i) {
        const NodeId in = net.inputOf(id, i);
        clip.inputNames.push_back(in == invalidNodeId ? std::string() : net.node(in).name());
    }
    json5::document doc;
    json5::builder b(doc);
    writeNode(b, net, id, false);
    clip.text = std::make_shared<const std::string>(dump(doc));
    return clip;
}

std::vector<NodeId> pasteNodes(Network& net, const std::vector<NodeClip>& clips) {
    std::vector<NodeId> created;
    try {
        std::map<std::string, size_t> indexOf;
        std::map<std::string, std::string> renames;
        for (size_t i = 0; i < clips.size(); ++i) {
            json5::document doc;
            if (json5::from_string(*clips[i].text, doc)) fail("the copied node is corrupt");
            const NodeId id = net.addNode(clips[i].type, net.uniqueName(clips[i].name));
            created.push_back(id);
            applyNodeBody(net.node(id), doc, true);
            indexOf[clips[i].name] = i;
            if (net.node(id).name() != clips[i].name) renames[clips[i].name] = net.node(id).name();
        }
        for (size_t i = 0; i < clips.size(); ++i) {
            if (!renames.empty()) net.rewriteReferences(created[i], renames);
            for (size_t slot = 0; slot < clips[i].inputNames.size(); ++slot) {
                const std::string& source = clips[i].inputNames[slot];
                if (source.empty()) continue;
                const auto inside = indexOf.find(source);
                const NodeId from = inside != indexOf.end() ? created[inside->second] : net.findNode(source);
                if (from != invalidNodeId && static_cast<int>(slot) < net.node(created[i]).inputCount()) net.connect(from, created[i], static_cast<int>(slot));
            }
        }
    } catch (...) {
        for (NodeId id : created) net.removeNode(id);
        throw;
    }
    return created;
}

namespace {

bool sameText(const NodeSnapshot& a, const NodeSnapshot& b) { return a.text == b.text || *a.text == *b.text; }

bool sameMeta(const NodeSnapshot& a, const NodeSnapshot& b) { return a.type == b.type && a.name == b.name && a.spare == b.spare; }

}  // namespace

bool sameGraph(const GraphSnapshot& a, const GraphSnapshot& b) {
    if (a.display != b.display || a.fps != b.fps || a.nodes.size() != b.nodes.size()) return false;
    for (auto ia = a.nodes.begin(), ib = b.nodes.begin(); ia != a.nodes.end(); ++ia, ++ib) {
        if (ia->first != ib->first || !sameMeta(ia->second, ib->second) || ia->second.inputs != ib->second.inputs || !sameText(ia->second, ib->second))
            return false;
    }
    return true;
}

RestoreStats restoreSnapshot(Network& net, const GraphSnapshot& target, const GraphSnapshot* current) {
    GraphSnapshot owned;
    if (!current) {
        owned = snapshotNetwork(net);
        current = &owned;
    }
    RestoreStats stats;
    std::vector<NodeId> toAdd, toUpdate;
    for (const auto& [id, c] : current->nodes) {
        auto it = target.nodes.find(id);
        if (it == target.nodes.end()) {
            net.removeNode(id);
            ++stats.removed;
        } else if (!sameMeta(c, it->second)) {
            net.removeNode(id);
            toAdd.push_back(id);
            ++stats.rebuilt;
        } else if (!sameText(c, it->second)) {
            toUpdate.push_back(id);
        }
    }
    for (const auto& entry : target.nodes) {
        if (!current->nodes.count(entry.first)) {
            toAdd.push_back(entry.first);
            ++stats.added;
        }
    }

    auto parse = [](const NodeSnapshot& s, json5::document& doc) {
        if (json5::from_string(*s.text, doc)) fail("a stored node snapshot is corrupt");
    };
    for (NodeId id : toAdd) {
        const NodeSnapshot& t = target.nodes.at(id);
        json5::document doc;
        parse(t, doc);
        net.addNodeWithId(id, t.type, t.name);
        applyNodeBody(net.node(id), doc, true);
    }
    for (NodeId id : toUpdate) {
        json5::document doc;
        parse(target.nodes.at(id), doc);
        Node& n = net.node(id);
        for (const std::string& name : n.floatNames()) n.clearAnimation(name);
        applyNodeBody(n, doc, false);
        ++stats.updated;
    }

    std::vector<std::tuple<NodeId, int, NodeId>> connects;
    for (const auto& [id, t] : target.nodes) {
        const size_t slots = std::max(t.inputs.size(), static_cast<size_t>(net.node(id).inputCount()));
        for (size_t i = 0; i < slots; ++i) {
            const NodeId wanted = i < t.inputs.size() ? t.inputs[i] : invalidNodeId;
            const NodeId live = net.inputOf(id, static_cast<int>(i));
            if (live == wanted) continue;
            if (live != invalidNodeId) net.disconnect(id, static_cast<int>(i));
            if (wanted != invalidNodeId) connects.emplace_back(wanted, static_cast<int>(i), id);
            ++stats.rewired;
        }
    }
    for (const auto& [from, input, to] : connects) net.connect(from, to, input);

    if (net.displayNode() != target.display) net.setDisplayNode(target.display);
    if (net.fps() != target.fps) {
        net.setFps(target.fps);
        for (NodeId id : net.nodeIds()) net.markDirty(id);
    }
    return stats;
}

void saveNetworkToFile(const Network& net, const std::string& path) { writeFile(path, saveNetwork(net)); }

void loadNetworkFromFile(Network& net, const std::string& path) { loadNetwork(net, readFile(path)); }

}  // namespace jitnodes
