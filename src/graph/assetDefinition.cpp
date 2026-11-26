#include "jitnodes/graph/assetDefinition.h"

#include "jitnodes/graph/serialize.h"
#include "jitnodes/graph/serializeValue.h"
#include "jitnodes/graph/subnet.h"

#include <json5/json5.hpp>
#include <json5/json5_builder.hpp>
#include <json5/json5_input.hpp>
#include <json5/json5_output.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace jitnodes {

namespace {

constexpr const char* assetFormatName = "jitnodes-asset";

AssetDefinition::ScriptValidator& scriptValidator() {
    static AssetDefinition::ScriptValidator validator;
    return validator;
}

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("asset definition: " + message); }

std::string dumpValueText(const json5::value& v) {
    json5::writer_params params;
    params.json_compatible = true;
    std::ostringstream os;
    os << std::setprecision(9);
    json5::to_stream(os, v, params, 0);
    return os.str();
}

std::string dumpDocumentText(const json5::document& doc) {
    json5::writer_params params;
    params.json_compatible = true;
    std::ostringstream os;
    os << std::setprecision(9);
    json5::to_stream(os, doc, params);
    return os.str();
}

json5::value copyInto(json5::builder& b, const json5::value& v) {
    if (v.is_object()) {
        b.push_object();
        for (auto [key, child] : json5::object_view(v)) {
            b[key] = copyInto(b, child);
        }
        return b.pop();
    }
    if (v.is_array()) {
        b.push_array();
        for (const json5::value& child : json5::array_view(v)) {
            b += copyInto(b, child);
        }
        return b.pop();
    }
    if (v.is_string()) {
        return b.new_string(v.get_c_str());
    }
    return v;
}

void parseText(const std::string& text, json5::document& doc) {
    const json5::error err = json5::from_string(text, doc);
    if (err) {
        fail(std::string("parse error at line ") + std::to_string(err.line) + ", column " + std::to_string(err.column) + ": " +
             json5::error::type_string[err.type]);
    }
}

bool find(const json5::value& obj, const char* key, json5::value& out) {
    const json5::object_view view(obj);
    if (!view.is_valid()) {
        return false;
    }
    const auto it = view.find(key);
    if (it == view.end()) {
        return false;
    }
    out = (*it).second;
    return true;
}

std::string textOf(const json5::value& v, const std::string& what) {
    if (!v.is_string()) {
        fail(what + " must be a string");
    }
    return v.get_c_str();
}

double numberOf(const json5::value& v, const std::string& what) {
    if (!v.is_number()) {
        fail(what + " must be a number");
    }
    return v.get<double>();
}

void readText(const json5::value& obj, const char* key, std::string& out) {
    json5::value v;
    if (find(obj, key, v)) {
        out = textOf(v, key);
    }
}

std::map<std::string, std::string> readTextMap(const json5::value& obj, const char* key) {
    std::map<std::string, std::string> out;
    json5::value v;
    if (!find(obj, key, v)) {
        return out;
    }
    if (!v.is_object()) {
        fail(std::string(key) + " must be an object");
    }
    for (auto [name, value] : json5::object_view(v)) {
        out[std::string(name)] = textOf(value, std::string(key) + "." + std::string(name));
    }
    return out;
}

void writeTextMap(json5::builder& b, const char* key, const std::map<std::string, std::string>& map) {
    if (map.empty()) {
        return;
    }
    b.push_object();
    for (const auto& [name, text] : map) {
        b[name] = b.new_string(text);
    }
    b[key] = b.pop();
}

// File names are values, not keys: the reader rejects keys such as "notes.txt".
std::map<std::string, std::string> readFiles(const json5::value& obj) {
    std::map<std::string, std::string> out;
    json5::value v;
    if (!find(obj, "files", v)) {
        return out;
    }
    const json5::array_view items(v);
    if (!items.is_valid()) {
        fail("files must be an array");
    }
    for (const json5::value& item : items) {
        json5::value name;
        json5::value text;
        if (!find(item, "name", name) || !find(item, "text", text)) {
            fail("each file needs a name and a text");
        }
        out[textOf(name, "file name")] = textOf(text, "file text");
    }
    return out;
}

