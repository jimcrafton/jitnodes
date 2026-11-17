#include "jitnodes/graph/assetLibrary.h"

#include "jitnodes/graph/network.h"
#include "jitnodes/graph/serialize.h"
#include "jitnodes/graph/subnet.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace jitnodes {

namespace {

std::string readFileText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open '" + path + "'");
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFileText(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot open '" + path + "' for writing");
    }
    out << text;
    if (!out) {
        throw std::runtime_error("failed writing '" + path + "'");
    }
}

void countInstances(const Network& net, const std::string& name, size_t& instances, size_t& outdated) {
    for (NodeId id : net.nodeIds()) {
        const Node& n = net.node(id);
        if (n.typeName() == name) {
            ++instances;
            const auto* asset = dynamic_cast<const AssetNode*>(&n);
            if (asset && asset->revision() != NodeRegistry::instance().assetRevision(name)) {
                ++outdated;
            }
        }
        if (const Network* child = n.childNetwork()) {
            if (!n.isAssetInstance()) {
                countInstances(*child, name, instances, outdated);
            }
        }
    }
}

}  // namespace

AssetLibrary& AssetLibrary::instance() {
    static AssetLibrary library;
    return library;
}

bool AssetLibrary::has(const std::string& name) const { return NodeRegistry::instance().assetText(name) != nullptr; }

std::string AssetLibrary::source(const std::string& name) const {
    const auto it = sources_.find(name);
    return it == sources_.end() ? std::string() : it->second;
}

AssetDefinition AssetLibrary::definition(const std::string& name) const {
    const std::string* text = NodeRegistry::instance().assetText(name);
    if (!text) {
        throw std::runtime_error("no asset named '" + name + "'");
    }
    AssetDefinition d = AssetDefinition::fromText(*text);
    d.source = source(name);
    return d;
}

std::vector<AssetEntry> AssetLibrary::list(const Network* net) const {
    std::vector<AssetEntry> entries;
    std::vector<std::string> names = NodeRegistry::instance().assetNames();
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        AssetEntry e;
        e.name = name;
        e.version = NodeRegistry::instance().assetVersion(name);
        try {
            const AssetDefinition d = definition(name);
            e.label = d.label;
            e.category = d.category;
            e.inputs = d.inputs;
            for (const AssetParam& p : d.params) {
                e.params.push_back(p.name);
            }
        } catch (const std::runtime_error&) {
        }
        e.source = source(name);
        e.embedded = e.source.empty();
        if (net) {
            countInstances(*net, name, e.instances, e.outdated);
        }
        entries.push_back(std::move(e));
    }
    return entries;
}

void AssetLibrary::installFile(const std::string& path) {
    const std::string text = readFileText(path);
    const AssetDefinition d = AssetDefinition::fromText(text);
    registerAsset(text);
    sources_[d.name] = path;
    notify();
}

std::string AssetLibrary::installDirectory(const std::string& directory, size_t* installed) {
    std::string firstError;
    size_t count = 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return firstError;
    }
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.size() > 11 && name.compare(name.size() - 11, 11, ".asset.json") == 0) {
            files.push_back(entry.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    for (const std::string& file : files) {
        try {
            installFile(file);
            ++count;
        } catch (const std::runtime_error& e) {
            if (firstError.empty()) {
                firstError = file + ": " + e.what();
            }
        }
    }
    if (installed) {
        *installed = count;
    }
    return firstError;
}

void AssetLibrary::reload(const std::string& name) {
    const std::string path = source(name);
    if (path.empty()) {
        throw std::runtime_error("asset '" + name + "' has no file to reload");
    }
    installFile(path);
}

void AssetLibrary::apply(const AssetDefinition& definition) {
    const std::string problem = definition.validate();
    if (!problem.empty()) {
        throw std::runtime_error(problem);
    }
    registerAsset(definition.toText());
    if (!definition.source.empty()) {
        sources_[definition.name] = definition.source;
    }
    notify();
}

void AssetLibrary::save(AssetDefinition& definition, const std::string& path) {
    const int registered = NodeRegistry::instance().assetVersion(definition.name);
    AssetDefinition next = definition;
    if (NodeRegistry::instance().assetText(next.name) && registered >= next.version) {
        next.version = registered + 1;
    }
    const std::string problem = next.validate();
    if (!problem.empty()) {
        throw std::runtime_error(problem);
    }
    next.source = path;
    const std::string text = next.toText();
    writeFileText(path, text);
    registerAsset(text);
    sources_[next.name] = path;
    definition = std::move(next);
    notify();
}

void AssetLibrary::uninstall(const std::string& name, const Network* net) {
    if (!has(name)) {
        throw std::runtime_error("no asset named '" + name + "'");
    }
    if (net) {
        size_t instances = 0;
        size_t outdated = 0;
        countInstances(*net, name, instances, outdated);
        if (instances > 0) {
            throw std::runtime_error("asset '" + name + "' still has " + std::to_string(instances) + " instance(s) in the graph");
        }
    }
    NodeRegistry::instance().unregisterAsset(name);
    sources_.erase(name);
    notify();
}

size_t AssetLibrary::updateInstances(Network& net, const std::string& name) { return updateAssetInstances(net, name); }

int AssetLibrary::addListener(std::function<void()> listener) {
    const int id = nextListener_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

void AssetLibrary::removeListener(int id) {
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [id](const auto& l) { return l.first == id; }), listeners_.end());
}

void AssetLibrary::notify() {
    const auto copy = listeners_;
    for (const auto& listener : copy) {
        listener.second();
    }
}

}  // namespace jitnodes
