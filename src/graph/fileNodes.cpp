#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "jitnodes/geo/geoFile.h"
#include "jitnodes/graph/network.h"

namespace jitnodes {

namespace {

bool hasFrameToken(const std::string& path) { return path.find("$F") != std::string::npos; }

// $F is the frame number, $F4 the frame padded with zeros to 4 digits (any digit count works).
std::string expandFrame(const std::string& path, long long frame) {
    std::string out;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] != '$' || i + 1 >= path.size() || path[i + 1] != 'F') {
            out += path[i];
            continue;
        }
        size_t j = i + 2;
        int width = 0;
        while (j < path.size() && path[j] >= '0' && path[j] <= '9' && width < 100) width = width * 10 + (path[j++] - '0');
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "%0*lld", width, frame);
        out += buffer;
        i = j - 1;
    }
    return out;
}

enum class CacheMode { automatic, read, write };

class FileReadNode : public Node {
public:
    FileReadNode() : Node("fileRead", 0) { declareString("path", ""); }

    bool dependsOnPlayhead() const override { return hasFrameToken(getString("path")); }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>&) override {
        const std::string path = pathAt(getString("path"), ctx.time, ctx.network);
        if (path.empty()) throw std::runtime_error(name() + ": path is empty");
        try {
            return std::make_shared<Geometry>(loadGeometryFromFile(path));
        } catch (const std::runtime_error& e) {
            throw std::runtime_error(name() + ": " + path + ": " + e.what());
        }
    }

    static std::string pathAt(const std::string& pattern, double time, const Network* network) {
        const double fps = network ? network->fps() : 24.0;
        return expandFrame(pattern, std::llround(time * fps));
    }
};

// Passes its input through. `write` cooks the input and saves it, `read` loads the file and never cooks the input, `auto` reads a valid file
// and otherwise cooks and saves. The file does not record what produced it: a stale cache is deleted or rewritten by the user.
class FileCacheNode : public Node {
public:
    FileCacheNode() : Node("fileCache", 1) {
        declareString("path", "");
        declareString("mode", "auto");
    }

    bool dependsOnPlayhead() const override { return hasFrameToken(getString("path")); }

    bool needsInputs() override {
        pending_.reset();
        switch (mode()) {
            case CacheMode::read: return false;
            case CacheMode::write: return true;
            case CacheMode::automatic: break;
        }
        const std::string path = currentPath();
        if (path.empty() || !std::filesystem::exists(path)) return true;
        try {
            pending_ = std::make_shared<Geometry>(loadGeometryFromFile(path));
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>& inputs) override {
        GeometryHandle loaded = std::move(pending_);
        pending_.reset();
        const std::string path = FileReadNode::pathAt(getString("path"), ctx.time, ctx.network);
        if (path.empty()) throw std::runtime_error(name() + ": path is empty");
        if (loaded) return loaded;
        if (mode() == CacheMode::read) {
            try {
                return std::make_shared<Geometry>(loadGeometryFromFile(path));
            } catch (const std::runtime_error& e) {
                throw std::runtime_error(name() + ": " + path + ": " + e.what());
            }
        }
        if (inputs.empty() || !inputs[0]) throw std::runtime_error(name() + ": input 0 is not connected");
        try {
            saveGeometryToFile(*inputs[0], path);
        } catch (const std::runtime_error& e) {
            throw std::runtime_error(name() + ": " + path + ": " + e.what());
        }
        return inputs[0];
    }

private:
    CacheMode mode() const {
        const std::string& m = getString("mode");
        if (m == "auto") return CacheMode::automatic;
        if (m == "read") return CacheMode::read;
        if (m == "write") return CacheMode::write;
        throw std::runtime_error(name() + ": mode must be auto, read or write, not '" + m + "'");
    }

    std::string currentPath() const {
        return FileReadNode::pathAt(getString("path"), owner() ? owner()->time() : 0.0, owner());
    }

    GeometryHandle pending_;
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerFileNodes() {
    add<FileReadNode>("fileRead");
    add<FileCacheNode>("fileCache");
}

}  // namespace jitnodes