void writeFiles(json5::builder& b, const std::map<std::string, std::string>& files) {
    if (files.empty()) {
        return;
    }
    b.push_array();
    for (const auto& [name, text] : files) {
        b.push_object();
        b["name"] = b.new_string(name);
        b["text"] = b.new_string(text);
        b += b.pop();
    }
    b["files"] = b.pop();
}

void writeText(json5::builder& b, const char* key, const std::string& text) {
    if (!text.empty()) {
        b[key] = b.new_string(text);
    }
}

bool isIdentifier(const std::string& name) {
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) {
        return false;
    }
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            return false;
        }
    }
    return true;
}

bool isNameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; }

// `parent.<from>` as a whole reference, not as part of a longer name or of `x.parent.<from>`.
std::string replaceParentReference(const std::string& text, const std::string& from, const std::string& to) {
    const std::string needle = "parent." + from;
    std::string out;
    size_t pos = 0;
    while (true) {
        const size_t hit = text.find(needle, pos);
        if (hit == std::string::npos) {
            out.append(text, pos, std::string::npos);
            break;
        }
        const size_t end = hit + needle.size();
        const bool startsOk = hit == 0 || !isNameChar(text[hit - 1]);
        const bool endsOk = end >= text.size() || !isNameChar(text[end]);
        out.append(text, pos, hit - pos);
        out += startsOk && endsOk ? "parent." + to : needle;
        pos = end;
    }
    return out;
}

const std::set<std::string>& knownKeys() {
    static const std::set<std::string> keys = {"format",  "version", "type",    "assetVersion", "inputs",  "promote", "params", "label",
                                               "category", "description", "author", "icon",       "help",    "inputLabels", "renames",
                                               "scripts", "files",   "graph"};
    return keys;
}

}  // namespace

const std::vector<std::string>& assetScriptNames() {
    static const std::vector<std::string> names = {"module",       "onCreated",      "onVersionUpdated", "onInputChanged",
                                                   "onParamChanged", "onFrameChanged", "onDeleted",        "onInstalled"};
    return names;
}

void AssetDefinition::setScriptValidator(ScriptValidator validator) { scriptValidator() = std::move(validator); }

AssetDefinition AssetDefinition::fromNode(const Network& net, NodeId subnet, const std::string& typeName, int assetVersion) {
    return fromText(saveAsset(net, subnet, typeName, assetVersion));
}

AssetDefinition AssetDefinition::fromText(const std::string& text) {
    json5::document doc;
    parseText(text, doc);
    if (!doc.is_object()) {
        fail("the root must be an object");
    }
    json5::value v;
    if (!find(doc, "format", v) || textOf(v, "format") != assetFormatName) {
        fail("not a jitnodes asset (format is not 'jitnodes-asset')");
    }
    if (!find(doc, "version", v) || numberOf(v, "version") != 1.0) {
        fail("unsupported asset file version");
    }
    AssetDefinition d;
    if (!find(doc, "type", v)) {
        fail("type is missing");
    }
    d.name = textOf(v, "type");
    if (!find(doc, "inputs", v)) {
        fail("inputs is missing");
    }
    d.inputs = static_cast<int>(numberOf(v, "inputs"));
    if (find(doc, "assetVersion", v)) {
        d.version = static_cast<int>(numberOf(v, "assetVersion"));
    }
    readText(doc, "label", d.label);
    readText(doc, "category", d.category);
    readText(doc, "description", d.description);
    readText(doc, "author", d.author);
    readText(doc, "icon", d.icon);
    readText(doc, "help", d.help);
    if (find(doc, "inputLabels", v)) {
        const json5::array_view labels(v);
        if (!labels.is_valid()) {
            fail("inputLabels must be an array of strings");
        }
        for (const json5::value& label : labels) {
            d.inputLabels.push_back(textOf(label, "input label"));
        }
    }
    try {
        d.params = readAssetParams(doc);
    } catch (const std::runtime_error& e) {
        fail(e.what());
    }
    d.renames = readTextMap(doc, "renames");
    d.scripts = readTextMap(doc, "scripts");
    d.files = readFiles(doc);
    if (!find(doc, "graph", v) || !v.is_object()) {
        fail("the definition needs a graph object");
    }
    d.graphText = dumpValueText(v);
    for (auto [key, value] : json5::object_view(doc)) {
        if (knownKeys().count(std::string(key)) == 0) {
            d.unknown[std::string(key)] = dumpValueText(value);
        }
    }
    return d;
}

