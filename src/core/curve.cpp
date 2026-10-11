#include "core/curve.hpp"

#include <algorithm>
#include <cmath>

namespace mf {

const char* interpName(Interp i) {
    switch (i) {
        case Interp::Hold: return "hold";
        case Interp::Linear: return "linear";
        case Interp::Bezier: return "bezier";
    }
    return "linear";
}

std::optional<Interp> parseInterp(std::string_view s) {
    if (s == "hold") return Interp::Hold;
    if (s == "linear") return Interp::Linear;
    if (s == "bezier") return Interp::Bezier;
    return std::nullopt;
}

const std::vector<EasePreset>& easePresets() {
    static const std::vector<EasePreset> presets = {
        {"linear", 0.0f, 0.0f, 1.0f, 1.0f},
        {"ease", 0.25f, 0.1f, 0.25f, 1.0f},
        {"ease-in", 0.42f, 0.0f, 1.0f, 1.0f},
        {"ease-out", 0.0f, 0.0f, 0.58f, 1.0f},
        {"ease-in-out", 0.42f, 0.0f, 0.58f, 1.0f},
        {"ease-in-cubic", 0.55f, 0.055f, 0.675f, 0.19f},
        {"ease-out-cubic", 0.215f, 0.61f, 0.355f, 1.0f},
        {"ease-in-out-cubic", 0.645f, 0.045f, 0.355f, 1.0f},
        {"ease-in-back", 0.6f, -0.28f, 0.735f, 0.045f},
        {"ease-out-back", 0.175f, 0.885f, 0.32f, 1.275f},
        {"ease-in-out-back", 0.68f, -0.55f, 0.265f, 1.55f},
    };
    return presets;
}

const EasePreset* findEase(std::string_view name) {
    for (const auto& p : easePresets())
        if (name == p.name) return &p;
    return nullptr;
}

double cubicBezier(double x, float x1, float y1, float x2, float y2) {
    x = std::clamp(x, 0.0, 1.0);
    auto bez = [](double t, double a, double b) {
        double u = 1 - t;
        return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
    };
    auto dbez = [](double t, double a, double b) {
        double u = 1 - t;
        return 3 * u * u * a + 6 * u * t * (b - a) + 3 * t * t * (1 - b);
    };
    // Newton first, bisection as a fallback for flat slopes.
    double t = x;
    for (int i = 0; i < 8; ++i) {
        double err = bez(t, x1, x2) - x;
        if (std::abs(err) < 1e-7) return bez(t, y1, y2);
        double d = dbez(t, x1, x2);
        if (std::abs(d) < 1e-6) break;
        t = std::clamp(t - err / d, 0.0, 1.0);
    }
    double lo = 0, hi = 1;
    t = x;
    for (int i = 0; i < 40; ++i) {
        double v = bez(t, x1, x2);
        if (std::abs(v - x) < 1e-7) break;
        if (v < x) lo = t; else hi = t;
        t = (lo + hi) / 2;
    }
    return bez(t, y1, y2);
}

double Curve::sample(double t) const {
    if (keys.empty()) return 0.0;
    if (t <= keys.front().t) return keys.front().v;
    if (t >= keys.back().t) return keys.back().v;
    auto it = std::upper_bound(keys.begin(), keys.end(), t, [](double v, const Keyframe& k) { return v < k.t; });
    const Keyframe& b = *it;
    const Keyframe& a = *(it - 1);
    double span = b.t - a.t;
    if (span <= 0) return b.v;
    double x = (t - a.t) / span;
    switch (a.interp) {
        case Interp::Hold: return a.v;
        case Interp::Linear: return a.v + (b.v - a.v) * x;
        case Interp::Bezier: return a.v + (b.v - a.v) * cubicBezier(x, a.x1, a.y1, a.x2, a.y2);
    }
    return a.v;
}

void Curve::sort() {
    std::stable_sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
}

size_t Curve::setKey(double t, double v, Interp interp) {
    for (size_t i = 0; i < keys.size(); ++i) {
        if (std::abs(keys[i].t - t) < 1e-6) {
            keys[i].v = v;
            return i;
        }
    }
    Keyframe k;
    k.t = t;
    k.v = v;
    k.interp = interp;
    auto it = std::upper_bound(keys.begin(), keys.end(), t, [](double v, const Keyframe& k) { return v < k.t; });
    return size_t(keys.insert(it, k) - keys.begin());
}

bool Curve::removeKeyAt(double t, double epsilon) {
    auto it = std::find_if(keys.begin(), keys.end(), [&](const Keyframe& k) { return std::abs(k.t - t) < epsilon; });
    if (it == keys.end()) return false;
    keys.erase(it);
    return true;
}

Curve Curve::ramp(std::string_view ease) {
    Curve c;
    Keyframe a;
    a.t = 0;
    a.v = 0;
    const EasePreset* p = findEase(ease);
    if (!p || std::string_view(p->name) == "linear") {
        a.interp = Interp::Linear;
    } else {
        a.interp = Interp::Bezier;
        a.x1 = p->x1;
        a.y1 = p->y1;
        a.x2 = p->x2;
        a.y2 = p->y2;
    }
    Keyframe b;
    b.t = 1;
    b.v = 1;
    c.keys = {a, b};
    return c;
}

}  // namespace mf
