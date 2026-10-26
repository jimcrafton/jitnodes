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
        "       jitnodesCli --geo-in FILE.jngeo [--svg FILE] [--png FILE] [--size WxH]\n"
        "       jitnodesCli --types\n"
        "options:\n"
        "  --geo-out FILE  write the cooked geometry as a .jngeo file (see docs/geometryFiles.md)\n"
        "  --geo-in FILE   use a .jngeo file instead of a graph (for --svg and --png)\n"
        "  --node NAME   cook this node instead of the display node\n"
        "  --time T      playhead in seconds\n"
        "  --threads N   total threads (default: hardware)\n"
        "  --asset FILE  register an asset definition before loading (repeatable)\n"
        "  --isa NAME    JIT instruction set: sse2, avx2 (default) or avx512\n"
        "  --svg FILE    write the cooked geometry as SVG\n"
        "  --points      draw points in the SVG\n"
        "  --png FILE    render the geometry's points as sprites to a PNG (settings from a render node, see docs/fx.md)\n"
        "  --size WxH    PNG size in pixels (default: the render node's width and height)\n"
        "  --save FILE   write the graph back out (normalises formatting)\n"
        "  --profile     print per-node timings\n"
        "  --trace FILE  write the cook as Chrome trace JSON (chrome://tracing, Perfetto)\n"
        "  --types       list registered node types");
}

}  // namespace

int main(int argc, char** argv) {
    std::string graphPath, nodeName, svgPath, pngPath, sizeText, savePath, isaName_, geoOutPath, geoInPath, tracePath;
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
        else if (arg == "--geo-out") geoOutPath = value();
        else if (arg == "--geo-in") geoInPath = value();
        else if (arg == "--svg") svgPath = value();
        else if (arg == "--png") pngPath = value();
        else if (arg == "--size") sizeText = value();
        else if (arg == "--save") savePath = value();
        else if (arg == "--points") points = true;
        else if (arg == "--profile") profile = true;
        else if (arg == "--trace") tracePath = value();
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
        if (graphPath.empty() && geoInPath.empty()) {
            usage();
            return 2;
        }

        GeometryHandle geo;
        if (!geoInPath.empty()) {
            geo = std::make_shared<Geometry>(loadGeometryFromFile(geoInPath));
            std::printf("%s: %zu points, %zu primitives\n", geoInPath.c_str(), geo->pointCount(), geo->primitiveCount());
        } else {
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

            net.setTraceEnabled(!tracePath.empty());
            geo = net.cook(target);
            if (!tracePath.empty()) {
                net.writeTrace(tracePath);
                std::printf("wrote %s (%zu events)\n", tracePath.c_str(), net.traceEventCount());
            }
            std::printf("%s: %zu points, %zu primitives, time %g, %zu thread(s), jit %s\n", net.node(target).name().c_str(), geo->pointCount(),
                        geo->primitiveCount(), net.time(), net.threadCount(),
                        net.compiler().jitSupported() ? jitnodes::isaName(net.compiler().isa()) : "unavailable (interpreter)");
        }
        if (!geoOutPath.empty()) {
            saveGeometryToFile(*geo, geoOutPath);
            std::printf("wrote %s\n", geoOutPath.c_str());
        }
        for (AttribClass cls : {AttribClass::point, AttribClass::primitive}) {
            for (const auto& [name, array] : geo->attribs(cls))
                std::printf("  %s attribute %s (%s)\n", cls == AttribClass::point ? "point" : "primitive", name.c_str(),
                            array.type() == AttribType::string ? "string" : array.type() == AttribType::int32 ? "int" : "float");
        }

        if (!svgPath.empty()) {
            SvgOptions options;
            options.drawPoints = points;
            std::ofstream out(svgPath, std::ios::binary);
            if (!out) throw std::runtime_error("cannot open '" + svgPath + "' for writing");
            out << geometryToSvg(*geo, options);
            std::printf("wrote %s\n", svgPath.c_str());
        }
        if (!pngPath.empty()) {
            int width = 0, height = 0;
            if (!sizeText.empty() && std::sscanf(sizeText.c_str(), "%dx%d", &width, &height) != 2)
                throw std::runtime_error("--size expects WIDTHxHEIGHT, got '" + sizeText + "'");
            const RenderSettings settings = renderSettingsFromDetails(*geo, width, height);
            writePng(pngPath, renderSplats(*geo, settings, net.pool()));
            std::printf("wrote %s (%dx%d)\n", pngPath.c_str(), settings.width, settings.height);
        }
        if (profile) std::printf("\n%s", net.profileReport().c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
