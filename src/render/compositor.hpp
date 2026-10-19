#pragma once

#include "core/value.hpp"
#include "render/image.hpp"

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
// Set sourceOpaque only when every source alpha byte is known to be 255.
void drawLayer(Image& dst, const Image& src, const Placement& at, float opacity, Blend blend, bool sourceOpaque = false);

// Composites a w x h layer that is a single repeated premultiplied colour,
// which drawLayer would otherwise have to be handed an image of. Only the
// identity placement is handled, where every destination pixel maps to one
// source texel; anything else returns false without drawing, so the caller can
// build the image and let drawLayer sample it as before.
bool drawSolidLayer(Image& dst, const Placement& at, int w, int h, Color c, float opacity, Blend blend);

void fill(Image& img, Color c);

// The part of a layer image that holds the clip, inside any margin added
// for effects. Empty means the whole image.
struct PixelBox {
    int x = 0, y = 0, w = 0, h = 0;
};

// Applies a video effect in place. `unit` converts project pixels to image
// pixels. Crop and vignette measure from `content`. Unknown types are ignored.
void applyEffect(Image& img, std::string_view type, const ParamFn& param, double unit, PixelBox content = {});

// Pixels an effect draws past the clip's edges (a drop shadow's offset and
// blur), so the layer can be padded before effects run.
int effectMargin(std::string_view type, const ParamFn& param, double unit);

// Mixes two full frames by a transition at mix m (0 = all A, 1 = all B).
void mixTransition(Image& out, const Image& a, const Image& b, std::string_view type, double m, const ParamFn& param);

}  // namespace mf