std::string AssetDefinition::toText() const {
    json5::document doc;
    json5::builder b(doc);
    b.push_object();
    b["format"] = b.new_string(assetFormatName);
    b["version"] = json5::value(1);
    b["type"] = b.new_string(name);
    b["assetVersion"] = json5::value(version);
    writeText(b, "label", label);
    writeText(b, "category", category);
    writeText(b, "description", description);
    writeText(b, "author", author);
    writeText(b, "icon", icon);
    writeText(b, "help", help);
    b["inputs"] = json5::value(inputs);
    if (!inputLabels.empty()) {
        b.push_array();
        for (const std::string& inputLabel : inputLabels) {
            b += b.new_string(inputLabel);
        }
        b["inputLabels"] = b.pop();
    }
    // "promote" stays for readers from before params existed.
    b.push_object();
    for (const AssetParam& p : params) {
        if (!p.ramp) {
            b[p.name] = json5::value(static_cast<double>(p.defaultValue));
        }
    }
    b["promote"] = b.pop();
    b["params"] = writeAssetParams(b, params);
    writeTextMap(b, "renames", renames);
    writeTextMap(b, "scripts", scripts);
    writeFiles(b, files);
    for (const auto& [key, text] : unknown) {
        json5::document part;
        parseText(text, part);
        b[key] = copyInto(b, part);
    }
    json5::document graph;
    parseText(graphText.empty() ? "{ nodes: [] }" : graphText, graph);
    b["graph"] = copyInto(b, graph);
    b.pop();
    return dumpDocumentText(doc);
}

std::string AssetDefinition::validate() const {
    if (name.empty()) {
        return "the asset needs a type name";
    }
    if (!isIdentifier(name)) {
        return "'" + name + "' is not a valid type name: use letters, digits and underscores, and start with a letter or underscore";
    }
    const NodeRegistry& registry = NodeRegistry::instance();
    if (registry.has(name) && !registry.assetText(name)) {
        return "'" + name + "' is the name of a built-in node type";
    }
    if (inputs < 0 || inputs > 16) {
        return "inputs must be between 0 and 16";
    }
    if (!inputLabels.empty() && static_cast<int>(inputLabels.size()) != inputs) {
        return "there must be one input label per input, or none";
    }
    std::set<std::string> seen;
    for (const AssetParam& p : params) {
        if (!isIdentifier(p.name)) {
            return "'" + p.name + "' is not a valid parameter name";
        }
        if (!seen.insert(p.name).second) {
            return "parameter '" + p.name + "' appears twice";
        }
        if (p.ramp) {
            const std::string problem = Ramp::check(p.defaultRamp);
            if (!problem.empty()) {
                return "parameter '" + p.name + "': " + problem;
            }
            continue;
        }
        if (p.minimum && p.maximum && *p.minimum > *p.maximum) {
            return "parameter '" + p.name + "': the minimum is above the maximum";
        }
        if ((p.minimum && p.defaultValue < *p.minimum) || (p.maximum && p.defaultValue > *p.maximum)) {
            return "parameter '" + p.name + "': the default is outside its limits";
        }
    }
    for (const auto& [from, to] : renames) {
        if (!isIdentifier(from) || !isIdentifier(to) || from == to) {
            return "rename '" + from + "' -> '" + to + "' is not valid";
        }
    }
    const std::vector<std::string>& allowed = assetScriptNames();
    for (const auto& entry : scripts) {
        if (std::find(allowed.begin(), allowed.end(), entry.first) == allowed.end()) {
            return "'" + entry.first + "' is not a script name an asset can have";
        }
    }
    if (graphText.empty()) {
        return "the asset has no contents";
    }
    std::string problem;
    try {
        problem = checkAsset(toText());
    } catch (const std::runtime_error& e) {
        problem = e.what();
    }
    if (!problem.empty()) {
        return problem;
    }
    if (const ScriptValidator& check = scriptValidator()) {
        return check(*this);
    }
    return {};
}

