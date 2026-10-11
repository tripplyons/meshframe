#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mf {

enum class Interp { Hold, Linear, Bezier };

const char* interpName(Interp i);
std::optional<Interp> parseInterp(std::string_view s);

// Named cubic-bezier easing presets, as in CSS.
struct EasePreset {
    const char* name;
    float x1, y1, x2, y2;
};
const std::vector<EasePreset>& easePresets();
const EasePreset* findEase(std::string_view name);

// The segment from a keyframe to the next one uses that keyframe's interp and,
// for Bezier, its control points (normalized to the segment, like CSS).
struct Keyframe {
    double t = 0;
    double v = 0;
    Interp interp = Interp::Linear;
    float x1 = 0.42f, y1 = 0.0f, x2 = 0.58f, y2 = 1.0f;

    bool operator==(const Keyframe&) const = default;
};

struct Curve {
    std::vector<Keyframe> keys;  // sorted by t

    bool empty() const { return keys.empty(); }
    double sample(double t) const;
    // Inserts or replaces the key at time t (within epsilon). Returns its index.
    size_t setKey(double t, double v, Interp interp = Interp::Linear);
    bool removeKeyAt(double t, double epsilon = 1e-6);
    void sort();

    // 0..1 ramp with the given easing, used by transitions.
    static Curve ramp(std::string_view ease = "ease-in-out");

    bool operator==(const Curve&) const = default;
};

// Solves a CSS-style cubic bezier: maps x in [0,1] to y.
double cubicBezier(double x, float x1, float y1, float x2, float y2);

}  // namespace mf
