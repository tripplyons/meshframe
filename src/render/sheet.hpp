#pragma once

#include "render/image.hpp"

#include <span>
#include <string>

namespace mf {

struct SheetCell {
    ImagePtr frame;  // every frame has the same size
    std::string label;
};

struct SheetLayout {
    int columns = 0, rows = 0;
    int cellW = 0, cellH = 0;  // frame size plus the label strip
    int gap = 0;
    // Top-left corner of cell i.
    int x(int i) const { return gap + (i % columns) * (cellW + gap); }
    int y(int i) const { return gap + (i / columns) * (cellH + gap); }
};

// Columns that keep a grid of `count` frames of size w x h near square.
int squareColumns(int count, int w, int h);

// Lays frames out in a grid with a label strip under each one.
Image composeSheet(std::span<const SheetCell> cells, int columns, SheetLayout* layout = nullptr);

}  // namespace mf
