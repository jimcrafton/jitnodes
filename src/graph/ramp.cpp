#include "jitnodes/graph/ramp.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace jitnodes {

Ramp::Ramp() : Ramp(std::vector<RampPoint>{{0.0f, 0.0f, RampInterp::linear}, {1.0f, 1.0f, RampInterp::linear}}) {}

Ramp::Ramp(std::vector<RampPoint> points) : points_(std::move(points)) {
    if (points_.empty()) {
        throw std::runtime_error("ramp: the ramp has no points");
    }
    for (const RampPoint& p : points_) {
        if (!std::isfinite(p.pos) || !std::isfinite(p.value)) {
            throw std::runtime_error("ramp: positions and values must be finite numbers");
        }
    }
    std::stable_sort(points_.begin(), points_.end(), [](const RampPoint& a, const RampPoint& b) { return a.pos < b.pos; });
    prepare();
}

void Ramp::prepare() {
    const size_t n = points_.size();
    catmullTangents_.assign(n, 0.0f);
    monotoneTangents_.assign(n, 0.0f);
    if (n < 2) {
        return;
    }
    auto slope = [&](size_t a, size_t b) {
        const float width = points_[b].pos - points_[a].pos;
        return width > 0.0f ? (points_[b].value - points_[a].value) / width : 0.0f;
    };
    std::vector<float> secant(n - 1);
    for (size_t i = 0; i + 1 < n; ++i) {
        secant[i] = slope(i, i + 1);
    }
    for (size_t i = 0; i < n; ++i) {
        const size_t before = i == 0 ? 0 : i - 1;
        const size_t after = i + 1 == n ? n - 1 : i + 1;
        catmullTangents_[i] = slope(before, after);
    }

    monotoneTangents_[0] = secant[0];
    monotoneTangents_[n - 1] = secant[n - 2];
    for (size_t i = 1; i + 1 < n; ++i) {
        monotoneTangents_[i] = secant[i - 1] * secant[i] <= 0.0f ? 0.0f : 0.5f * (secant[i - 1] + secant[i]);
    }
    for (size_t i = 0; i + 1 < n; ++i) {
        if (secant[i] == 0.0f) {
            monotoneTangents_[i] = monotoneTangents_[i + 1] = 0.0f;
            continue;
        }
        const float alpha = monotoneTangents_[i] / secant[i];
        const float beta = monotoneTangents_[i + 1] / secant[i];
        const float radius = alpha * alpha + beta * beta;
        if (radius > 9.0f) {
            const float scale = 3.0f / std::sqrt(radius);
            monotoneTangents_[i] = scale * alpha * secant[i];
            monotoneTangents_[i + 1] = scale * beta * secant[i];
        }
    }
}

float Ramp::eval(float x) const {
    const size_t n = points_.size();
    if (!(x > points_.front().pos)) {
        return points_.front().value;
    }
    if (x >= points_.back().pos) {
        return points_.back().value;
    }
    size_t i = static_cast<size_t>(std::upper_bound(points_.begin(), points_.end(), x, [](float v, const RampPoint& p) { return v < p.pos; }) - points_.begin());
    i = i == 0 ? 0 : i - 1;
    if (i + 1 >= n) {
        return points_.back().value;
    }
    const RampPoint& a = points_[i];
    const RampPoint& b = points_[i + 1];
    const float width = b.pos - a.pos;
    const float t = width > 0.0f ? (x - a.pos) / width : 1.0f;
    switch (a.interp) {
        case RampInterp::constant:
            return a.value;
        case RampInterp::linear:
            return a.value + (b.value - a.value) * t;
        case RampInterp::smooth:
            return a.value + (b.value - a.value) * (t * t * (3.0f - 2.0f * t));
        case RampInterp::catmullRom:
        case RampInterp::monotone: {
            const std::vector<float>& m = a.interp == RampInterp::catmullRom ? catmullTangents_ : monotoneTangents_;
            const float t2 = t * t, t3 = t2 * t;
            return (2.0f * t3 - 3.0f * t2 + 1.0f) * a.value + (t3 - 2.0f * t2 + t) * width * m[i] + (-2.0f * t3 + 3.0f * t2) * b.value +
                   (t3 - t2) * width * m[i + 1];
        }
    }
    return a.value;
}

std::vector<float> Ramp::bake(size_t count) const {
    count = std::max<size_t>(count, 2);
    std::vector<float> table(count);
    const float first = points_.front().pos, span = points_.back().pos - first;
    for (size_t i = 0; i < count; ++i) {
        table[i] = eval(first + span * static_cast<float>(i) / static_cast<float>(count - 1));
    }
    return table;
}

const char* Ramp::interpName(RampInterp interp) {
    switch (interp) {
        case RampInterp::constant: return "constant";
        case RampInterp::linear: return "linear";
        case RampInterp::smooth: return "smooth";
        case RampInterp::catmullRom: return "catmullRom";
        case RampInterp::monotone: return "monotone";
    }
    return "linear";
}

RampInterp Ramp::interpFromName(const std::string& name) {
    std::string key;
    for (char c : name) {
        if (c != '-' && c != '_') {
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (key == "constant") return RampInterp::constant;
    if (key == "linear") return RampInterp::linear;
    if (key == "smooth") return RampInterp::smooth;
    if (key == "catmullrom") return RampInterp::catmullRom;
    if (key == "monotone") return RampInterp::monotone;
    throw std::runtime_error("ramp: interpolation must be constant, linear, smooth, catmullRom or monotone, got '" + name + "'");
}

Ramp Ramp::parse(const std::string& text) {
    std::vector<RampPoint> points;
    std::string item;
    int number = 0;
    auto flush = [&]() {
        std::istringstream in(item);
        item.clear();
        std::string first;
        if (!(in >> first)) {
            return;
        }
        ++number;
        RampPoint p;
        std::string value;
        if (!(in >> value)) {
            throw std::runtime_error("ramp: point " + std::to_string(number) + " ('" + first + "') needs a position and a value");
        }
        try {
            size_t used = 0;
            p.pos = std::stof(first, &used);
            if (used != first.size()) throw std::invalid_argument(first);
            p.value = std::stof(value, &used);
            if (used != value.size()) throw std::invalid_argument(value);
        } catch (const std::exception&) {
            throw std::runtime_error("ramp: point " + std::to_string(number) + " ('" + first + " " + value + "') is not two numbers");
        }
        std::string interp;
        if (in >> interp) {
            p.interp = interpFromName(interp);
            std::string extra;
            if (in >> extra) {
                throw std::runtime_error("ramp: point " + std::to_string(number) + " has extra text '" + extra + "'");
            }
        }
        points.push_back(p);
    };
    for (char c : text) {
        if (c == ',' || c == ';' || c == '\n') {
            flush();
        } else {
            item.push_back(c);
        }
    }
    flush();
    return Ramp(std::move(points));
}

std::string Ramp::check(const std::string& text) {
    try {
        parse(text);
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

std::string Ramp::parentReference(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::string trimmed = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    static const std::string prefix = "parent.";
    if (trimmed.compare(0, prefix.size(), prefix) != 0) {
        return {};
    }
    const std::string name = trimmed.substr(prefix.size());
    for (const char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            return {};
        }
    }
    return name;
}

std::string Ramp::toText() const {
    std::string text;
    for (const RampPoint& p : points_) {
        char line[96];
        std::snprintf(line, sizeof line, "%g %g %s", static_cast<double>(p.pos), static_cast<double>(p.value), interpName(p.interp));
        if (!text.empty()) {
            text += ", ";
        }
        text += line;
    }
    return text;
}

}  // namespace jitnodes
