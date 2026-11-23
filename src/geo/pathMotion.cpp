#include "jitnodes/geo/pathMotion.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace jitnodes {

void PathMotion::compute(Geometry& geo, const PathMotionParams& params) {
    if (!(params.smoothing > 0.0f && params.smoothing <= 1.0f)) {
        throw std::runtime_error("pathMotion: smoothing must be greater than 0 and at most 1");
    }
    const AlignedArray<float>* timeColumn = geo.findPointAttrib(params.timeAttr);
    if (timeColumn == nullptr) {
        throw std::runtime_error("pathMotion: the points have no '" + params.timeAttr + "' attribute (sample time in seconds)");
    }
    const size_t n = geo.pointCount();
    const std::vector<float> time(timeColumn->data(), timeColumn->data() + n);
    const std::vector<float> px(geo.x(), geo.x() + n);
    const std::vector<float> py(geo.y(), geo.y() + n);

    std::vector<float> s(n, 0.0f), dt(n, 0.0f), vx(n, 0.0f), vy(n, 0.0f), speed(n, 0.0f), ax(n, 0.0f), ay(n, 0.0f), accel(n, 0.0f), heading(n, 0.0f);
    const float alpha = params.smoothing;

    for (const Primitive& prim : geo.primitives()) {
        if (prim.count == 0) {
            continue;
        }
        const size_t first = prim.start;
        const size_t last = first + prim.count;
        for (size_t i = first + 1; i < last; ++i) {
            const float dx = px[i] - px[i - 1];
            const float dy = py[i] - py[i - 1];
            const float step = time[i] - time[i - 1];
            s[i] = s[i - 1] + std::hypot(dx, dy);
            dt[i] = step > 0.0f ? step : 0.0f;
            if (step > 0.0f) {
                const float rawX = dx / step;
                const float rawY = dy / step;
                if (i == first + 1) {
                    vx[i] = rawX;
                    vy[i] = rawY;
                } else {
                    vx[i] = alpha * rawX + (1.0f - alpha) * vx[i - 1];
                    vy[i] = alpha * rawY + (1.0f - alpha) * vy[i - 1];
                }
                speed[i] = std::hypot(vx[i], vy[i]);
                if (i > first + 1) {
                    ax[i] = (vx[i] - vx[i - 1]) / step;
                    ay[i] = (vy[i] - vy[i - 1]) / step;
                    accel[i] = (speed[i] - speed[i - 1]) / step;
                }
                heading[i] = speed[i] > 1e-6f ? std::atan2(vy[i], vx[i]) : heading[i - 1];
            } else {
                vx[i] = vx[i - 1];
                vy[i] = vy[i - 1];
                speed[i] = speed[i - 1];
                heading[i] = heading[i - 1];
            }
        }
        if (prim.count > 1) {
            vx[first] = vx[first + 1];
            vy[first] = vy[first + 1];
            speed[first] = speed[first + 1];
            heading[first] = heading[first + 1];
        }
    }

    const struct {
        const char* name;
        const std::vector<float>* values;
    } outputs[] = {{"s", &s}, {"dt", &dt}, {"vx", &vx}, {"vy", &vy}, {"speed", &speed}, {"ax", &ax}, {"ay", &ay}, {"accel", &accel}, {"heading", &heading}};
    for (const auto& output : outputs) {
        float* out = geo.attrib(AttribClass::point, output.name).floats().data();
        std::copy(output.values->begin(), output.values->end(), out);
    }
}

}  // namespace jitnodes
