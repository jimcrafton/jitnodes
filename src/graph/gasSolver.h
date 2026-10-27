#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "jitnodes/core/threadPool.h"
#include "jitnodes/graph/node.h"

namespace jitnodes {

struct GasConfig {
    float minX = -10.0f, maxX = 10.0f, minY = -6.0f, maxY = 6.0f;
    int resolution = 64;  // cells along x; cells are square
    float viscosity = 0.0f, dissipation = 0.2f, cooling = 0.5f;
    float buoyancy = 0.0f, weight = 0.0f, vorticity = 5.0f;
    int iterations = 20;
    bool open = false;  // gas leaves through the walls instead of piling up against them
    bool redBlack = false;  // red-black Gauss-Seidel: order independent within a colour, so it threads on big grids; results differ slightly
};

// A gas injected over a disc: `density` and `heat` per second at full weight, and the velocity the gas is blended towards.
struct GasSource {
    float x = 0.0f, y = 0.0f, radius = 0.5f;
    float density = 0.0f, heat = 0.0f, vx = 0.0f, vy = 0.0f;
};

// Jos Stam's stable fluids on a collocated grid (semi-Lagrangian advection, Gauss-Seidel diffusion and pressure projection) with
// Fedkiw's vorticity confinement, buoyancy from `temperature` and weight from `density`. Serial and deterministic.
class GasSolver {
public:
    void reset(const GasConfig& config);
    bool ready() const { return nx_ > 0; }
    size_t bytes() const { return (u_.capacity() + v_.capacity() + d_.capacity() + t_.capacity() + u0_.capacity() + v0_.capacity() + p_.capacity() + div_.capacity() + w_.capacity() + tmp_.capacity()) * sizeof(float); }
    int width() const { return nx_; }
    int height() const { return ny_; }
    float cellSize() const { return h_; }
    float cellX(int i) const { return config_.minX + (static_cast<float>(i) + 0.5f) * h_; }
    float cellY(int j) const { return config_.minY + (static_cast<float>(j) + 0.5f) * h_; }

    void addSource(const GasSource& source, float dt);
    // A moving body: density, and a share `push` (per second) of its velocity given to the gas around it.
    void addBody(float x, float y, float vx, float vy, float density, float push, float dt);
    // With `pool` and config.redBlack the relaxation sweeps of a large grid run on the pool; the result does not depend on the pool.
    void step(float dt, ThreadPool* pool = nullptr);

    // Bilinear samples at a world position; outside the domain everything is 0.
    void sample(float x, float y, float& vx, float& vy, float& density) const;

    float velocityX(int i, int j) const { return u_[at(i + 1, j + 1)]; }
    float velocityY(int i, int j) const { return v_[at(i + 1, j + 1)]; }
    float density(int i, int j) const { return d_[at(i + 1, j + 1)]; }
    float temperature(int i, int j) const { return t_[at(i + 1, j + 1)]; }

private:
    size_t at(int i, int j) const { return static_cast<size_t>(i) + static_cast<size_t>(nx_ + 2) * static_cast<size_t>(j); }
    void boundary(std::vector<float>& a, int kind) const;
    void project();
    void diffuse(std::vector<float>& a, const std::vector<float>& a0, float k, int kind);
    void advect(std::vector<float>& dst, const std::vector<float>& src, const std::vector<float>& uu, const std::vector<float>& vv, float dt, int kind);
    void confineVorticity(float dt);
    // Calls cell(i, j) for every interior cell: row by row, or in two checkerboard colours with rows spread over the pool.
    template <class F>
    void sweep(F&& cell);

    ThreadPool* pool_ = nullptr;
    GasConfig config_;
    int nx_ = 0, ny_ = 0;
    float h_ = 1.0f;
    std::vector<float> u_, v_, d_, t_, u0_, v0_, p_, div_, w_, tmp_;
};

// The names and defaults of the solver parameters shared by `gasField` and the gas coupling of `particles`: with a prefix
// "gas" the name `resolution` becomes `gasResolution`.
std::string gasParamName(const char* prefix, const char* base);
template <class Fn>
void forEachGasParam(const char* prefix, Fn&& fn) {
    fn(gasParamName(prefix, "resolution"), 64.0f);
    fn(gasParamName(prefix, "viscosity"), 0.0f);
    fn(gasParamName(prefix, "dissipation"), 0.2f);
    fn(gasParamName(prefix, "cooling"), 0.5f);
    fn(gasParamName(prefix, "buoyancy"), 0.0f);
    fn(gasParamName(prefix, "weight"), 0.0f);
    fn(gasParamName(prefix, "vorticity"), 5.0f);
    fn(gasParamName(prefix, "iterations"), 20.0f);
    fn(gasParamName(prefix, "open"), 0.0f);
    fn(gasParamName(prefix, "redBlack"), 0.0f);
    fn(gasParamName(prefix, "nozzleX"), 0.0f);
    fn(gasParamName(prefix, "nozzleY"), 0.0f);
    fn(gasParamName(prefix, "nozzleAngle"), 90.0f);
    fn(gasParamName(prefix, "nozzleSpeed"), 0.0f);
    fn(gasParamName(prefix, "nozzleRadius"), 0.5f);
    fn(gasParamName(prefix, "nozzleDensity"), 0.0f);
    fn(gasParamName(prefix, "nozzleHeat"), 0.0f);
}

// Reads the shared parameters (bounds are the node's boundsMin/Max). Throws with `who` in the message for bad values.
GasConfig readGasConfig(const Node& node, const char* prefix, const char* who);
// The nozzle as a source; false when it injects nothing.
bool readGasNozzle(const Node& node, const char* prefix, GasSource& out);

}  // namespace jitnodes
