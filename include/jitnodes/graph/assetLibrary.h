#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "jitnodes/graph/assetDefinition.h"
#include "jitnodes/graph/node.h"

namespace jitnodes {

class Network;

// What a library browser shows about one registered asset.
struct AssetEntry {
    std::string name;
    std::string label;
    std::string category;
    int version = 0;
    int inputs = 0;
    std::vector<std::string> params;
    std::string source;      // the file it was last installed from or saved to
    bool embedded = false;   // no file: it only arrived embedded in a graph file, or was applied but not saved
    size_t instances = 0;    // in the network given to list(), nested subnets and assets included
    size_t outdated = 0;     // of those, built from an older registration
};

// The registered assets, with where each came from. A facade over NodeRegistry (which keeps the definition text, version and revision):
// the library adds the source files, uninstalling, and change notification. Process-global like the registry; not thread safe.
class AssetLibrary {
public:
    static AssetLibrary& instance();

    // Entries sorted by name; `net` fills the instance counts.
    std::vector<AssetEntry> list(const Network* net = nullptr) const;
    bool has(const std::string& name) const;
    // A copy of the registered definition, to edit and then apply() or save(). Throws std::runtime_error for an unknown name.
    AssetDefinition definition(const std::string& name) const;
    std::string source(const std::string& name) const;

    // Registers the definitions in a file (or every *.asset.json of a directory) and remembers the file as the source. Throws std::runtime_error
    // (installFile) or returns the first error and goes on with the other files (installDirectory; a missing directory does nothing).
    void installFile(const std::string& path);
    std::string installDirectory(const std::string& directory, size_t* installed = nullptr);
    // Reads the source file again. Throws when there is none or it no longer reads.
    void reload(const std::string& name);

    // Registers the definition in memory only (the dialog's Apply). The asset keeps its source file; a new one has none until saved. Throws
    // std::runtime_error with validate()'s message when the definition is not valid, changing nothing.
    void apply(const AssetDefinition& definition);
    // Validates, writes the file, registers, and sets the source. The version becomes one more than the registered one when it is not higher
    // already, so instances built from the old text show as outdated. `definition` is updated with the version and source that were written.
    void save(AssetDefinition& definition, const std::string& path);

    // Removes the asset from the registry. With a network, refuses (std::runtime_error) while it still has instances of it. The file is not deleted.
    void uninstall(const std::string& name, const Network* net = nullptr);

    // Rebuilds the instances in `net` that were made from an older registration (all assets, or one); returns how many. Not undoable.
    size_t updateInstances(Network& net, const std::string& name = {});

    // Called after the set of assets or a definition changed. Returns an id for removeListener.
    int addListener(std::function<void()> listener);
    void removeListener(int id);

private:
    AssetLibrary() = default;
    void notify();

    std::map<std::string, std::string> sources_;
    std::vector<std::pair<int, std::function<void()>>> listeners_;
    int nextListener_ = 1;
};

}  // namespace jitnodes
