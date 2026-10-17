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
#include <sstream>
#include <stdexcept>

namespace jitnodes {

namespace {

constexpr int formatVersion = 1;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("graph file: " + message); }

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

json5::value writeNode(json5::builder& b, const Network& net, NodeId id) {
    const Node& n = net.node(id);
    b.push_object();
    b["id"] = json5::value(static_cast<double>(id));
    b["type"] = b.new_string(n.typeName());
    b["name"] = b.new_string(n.name());

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

    b.push_array();
    for (int i = 0; i < n.inputCount(); ++i) b += json5::value(static_cast<double>(net.inputOf(id, i)));
    b["inputs"] = b.pop();
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

void loadNode(Network& net, const json5::value& nv, size_t index, std::vector<std::pair<NodeId, json5::value>>& wiring) {
    const std::string where = "node #" + std::to_string(index);
    if (!nv.is_object()) fail(where + " must be an object");
    const NodeId id = static_cast<NodeId>(requireNumberMember(nv, "id", where + ".id"));
    const std::string type = requireStringMember(nv, "type", where + ".type");
    const std::string name = requireStringMember(nv, "name", where + ".name");

    try {
        net.addNodeWithId(id, type, name);
        Node& n = net.node(id);
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
        wiring.emplace_back(id, nv["inputs"]);
    } catch (const std::runtime_error& e) {
        throw std::runtime_error("graph file: " + where + " ('" + name + "', " + type + "): " + e.what());
    }
}

void loadFromValue(Network& net, const json5::value& root) {
    if (!root.is_object()) fail("the root must be an object");
    if (requireStringMember(root, "format", "format") != "jitnodes") fail("not a jitnodes graph (format is not 'jitnodes')");
    const int version = static_cast<int>(requireNumberMember(root, "version", "version"));
    if (version != formatVersion) fail("unsupported version " + std::to_string(version) + " (this build reads version " + std::to_string(formatVersion) + ")");

    const json5::array_view nodes(root["nodes"]);
    if (!nodes.is_valid()) fail("nodes must be an array");

    net.clear();
    json5::value optional;
    if (member(root, "fps", optional)) net.setFps(requireNumber(optional, "fps"));

    std::vector<std::pair<NodeId, json5::value>> wiring;
    for (size_t i = 0; i < nodes.size(); ++i) loadNode(net, nodes[i], i, wiring);

    for (const auto& [id, inputs] : wiring) {
        const json5::array_view arr(inputs);
        const Node& n = net.node(id);
        const std::string where = "node '" + n.name() + "' inputs";
        if (!arr.is_valid() || arr.size() != static_cast<size_t>(n.inputCount()))
            fail(where + " must be an array of " + std::to_string(n.inputCount()) + " ids");
        for (size_t i = 0; i < arr.size(); ++i) {
            const NodeId from = static_cast<NodeId>(requireNumber(arr[i], where));
            if (from == invalidNodeId) continue;
            try {
                net.connect(from, id, static_cast<int>(i));
            } catch (const std::runtime_error& e) {
                fail("node '" + n.name() + "' input " + std::to_string(i) + ": " + e.what());
            }
        }
    }

    if (member(root, "display", optional)) {
        const NodeId display = static_cast<NodeId>(requireNumber(optional, "display"));
        if (display != invalidNodeId) {
            const auto ids = net.nodeIds();
            if (std::find(ids.begin(), ids.end(), display) == ids.end()) fail("display refers to a missing node");
            net.setDisplayNode(display);
        }
    }
    if (member(root, "time", optional)) net.setTime(requireNumber(optional, "time"));
}

}  // namespace

std::string saveNetwork(const Network& net, bool includeTime) {
    json5::document doc;
    json5::builder b(doc);
    b.push_object();
    b["format"] = b.new_string("jitnodes");
    b["version"] = json5::value(formatVersion);
    b["fps"] = json5::value(net.fps());
    if (includeTime) b["time"] = json5::value(net.time());
    b["display"] = json5::value(static_cast<double>(net.displayNode()));
    b.push_array();
    for (NodeId id : net.nodeIds()) b += writeNode(b, net, id);
    b["nodes"] = b.pop();
    b.pop();

    json5::writer_params params;
    params.json_compatible = true;
    std::ostringstream os;
    os << std::setprecision(9);
    json5::to_stream(os, doc, params);
    return os.str();
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

void saveNetworkToFile(const Network& net, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot open '" + path + "' for writing");
    out << saveNetwork(net);
    if (!out) throw std::runtime_error("failed writing '" + path + "'");
}

void loadNetworkFromFile(Network& net, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open '" + path + "'");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    loadNetwork(net, text);
}

}  // namespace jitnodes
