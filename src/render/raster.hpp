#pragma once

#include "core/value.hpp"
#include "render/draw_list.hpp"
#include "render/image.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mf {

class Font;

struct Vec2 {
    float x = 0, y = 0;
};

// Fills a polygon (nonzero-style winding, anti-aliased) with a straight-alpha color.
void fillPolygon(Image& dst, std::span<const Vec2> pts, Color color);

// Draws one line of text with its top-left at (x, y).
void drawText(Image& dst, Font& font, std::string_view text, float x, float y, float px, Color color);

// How text clips lay out a block of text. Sizes are in pixels.
struct TextLayout {
    float px = 96;
    float wrap = 0;         // break lines at spaces to fit this width; 0 for none
    float lineSpacing = 1;  // multiple of the font's line height
    float outline = 0;      // outline width, which widens the margin
};

// Text split into lines, as text clips draw it.
struct TextBlock {
    std::vector<std::string> lines;
    std::vector<float> widths;  // of each line
    float lineHeight = 0;       // distance between baselines
    float margin = 1;           // around the text: 1 px plus the outline
    int w = 0, h = 0;           // block size
};
TextBlock layoutTextBlock(Font& font, const std::string& text, const TextLayout& layout);

// Fills `color` around the shapes in `src` (by its alpha) out to `width`
// pixels, and under them, into an image of the same size.
Image outlineOf(const Image& src, float width, Color color);

// Draws a generator's commands, scaling project coordinates by `scale`.
void drawList(Image& dst, const DrawList& list, float scale);

}  // namespace mf
