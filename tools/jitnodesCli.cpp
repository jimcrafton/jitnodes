#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "jitnodes/geo/svgExport.h"
#include "jitnodes/graph/serialize.h"
#include "jitnodes/jitnodes.h"

using namespace jitnodes;

namespace {

void usage() {
    std::puts(
        "usage: jitnodesCli <graph.json> [options]\n"
        "       jitnodesCli --types\n"
        "options:\n"
        "  --node NAME   cook this node instead of the display node\n"
        "  --time T      playhead in seconds\n"
        "  --threads N   total threads (default: hardware)\n"
        "  --asset FILE  register an asset definition before loading (repeatable)\n"
        "  --isa NAME    JIT instruction set: sse2, avx2 (default) or avx512\n"
        "  --svg FILE    write the cooked geometry as SVG\n"
        "  --points      draw points in the SVG\n"
        "  --save FILE   write the graph back out (normalises formatting)\n"
        "  --profile     print per-node timings\n"
        "  --types       list registered node types");
}

}  // namespace

int main(int argc, char** argv) {
    std::string graphPath, nodeName, svgPath, savePath, isaName_;
    std::vector<std::string> assetPaths;
    double time = 0.0;
    bool haveTime = false, profile = false, points = false, listTypes = false;
    size_t threads = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", arg.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--node") nodeName = value();
        else if (arg == "--time") { time = std::atof(value().c_str()); haveTime = true; }
        else if (arg == "--threads") threads = static_cast<size_t>(std::atoi(value().c_str()));
        else if (arg == "--isa") isaName_ = value();
        else if (arg == "--asset") assetPaths.push_back(value());
        else if (arg == "--svg") svgPath = value();
        else if (arg == "--save") savePath = value();
        else if (arg == "--points") points = true;
        else if (arg == "--profile") profile = true;
        else if (arg == "--types") listTypes = true;
        else if (arg == "--help" || arg == "-h") { usage(); return 0; }
        else if (!arg.empty() && arg[0] == '-') { std::fprintf(stderr, "unknown option %s\n", arg.c_str()); usage(); return 2; }
        else graphPath = arg;
    }

    try {
        Network net(threads);
        if (listTypes) {
            for (const std::string& type : NodeRegistry::instance().typeNames()) std::puts(type.c_str());
            return 0;
        }
        if (graphPath.empty()) {
            usage();
            return 2;
        }

        if (!isaName_.empty()) {
            Isa isa;
            if (isaName_ == "sse2") isa = Isa::sse2;
            else if (isaName_ == "avx2") isa = Isa::avx2;
            else if (isaName_ == "avx512") isa = Isa::avx512;
            else throw std::runtime_error("unknown --isa '" + isaName_ + "' (use sse2, avx2 or avx512)");
            net.compiler().setIsa(isa);
        }
        for (const std::string& path : assetPaths) registerAssetFromFile(path);
        loadNetworkFromFile(net, graphPath);
        if (haveTime) net.setTime(time);
        if (!savePath.empty()) saveNetworkToFile(net, savePath);

        NodeId target = net.displayNode();
        if (!nodeName.empty()) {
            target = net.findNode(nodeName);
            if (target == invalidNodeId) throw std::runtime_error("no node named '" + nodeName + "'");
        }
        if (target == invalidNodeId) throw std::runtime_error("the graph has no display node; use --node NAME");

        GeometryHandle geo = net.cook(target);
        std::printf("%s: %zu points, %zu primitives, time %g, %zu thread(s), jit %s\n", net.node(target).name().c_str(), geo->pointCount(),
                    geo->primitiveCount(), net.time(), net.threadCount(),
                    net.compiler().jitSupported() ? jitnodes::isaName(net.compiler().isa()) : "unavailable (interpreter)");
        for (AttribClass cls : {AttribClass::point, AttribClass::primitive}) {
            for (const auto& [name, array] : geo->attribs(cls))
                std::printf("  %s attribute %s (%s)\n", cls == AttribClass::point ? "point" : "primitive", name.c_str(),
                            array.type() == AttribType::int32 ? "int" : "float");
        }

        if (!svgPath.empty()) {
            SvgOptions options;
            options.drawPoints = points;
            std::ofstream out(svgPath, std::ios::binary);
            if (!out) throw std::runtime_error("cannot open '" + svgPath + "' for writing");
            out << geometryToSvg(*geo, options);
            std::printf("wrote %s\n", svgPath.c_str());
        }
        if (profile) std::printf("\n%s", net.profileReport().c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
