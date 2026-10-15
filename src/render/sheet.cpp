#include "render/sheet.hpp"

#include "render/font.hpp"
#include "render/raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mf {

int squareColumns(int count, int w, int h) {
    if (count <= 0 || w <= 0 || h <= 0) return 1;
    int best = 1;
    double bestScore = 1e300;
    for (int cols = 1; cols <= count; ++cols) {
        int rows = (count + cols - 1) / cols;
        double score = std::abs(std::log(double(cols) * w / (double(rows) * h)));
        if (score < bestScore - 1e-9) best = cols, bestScore = score;
    }
    return best;
}

Image composeSheet(std::span<const SheetCell> cells, int columns, SheetLayout* layout) {
    SheetLayout l;
    if (cells.empty()) return {};
    const Image& first = *cells.front().frame;
    float px = std::clamp(first.w / 20.0f, 11.0f, 18.0f);
    int strip = int(std::ceil(px * 1.6f));
    l.columns = std::clamp(columns, 1, int(cells.size()));
    l.rows = (int(cells.size()) + l.columns - 1) / l.columns;
    l.cellW = first.w;
    l.cellH = first.h + strip;
    l.gap = std::max(2, first.w / 80);
    Image out(l.gap + l.columns * (l.cellW + l.gap), l.gap + l.rows * (l.cellH + l.gap));
    auto fill = [&](int x0, int y0, int w, int h, uint8_t v) {
        for (int y = y0; y < y0 + h; ++y) {
            uint8_t* row = out.at(x0, y);
            for (int i = 0; i < w; ++i, row += 4) row[0] = row[1] = row[2] = v, row[3] = 255;
        }
    };
    fill(0, 0, out.w, out.h, 26);
    std::shared_ptr<Font> font = Font::sans();
    for (size_t i = 0; i < cells.size(); ++i) {
        int x = l.x(int(i)), y = l.y(int(i));
        // Frames are premultiplied; draw them over black.
        const Image& f = *cells[i].frame;
        for (int r = 0; r < std::min(f.h, first.h); ++r) {
            const uint8_t* src = f.row(r);
            uint8_t* dst = out.at(x, y + r);
            for (int c = 0; c < std::min(f.w, first.w); ++c, src += 4, dst += 4) {
                std::memcpy(dst, src, 3);
                dst[3] = 255;
            }
        }
        fill(x, y + first.h, l.cellW, strip, 40);
        if (font) drawText(out, *font, cells[i].label, float(x) + px * 0.5f, float(y + first.h) + (strip - px * 1.2f) / 2, px, Color::gray(0.9f));
    }
    if (layout) *layout = l;
    return out;
}

}  // namespace mf
