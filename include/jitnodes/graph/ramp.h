#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jitnodes {

// How the curve runs from one ramp point to the next (the interpolation of the point it leaves).
enum class RampInterp : uint8_t {
    constant,    // holds the point's value until the next point
    linear,
    smooth,      // smoothstep between the two values: flat at both points
    catmullRom,  // cubic through the points with finite-difference tangents; may overshoot
    monotone     // cubic through the points that never overshoots (Fritsch-Carlson)
};

struct RampPoint {
    float pos = 0.0f;
    float value = 0.0f;
    RampInterp interp = RampInterp::linear;
};

// A float curve over a position: a list of points sorted by position. Outside the first and last point the value of that end point holds.
// The text form is what a ramp parameter stores (Node::declareRamp): points separated by commas, semicolons or new lines, each
// `position value [interpolation]`, the interpolation one of constant, linear, smooth, catmullRom, monotone (linear when omitted):
//     0 0 monotone, 0.3 0.9 monotone, 1 1 linear
// Two points at one position make a step. Positions may lie outside 0..1.
// @scriptable ignore=true
class Ramp {
public:
    // A straight line from (0, 0) to (1, 1).
    Ramp();
    // The points are sorted by position (stable). Throws std::runtime_error when there are none or a number is not finite.
    explicit Ramp(std::vector<RampPoint> points);

    // Throws std::runtime_error with the reason (naming the point) for text that is not a ramp.
    static Ramp parse(const std::string& text);
    // "" when the text is a ramp, else the reason; for an editor that checks a typed value before storing it.
    static std::string check(const std::string& text);
    // The `name` of a ramp text of the form `parent.<name>`, which means "the ramp <name> of the subnet around the node"; "" for any other
    // text. Such a text is not a ramp by itself: parse and check refuse it, Node::rampText follows it.
    static std::string parentReference(const std::string& text);
    static const char* interpName(RampInterp interp);
    // Throws std::runtime_error for an unknown name (case and a '-' are ignored: "Catmull-Rom" is catmullRom).
    static RampInterp interpFromName(const std::string& name);

    std::string toText() const;
    const std::vector<RampPoint>& points() const { return points_; }
    size_t size() const { return points_.size(); }

    float eval(float x) const;
    // `count` (at least 2) values at evenly spaced positions from the first point to the last, for a lookup table.
    std::vector<float> bake(size_t count) const;

private:
    void prepare();

    std::vector<RampPoint> points_;
    std::vector<float> catmullTangents_;
    std::vector<float> monotoneTangents_;
};

}  // namespace jitnodes
