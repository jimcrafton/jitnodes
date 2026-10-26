#include <algorithm>
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

// A recorded trail: every point of the current frame of a baked sequence gets a polyline through its positions in the `frames` earlier
// frames (`step` frames apart). Points are matched by index. A frame that is missing or before the start repeats the newer one, and a
// point an older frame does not have starts where the newer frame has it, so a trail never jumps.
class FileTrailNode : public Node {
public:
    FileTrailNode() : Node("fileTrail", 0) {
        declareString("path", "");
        declareFloat("frames", 8.0f);
        declareFloat("step", 1.0f);
        declareFloat("fade", 1.0f);
    }

    bool dependsOnPlayhead() const override { return true; }

    GeometryHandle cook(CookContext& ctx, const std::vector<GeometryHandle>&) override {
        const std::string& pattern = getString("path");
        if (!hasFrameToken(pattern)) throw std::runtime_error(name() + ": path needs a $F token");
        const size_t segments = static_cast<size_t>(std::clamp(getFloat("frames"), 1.0f, 1000.0f));
        const long long step = std::max<long long>(std::llround(getFloat("step")), 1);
        const float fade = std::clamp(getFloat("fade"), 0.0f, 1.0f);
        const double fps = ctx.network ? ctx.network->fps() : 24.0;
        const long long frame = std::llround(ctx.time * fps);

        const std::string headPath = expandFrame(pattern, frame);
        Geometry head;
        try {
            head = loadGeometryFromFile(headPath);
        } catch (const std::runtime_error& e) {
            throw std::runtime_error(name() + ": " + headPath + ": " + e.what());
        }
        const size_t n = head.pointCount(), per = segments + 1;
        if (n * per > 100'000'000) throw std::runtime_error(name() + ": more than 100 million points; lower frames");

        auto geo = std::make_shared<Geometry>();
        geo->resizePoints(n * per);
        for (const auto& entry : head.attribs(AttribClass::point)) {
            if (entry.first == "x" || entry.first == "y") continue;
            AttribArray& target = geo->attrib(AttribClass::point, entry.first, entry.second.type());
            for (size_t p = 0; p < n; ++p)
                for (size_t k = 0; k < per; ++k) {
                    const size_t i = p * per + k;
                    if (entry.second.type() == AttribType::float32) target.floats()[i] = entry.second.floats()[p];
                    else target.ints()[i] = entry.second.ints()[p];
                }
        }
        for (const auto& entry : head.details()) geo->setDetail(entry.first, entry.second);

        float* xs = geo->x();
        float* ys = geo->y();
        float* u = geo->attrib(AttribClass::point, "u").floats().data();
        int32_t* trail = geo->attrib(AttribClass::point, "trail", AttribType::int32).ints().data();
        float* alpha = geo->attrib(AttribClass::point, "alpha").floats().data();
        if (!head.findAttrib(AttribClass::point, "alpha")) std::fill(alpha, alpha + n * per, 1.0f);

        std::vector<float> px(std::as_const(head).x(), std::as_const(head).x() + n), py(std::as_const(head).y(), std::as_const(head).y() + n);
        for (size_t k = 0; k < per; ++k) {
            if (k > 0 && frame - static_cast<long long>(k) * step >= 0) {
                const std::string path = expandFrame(pattern, frame - static_cast<long long>(k) * step);
                if (std::filesystem::exists(path)) {
                    try {
                        const Geometry older = loadGeometryFromFile(path);
                        const size_t m = std::min(older.pointCount(), n);
                        std::copy(std::as_const(older).x(), std::as_const(older).x() + m, px.begin());
                        std::copy(std::as_const(older).y(), std::as_const(older).y() + m, py.begin());
                    } catch (const std::runtime_error& e) {
                        throw std::runtime_error(name() + ": " + path + ": " + e.what());
                    }
                }
            }
            const float t = static_cast<float>(k) / static_cast<float>(segments);
            for (size_t p = 0; p < n; ++p) {
                const size_t i = p * per + k;
                xs[i] = px[p];
                ys[i] = py[p];
                u[i] = t;
                trail[i] = static_cast<int32_t>(p);
                alpha[i] *= 1.0f - fade * t;
            }
        }
        for (size_t p = 0; p < n; ++p) geo->addPrimitive(Primitive{static_cast<uint32_t>(p * per), static_cast<uint32_t>(per), false, PrimKind::polyline});
        return geo;
    }
};

template <class T>
void add(const char* typeName) {
    NodeRegistry::instance().registerType(typeName, [] { return std::make_unique<T>(); });
}

}  // namespace

void registerFileNodes() {
    add<FileReadNode>("fileRead");
    add<FileCacheNode>("fileCache");
    add<FileTrailNode>("fileTrail");
}

}  // namespace jitnodes
