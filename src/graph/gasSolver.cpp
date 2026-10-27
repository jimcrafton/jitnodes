#include "gasSolver.h"

#include "nodeUtil.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace jitnodes {

namespace {

constexpr int scalarKind = 0, horizontalKind = 1, verticalKind = 2;
constexpr float sourceBlendRate = 20.0f;  // per second: how fast a nozzle drags the gas to its own velocity

}  // namespace

void GasSolver::reset(const GasConfig& config) {
    config_ = config;
    nx_ = std::max(config.resolution, 1);
    h_ = (config.maxX - config.minX) / static_cast<float>(nx_);
    ny_ = std::max(static_cast<int>(std::lround((config.maxY - config.minY) / h_)), 1);
    const size_t cells = static_cast<size_t>(nx_ + 2) * static_cast<size_t>(ny_ + 2);
    for (std::vector<float>* a : {&u_, &v_, &d_, &t_, &u0_, &v0_, &p_, &div_, &w_, &tmp_}) a->assign(cells, 0.0f);
}

void GasSolver::boundary(std::vector<float>& a, int kind) const {
    const bool mirror = !config_.open;
    for (int i = 1; i <= nx_; ++i) {
        a[at(i, 0)] = kind == verticalKind && mirror ? -a[at(i, 1)] : a[at(i, 1)];
        a[at(i, ny_ + 1)] = kind == verticalKind && mirror ? -a[at(i, ny_)] : a[at(i, ny_)];
    }
    for (int j = 1; j <= ny_; ++j) {
        a[at(0, j)] = kind == horizontalKind && mirror ? -a[at(1, j)] : a[at(1, j)];
        a[at(nx_ + 1, j)] = kind == horizontalKind && mirror ? -a[at(nx_, j)] : a[at(nx_, j)];
    }
    a[at(0, 0)] = 0.5f * (a[at(1, 0)] + a[at(0, 1)]);
    a[at(0, ny_ + 1)] = 0.5f * (a[at(1, ny_ + 1)] + a[at(0, ny_)]);
    a[at(nx_ + 1, 0)] = 0.5f * (a[at(nx_, 0)] + a[at(nx_ + 1, 1)]);
    a[at(nx_ + 1, ny_ + 1)] = 0.5f * (a[at(nx_, ny_ + 1)] + a[at(nx_ + 1, ny_)]);
}

void GasSolver::addSource(const GasSource& s, float dt) {
    const float r = std::max(s.radius, h_);
    const int i0 = std::max(static_cast<int>(std::floor((s.x - r - config_.minX) / h_)), 0);
    const int i1 = std::min(static_cast<int>(std::ceil((s.x + r - config_.minX) / h_)), nx_ - 1);
    const int j0 = std::max(static_cast<int>(std::floor((s.y - r - config_.minY) / h_)), 0);
    const int j1 = std::min(static_cast<int>(std::ceil((s.y + r - config_.minY) / h_)), ny_ - 1);
    const bool pushes = s.vx != 0.0f || s.vy != 0.0f;
    const float blend = std::min(sourceBlendRate * dt, 1.0f);
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            const float dx = cellX(i) - s.x, dy = cellY(j) - s.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (!(d < r)) continue;
            const float w = (1.0f - d / r) * (1.0f - d / r);
            const size_t k = at(i + 1, j + 1);
            d_[k] += s.density * w * dt;
            t_[k] += s.heat * w * dt;
            if (pushes) {
                u_[k] += (s.vx - u_[k]) * w * blend;
                v_[k] += (s.vy - v_[k]) * w * blend;
            }
        }
    }
}

void GasSolver::addBody(float x, float y, float vx, float vy, float density, float push, float dt) {
    const int i = static_cast<int>(std::floor((x - config_.minX) / h_));
    const int j = static_cast<int>(std::floor((y - config_.minY) / h_));
    if (i < 0 || j < 0 || i >= nx_ || j >= ny_) return;
    const size_t k = at(i + 1, j + 1);
    d_[k] += density * dt;
    const float blend = std::min(push * dt, 1.0f);
    u_[k] += (vx - u_[k]) * blend;
    v_[k] += (vy - v_[k]) * blend;
}

void GasSolver::sample(float x, float y, float& vx, float& vy, float& density) const {
    vx = vy = density = 0.0f;
    if (!(x >= config_.minX && x <= config_.minX + static_cast<float>(nx_) * h_ && y >= config_.minY && y <= config_.minY + static_cast<float>(ny_) * h_)) return;
    const float fx = std::clamp((x - config_.minX) / h_ + 0.5f, 0.5f, static_cast<float>(nx_) + 0.5f);
    const float fy = std::clamp((y - config_.minY) / h_ + 0.5f, 0.5f, static_cast<float>(ny_) + 0.5f);
    const int i = static_cast<int>(fx), j = static_cast<int>(fy);
    const float sx = fx - static_cast<float>(i), sy = fy - static_cast<float>(j);
    auto lerp2 = [&](const std::vector<float>& a) {
        return (a[at(i, j)] * (1.0f - sx) + a[at(i + 1, j)] * sx) * (1.0f - sy) + (a[at(i, j + 1)] * (1.0f - sx) + a[at(i + 1, j + 1)] * sx) * sy;
    };
    vx = lerp2(u_);
    vy = lerp2(v_);
    density = lerp2(d_);
}

