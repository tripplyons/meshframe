#pragma once

#include <algorithm>
#include <cmath>

namespace mf::ui {

struct Vec {
    float x = 0, y = 0;
};

// Axis-aligned rectangle in physical pixels.
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    float r() const { return x + w; }
    float b() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool contains(Vec p) const { return contains(p.x, p.y); }
    Rect inset(float d) const { return {x + d, y + d, std::max(0.0f, w - 2 * d), std::max(0.0f, h - 2 * d)}; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, std::max(0.0f, w - 2 * dx), std::max(0.0f, h - 2 * dy)}; }
    Rect intersect(const Rect& o) const {
        float x0 = std::max(x, o.x), y0 = std::max(y, o.y);
        float x1 = std::min(r(), o.r()), y1 = std::min(b(), o.b());
        return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    bool operator==(const Rect&) const = default;
};

// Slice helpers: take a strip off one side and shrink the source.
inline Rect cutTop(Rect& r, float h) {
    h = std::min(h, r.h);
    Rect out{r.x, r.y, r.w, h};
    r.y += h;
    r.h -= h;
    return out;
}
inline Rect cutBottom(Rect& r, float h) {
    h = std::min(h, r.h);
    r.h -= h;
    return {r.x, r.b(), r.w, h};
}
inline Rect cutLeft(Rect& r, float w) {
    w = std::min(w, r.w);
    Rect out{r.x, r.y, w, r.h};
    r.x += w;
    r.w -= w;
    return out;
}
inline Rect cutRight(Rect& r, float w) {
    w = std::min(w, r.w);
    r.w -= w;
    return {r.r(), r.y, w, r.h};
}

}  // namespace mf::ui
