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

// Text split into lines, each centered in a block, as text clips draw it.
struct TextBlock {
    std::vector<std::string> lines;
    float lineHeight = 0;
    int w = 0, h = 0;  // block size in pixels, with a 1 px margin
};
TextBlock layoutTextBlock(Font& font, const std::string& text, float px);

// Draws a generator's commands, scaling project coordinates by `scale`.
void drawList(Image& dst, const DrawList& list, float scale);

}  // namespace mf
