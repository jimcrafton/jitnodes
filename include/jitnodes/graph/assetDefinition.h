#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "jitnodes/graph/node.h"

namespace jitnodes {

class Network;

// One promoted parameter of an asset: what an instance starts with and what an editor shows for it. The limits are what a typed or dragged value is
// kept within (see Node::floatMinimum); they do not apply when the node cooks.
struct AssetParam {
    std::string name;
    std::string label;      // empty: show the name
    float defaultValue = 0.0f;
    std::optional<float> minimum;
    std::optional<float> maximum;
    float step = 0.0f;      // how much a drag moves the value per pixel; 0 leaves it to the editor
    std::string group;      // parameters with the same group are shown together
    std::string tooltip;
    bool ramp = false;           // a ramp parameter: defaultRamp is its default text; the number members do not apply
    std::string defaultRamp;
    std::uint64_t serial = 0;    // neither saved nor compared: lets a binding find this parameter again after the list moved
    bool operator==(const AssetParam& o) const {
        return name == o.name && label == o.label && defaultValue == o.defaultValue && minimum == o.minimum && maximum == o.maximum && step == o.step &&
               group == o.group && tooltip == o.tooltip && ramp == o.ramp && defaultRamp == o.defaultRamp;
    }
};

// The names an asset's scripts may use (see docs/assets.md, "Scripts"): the shared module and the event handlers. The core only stores the text and
// checks the names; running them is up to the application.
const std::vector<std::string>& assetScriptNames();

// An asset definition as a value: everything the definition file holds, editable, and independent of the registry. Copy it, change it, then
// register it (AssetLibrary::apply / save). Keys of the file this version does not know are kept and written back, so a newer file survives an
// older reader.
struct AssetDefinition {
    std::string name;              // the node type name
    std::string label;
    std::string category;
    std::string description;
    std::string author;
    std::string icon;
    std::string help;
    int version = 1;               // the asset's own version (assetVersion in the file)
    int inputs = 1;
    std::vector<std::string> inputLabels;   // empty, or one per input
    std::vector<AssetParam> params;
    std::map<std::string, std::string> renames;   // old parameter name -> new name, accumulated over the versions
    std::map<std::string, std::string> scripts;   // assetScriptNames() -> source
    std::map<std::string, std::string> files;     // extra files: name -> text (binary files are not supported yet)
    std::string graphText;         // the contents: the JSON of a graph body (what a file's "graph" member holds)
    std::string source;            // the file it was read from or saved to; empty when it only exists in memory or embedded in a graph file
    std::map<std::string, std::string> unknown;   // members this version does not know: key -> JSON text

    // Builds a definition from a subnet: contents, input count and promoted parameters (with a "group:" parameter tag as the group).
    // Throws std::runtime_error when the node is not a subnet.
    static AssetDefinition fromNode(const Network& net, NodeId subnet, const std::string& typeName, int assetVersion = 1);
    // Parses a definition file's text (JSON5). Throws std::runtime_error with the reason for text that is not an asset definition.
    static AssetDefinition fromText(const std::string& text);
    std::string toText() const;

    // An empty string when the definition can be registered, else the first problem: the name (letters, digits, underscore, not a built-in node),
    // the input count (0 to 16), parameters (names, duplicates, limits), renames, script names, the contents (by building one instance), and
    // finally the script validator, if the application installed one. Does not change the registry, except that assets the contents embed are
    // registered the way loading a graph file does.
    std::string validate() const;

    // Replaces the contents and promoted parameters of an unlocked subnet with this definition's (the "match node" operation). Throws
    // std::runtime_error when the node is not a subnet. Not allowed while the network cooks.
    void applyTo(Network& net, NodeId subnet) const;

    // Renames a promoted parameter of the definition and rewrites `parent.<from>` in the contents (through a scratch subnet, see
    // renameSpareParameter). With `recordRename` the old name goes into `renames` (earlier entries that ended in it follow), so instances of older
    // versions keep their values; pass false for a parameter that no registered version has had. Throws std::runtime_error (changing nothing) when
    // `from` is not a parameter, `to` is not a valid name or is taken. Returns the number of expressions rewritten.
    size_t renameParam(const std::string& from, const std::string& to, bool recordRename = true);

    // The application's check of the scripts (the Python editor installs one that parses them and applies the rules of each event). Gets the
    // definition, returns "" or a message that names the script and line. Called last by validate().
    using ScriptValidator = std::function<std::string(const AssetDefinition&)>;
    static void setScriptValidator(ScriptValidator validator);
};

// Renames a promoted parameter of a subnet and rewrites `parent.<from>` in the expressions of the nodes inside it, so the contents keep
// working. Throws std::runtime_error (changing nothing) when `from` is not a promoted parameter, `to` is not a valid name or is taken.
// Returns the number of expressions rewritten.
size_t renameSpareParameter(Network& net, NodeId subnet, const std::string& from, const std::string& to);

}  // namespace jitnodes
