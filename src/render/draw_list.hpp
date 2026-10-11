#pragma once

#include "core/value.hpp"

#include <string>
#include <vector>

namespace mf {

// Vector drawing produced by script generators, in project pixels with y down.
// Rect: a,b = top-left, c,d = size. Circle: a,b = center, c = radius.
// Line: a,b to c,d with width e. Text: top-left a,b, size c. Poly: flat points.
struct DrawCmd {
    enum class Type { Clear, Rect, Circle, Line, Text, Poly } type;
    float a = 0, b = 0, c = 0, d = 0, e = 0;
    Color color;
    std::string text;
    std::vector<float> points;
};

struct DrawList {
    std::vector<DrawCmd> cmds;
};

}  // namespace mf
