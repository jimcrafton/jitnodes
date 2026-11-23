#pragma once

#include <future>
#include <string>

#include "jitnodes/graph/network.h"
#include "jitnodes/graph/renderer.h"

namespace jitnodes {

// Writes the image that reaches it to a PNG or EXR file. Cooking only passes the image on (so a cook never touches the disk); the file is written
// by render(), the action an application runs for a Render button or a batch, after the cook of what feeds it.
//
// Parameters: `path` ($F is the frame number, $F4 padded to 4 digits), `format` ("auto" takes it from the extension: .exr is EXR, anything else
// PNG; or "png" / "exr"), `linear` (EXR: store the colour as linear light, converting from the sRGB an 8-bit picture holds), `half` (EXR: 16-bit
// instead of 32-bit float channels), `compress` (EXR: ZIP), `alpha` (PNG: write an alpha channel with straight colour; off gives RGB over black).
// @scriptable ignore=true
class ImageOutNode : public ValueNode {
public:
    // What one write needs, read from the parameters up front so the file can be written on another thread while the node is edited.
    struct Settings {
        std::string path;
        bool exr = false;
        bool linear = true;
        bool half = false;
        bool compress = true;
        bool alpha = true;
    };

    ImageOutNode();

    PortType inputType(int) const override { return PortType::image; }
    PortType outputType() const override { return PortType::image; }
    Value cookValue(CookContext& ctx, const std::vector<Value>& inputs) override;

    // The settings for the frame at `time`; throws std::runtime_error when the path is empty or the format is not one of the known ones.
    Settings settings(double time, const Network* network) const;

    // Cooks what feeds the node (on the calling thread, as any cook) and writes the file on a worker thread. The future holds the path written, or
    // the error. Throws at once when nothing is wired in or the settings are wrong.
    std::future<std::string> render(Network& network);

    // Writes one image; creates the folder when it is missing. Safe to call from any thread.
    static void write(const RenderResult& image, const Settings& settings);
};

}  // namespace jitnodes