template <class F>
void GasSolver::sweep(F&& cell) {
    if (!config_.redBlack) {
        for (int j = 1; j <= ny_; ++j)
            for (int i = 1; i <= nx_; ++i) cell(i, j);
        return;
    }
    // Below this the cost of waking the pool is larger than the sweep.
    const bool threaded = pool_ && static_cast<size_t>(nx_) * static_cast<size_t>(ny_) >= 16384;
    for (int colour = 0; colour < 2; ++colour) {
        auto rows = [&](size_t begin, size_t end) {
            for (int j = static_cast<int>(begin) + 1; j <= static_cast<int>(end); ++j)
                for (int i = 1 + (((1 + j) & 1) != colour ? 1 : 0); i <= nx_; i += 2) cell(i, j);
        };
        if (threaded) parallelChunks(pool_, static_cast<size_t>(ny_), rows);
        else rows(0, static_cast<size_t>(ny_));
    }
}

void GasSolver::project() {
    for (int j = 1; j <= ny_; ++j)
        for (int i = 1; i <= nx_; ++i) div_[at(i, j)] = 0.5f * h_ * (u_[at(i + 1, j)] - u_[at(i - 1, j)] + v_[at(i, j + 1)] - v_[at(i, j - 1)]);
    auto pressureBoundary = [&] {
        if (config_.open) {
            for (int i = 0; i <= nx_ + 1; ++i) p_[at(i, 0)] = p_[at(i, ny_ + 1)] = 0.0f;
            for (int j = 0; j <= ny_ + 1; ++j) p_[at(0, j)] = p_[at(nx_ + 1, j)] = 0.0f;
        } else {
            boundary(p_, scalarKind);
        }
    };
    pressureBoundary();
    for (int it = 0; it < config_.iterations; ++it) {
        sweep([&](int i, int j) { p_[at(i, j)] = (p_[at(i - 1, j)] + p_[at(i + 1, j)] + p_[at(i, j - 1)] + p_[at(i, j + 1)] - div_[at(i, j)]) * 0.25f; });
        pressureBoundary();
    }
    for (int j = 1; j <= ny_; ++j) {
        for (int i = 1; i <= nx_; ++i) {
            u_[at(i, j)] -= 0.5f * (p_[at(i + 1, j)] - p_[at(i - 1, j)]) / h_;
            v_[at(i, j)] -= 0.5f * (p_[at(i, j + 1)] - p_[at(i, j - 1)]) / h_;
        }
    }
    boundary(u_, horizontalKind);
    boundary(v_, verticalKind);
}

void GasSolver::diffuse(std::vector<float>& a, const std::vector<float>& a0, float k, int kind) {
    for (int it = 0; it < config_.iterations; ++it) {
        sweep([&](int i, int j) {
            a[at(i, j)] = (a0[at(i, j)] + k * (a[at(i - 1, j)] + a[at(i + 1, j)] + a[at(i, j - 1)] + a[at(i, j + 1)])) / (1.0f + 4.0f * k);
        });
        boundary(a, kind);
    }
}

void GasSolver::advect(std::vector<float>& dst, const std::vector<float>& src, const std::vector<float>& uu, const std::vector<float>& vv, float dt, int kind) {
    const float k = dt / h_;
    for (int j = 1; j <= ny_; ++j) {
        for (int i = 1; i <= nx_; ++i) {
            const float x = std::clamp(static_cast<float>(i) - uu[at(i, j)] * k, 0.5f, static_cast<float>(nx_) + 0.5f);
            const float y = std::clamp(static_cast<float>(j) - vv[at(i, j)] * k, 0.5f, static_cast<float>(ny_) + 0.5f);
            const int i0 = static_cast<int>(x), j0 = static_cast<int>(y);
            const float sx = x - static_cast<float>(i0), sy = y - static_cast<float>(j0);
            dst[at(i, j)] = (src[at(i0, j0)] * (1.0f - sx) + src[at(i0 + 1, j0)] * sx) * (1.0f - sy) +
                            (src[at(i0, j0 + 1)] * (1.0f - sx) + src[at(i0 + 1, j0 + 1)] * sx) * sy;
        }
    }
    boundary(dst, kind);
}

void GasSolver::confineVorticity(float dt) {
    for (int j = 1; j <= ny_; ++j)
        for (int i = 1; i <= nx_; ++i) w_[at(i, j)] = (v_[at(i + 1, j)] - v_[at(i - 1, j)] - (u_[at(i, j + 1)] - u_[at(i, j - 1)])) / (2.0f * h_);
    for (int j = 2; j < ny_; ++j) {
        for (int i = 2; i < nx_; ++i) {
            const float gx = (std::fabs(w_[at(i + 1, j)]) - std::fabs(w_[at(i - 1, j)])) / (2.0f * h_);
            const float gy = (std::fabs(w_[at(i, j + 1)]) - std::fabs(w_[at(i, j - 1)])) / (2.0f * h_);
            const float len = std::sqrt(gx * gx + gy * gy) + 1e-6f;
            const float nx = gx / len, ny = gy / len;
            const float w = w_[at(i, j)];
            u_[at(i, j)] += config_.vorticity * h_ * ny * w * dt;
            v_[at(i, j)] -= config_.vorticity * h_ * nx * w * dt;
        }
    }
}

