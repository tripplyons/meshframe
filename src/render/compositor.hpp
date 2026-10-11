#pragma once

#include "core/value.hpp"
#include "render/image.hpp"

#include <functional>
#include <string_view>

namespace mf {

enum class Blend { Normal, Add, Multiply, Screen };
Blend parseBlend(std::string_view s);

// Places a layer: its center goes to (cx, cy), scaled by sx, sy, then rotated
// by `rotation` radians clockwise (y down).
struct Placement {
    double cx = 0, cy = 0;
    double sx = 1, sy = 1;
    double rotation = 0;
};

// Composites src onto dst with bilinear sampling, opacity, and a blend mode.
void drawLayer(Image& dst, const Image& src, const Placement& at, float opacity, Blend blend);

void fill(Image& img, Color c);

// Looks up an effect or transition parameter; returns the spec default when unset.
using ParamFn = std::function<Value(std::string_view)>;

// Applies a video effect in place. `unit` converts project pixels to image pixels.
// Unknown types are ignored.
void applyEffect(Image& img, std::string_view type, const ParamFn& param, double unit);

// Mixes two full frames by a transition at mix m (0 = all A, 1 = all B).
void mixTransition(Image& out, const Image& a, const Image& b, std::string_view type, double m, const ParamFn& param);

}  // namespace mf