void AssetDefinition::applyTo(Network& net, NodeId subnet) const {
    Node& n = net.node(subnet);
    Network* child = n.childNetwork();
    if (!child || n.isAssetInstance()) {
        throw std::runtime_error("node '" + n.name() + "' is not a subnet");
    }
    loadGraphText(*child, graphText);
    std::set<std::string> numbers, ramps;
    for (const AssetParam& p : params) {
        (p.ramp ? ramps : numbers).insert(p.name);
    }
    // The ones that go first, so a name may change from a number to a ramp or back.
    for (const std::string& existing : n.spareNames()) {
        if (numbers.count(existing) == 0) {
            n.removeSpareFloat(existing);
        }
    }
    for (const std::string& existing : n.spareRampNames()) {
        if (ramps.count(existing) == 0) {
            n.removeSpareRamp(existing);
        }
    }
    for (const AssetParam& p : params) {
        if (p.ramp && !n.isSpareRamp(p.name)) {
            n.addSpareRamp(p.name, p.defaultRamp);
        } else if (!p.ramp && !n.isSpare(p.name)) {
            n.addSpareFloat(p.name, p.defaultValue);
        }
    }
}

size_t AssetDefinition::renameParam(const std::string& from, const std::string& to, bool recordRename) {
    const auto at = std::find_if(params.begin(), params.end(), [&](const AssetParam& p) { return p.name == from; });
    if (at == params.end()) {
        throw std::runtime_error("'" + from + "' is not a parameter of '" + name + "'");
    }
    if (!isIdentifier(to)) {
        throw std::runtime_error("'" + to + "' is not a valid parameter name");
    }
    if (std::any_of(params.begin(), params.end(), [&](const AssetParam& p) { return p.name == to; })) {
        throw std::runtime_error("parameter '" + to + "' already exists");
    }
    // Only the parameter names matter to the rewrite, so a subnet with the most inputs stands in for the asset.
    Network scratch(1);
    const NodeId subnet = scratch.addNode("subnet4", "scratch");
    applyTo(scratch, subnet);
    const size_t rewritten = renameSpareParameter(scratch, subnet, from, to);
    const std::string contents = fromNode(scratch, subnet, name.empty() ? std::string("scratch") : name, version).graphText;
    graphText = contents;
    at->name = to;
    if (recordRename) {
        for (auto& entry : renames) {
            if (entry.second == from) {
                entry.second = to;
            }
        }
        renames[from] = to;
    }
    return rewritten;
}

size_t renameSpareParameter(Network& net, NodeId subnet, const std::string& from, const std::string& to) {
    Node& n = net.node(subnet);
    Network* child = n.childNetwork();
    if (!child || n.isAssetInstance()) {
        throw std::runtime_error("node '" + n.name() + "' is not a subnet");
    }
    const bool ramp = n.isSpareRamp(from);
    if (!ramp && !n.isSpare(from)) {
        throw std::runtime_error("'" + from + "' is not a promoted parameter of '" + n.name() + "'");
    }
    if (!isIdentifier(to)) {
        throw std::runtime_error("'" + to + "' is not a valid parameter name");
    }
    if (n.hasFloat(to) || n.hasString(to)) {
        throw std::runtime_error("parameter '" + to + "' already exists on '" + n.name() + "'");
    }
    struct Edit {
        NodeId node;
        std::string param;
        std::string text;
    };
    std::vector<Edit> edits;
    for (NodeId id : child->nodeIds()) {
        const Node& c = child->node(id);
        for (const std::string& param : c.floatNames()) {
            const std::string& expression = c.expression(param);
            if (expression.empty()) {
                continue;
            }
            std::string rewritten = replaceParentReference(expression, from, to);
            if (rewritten != expression) {
                edits.push_back({id, param, std::move(rewritten)});
            }
        }
    }
    std::vector<Edit> rampEdits;
    if (ramp) {
        for (NodeId id : child->nodeIds()) {
            const Node& c = child->node(id);
            for (const std::string& param : c.rampNames()) {
                if (Ramp::parentReference(c.getString(param)) == from) {
                    rampEdits.push_back({id, param, "parent." + to});
                }
            }
        }
        n.renameSpareRamp(from, to);
    } else {
        n.renameSpareFloat(from, to);
    }
    for (const Edit& e : edits) {
        child->node(e.node).setExpression(e.param, e.text);
    }
    for (const Edit& e : rampEdits) {
        child->node(e.node).setString(e.param, e.text);
    }
    return edits.size() + rampEdits.size();
}

}  // namespace jitnodes