void GasSolver::step(float dt, ThreadPool* pool) {
    pool_ = pool;
    if (config_.buoyancy != 0.0f || config_.weight != 0.0f) {
        for (int j = 1; j <= ny_; ++j)
            for (int i = 1; i <= nx_; ++i) v_[at(i, j)] += (config_.buoyancy * t_[at(i, j)] - config_.weight * d_[at(i, j)]) * dt;
    }
    if (config_.vorticity != 0.0f) confineVorticity(dt);
    if (config_.viscosity > 0.0f) {
        const float k = config_.viscosity * dt / (h_ * h_);
        u0_ = u_;
        v0_ = v_;
        diffuse(u_, u0_, k, horizontalKind);
        diffuse(v_, v0_, k, verticalKind);
    }
    boundary(u_, horizontalKind);
    boundary(v_, verticalKind);
    project();
    u0_ = u_;
    v0_ = v_;
    advect(u_, u0_, u0_, v0_, dt, horizontalKind);
    advect(v_, v0_, u0_, v0_, dt, verticalKind);
    project();
    tmp_ = d_;
    advect(d_, tmp_, u_, v_, dt, scalarKind);
    tmp_ = t_;
    advect(t_, tmp_, u_, v_, dt, scalarKind);
    const float keepDensity = std::exp(-std::max(config_.dissipation, 0.0f) * dt), keepHeat = std::exp(-std::max(config_.cooling, 0.0f) * dt);
    for (size_t k = 0; k < d_.size(); ++k) {
        d_[k] *= keepDensity;
        t_[k] *= keepHeat;
    }
}

std::string gasParamName(const char* prefix, const char* base) {
    std::string name = base;
    if (prefix && *prefix) {
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        name = prefix + name;
    }
    return name;
}

GasConfig readGasConfig(const Node& node, const char* prefix, const char* who) {
    GasConfig c;
    c.minX = node.getFloat("boundsMinX");
    c.maxX = node.getFloat("boundsMaxX");
    c.minY = node.getFloat("boundsMinY");
    c.maxY = node.getFloat("boundsMaxY");
    if (!(c.maxX > c.minX) || !(c.maxY > c.minY)) throw std::runtime_error(std::string(who) + ": the bounds must have a positive width and height");
    const float resolution = node.getFloat(gasParamName(prefix, "resolution"));
    if (!(resolution >= 8.0f && resolution <= 512.0f)) throw std::runtime_error(std::string(who) + ": " + gasParamName(prefix, "resolution") + " must be between 8 and 512");
    c.resolution = static_cast<int>(resolution);
    if (static_cast<double>(c.maxY - c.minY) / (static_cast<double>(c.maxX - c.minX) / c.resolution) > 2048.0)
        throw std::runtime_error(std::string(who) + ": the bounds are too tall for the resolution");
    c.viscosity = std::max(node.getFloat(gasParamName(prefix, "viscosity")), 0.0f);
    c.dissipation = node.getFloat(gasParamName(prefix, "dissipation"));
    c.cooling = node.getFloat(gasParamName(prefix, "cooling"));
    c.buoyancy = node.getFloat(gasParamName(prefix, "buoyancy"));
    c.weight = node.getFloat(gasParamName(prefix, "weight"));
    c.vorticity = std::max(node.getFloat(gasParamName(prefix, "vorticity")), 0.0f);
    c.iterations = std::clamp(static_cast<int>(node.getFloat(gasParamName(prefix, "iterations"))), 1, 200);
    c.open = node.getFloat(gasParamName(prefix, "open")) != 0.0f;
    c.redBlack = node.getFloat(gasParamName(prefix, "redBlack")) != 0.0f;
    return c;
}

bool readGasNozzle(const Node& node, const char* prefix, GasSource& out) {
    out.x = node.getFloat(gasParamName(prefix, "nozzleX"));
    out.y = node.getFloat(gasParamName(prefix, "nozzleY"));
    out.radius = std::max(node.getFloat(gasParamName(prefix, "nozzleRadius")), 1e-3f);
    out.density = node.getFloat(gasParamName(prefix, "nozzleDensity"));
    out.heat = node.getFloat(gasParamName(prefix, "nozzleHeat"));
    const float speed = node.getFloat(gasParamName(prefix, "nozzleSpeed"));
    const float angle = node.getFloat(gasParamName(prefix, "nozzleAngle")) * (3.14159265f / 180.0f);
    out.vx = speed * std::cos(angle);
    out.vy = speed * std::sin(angle);
    return out.density != 0.0f || out.heat != 0.0f || speed != 0.0f;
}

}  // namespace jitnodes
