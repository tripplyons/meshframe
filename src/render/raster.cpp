#include "render/raster.hpp"

#include "render/font.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>
#include <vector>

namespace mf {

namespace {

// Signed-area coverage accumulation over a clipped bounding box (the font-rs method).
class Coverage {
public:
    Coverage(int x0, int y0, int w, int h) : x0_(x0), y0_(y0), w_(w), h_(h), acc_(size_t(w + 2) * h, 0.0f) {}

    void line(Vec2 p0, Vec2 p1) {
        p0 = local(p0);
        p1 = local(p1);
        if (std::abs(p0.y - p1.y) <= 1e-6f) return;
        float dir = 1;
        if (p0.y > p1.y) {
            std::swap(p0, p1);
            dir = -1;
        }
        float dxdy = (p1.x - p0.x) / (p1.y - p0.y);
        float x = p0.x;
        int ystart = std::max(0, int(p0.y));
        if (p0.y < 0) x -= p0.y * dxdy;
        int yend = std::min(h_, int(std::ceil(p1.y)));
        for (int y = ystart; y < yend; ++y) {
            float* row = acc_.data() + size_t(y) * (w_ + 2);
            float dy = std::min(float(y + 1), p1.y) - std::max(float(y), p0.y);
            float xnext = x + dxdy * dy;
            float d = dy * dir;
            float xa = std::clamp(std::min(x, xnext), 0.0f, float(w_));
            float xb = std::clamp(std::max(x, xnext), 0.0f, float(w_));
            float xaf = std::floor(xa);
            int xai = int(xaf);
            float xbc = std::ceil(xb);
            int xbi = int(xbc);
            if (xbi <= xai + 1) {
                float xmf = 0.5f * (xa + xb) - xaf;
                row[xai] += d - d * xmf;
                row[xai + 1] += d * xmf;
            } else {
                float s = 1.0f / (xb - xa);
                float x0f = xa - xaf;
                float a0 = 0.5f * s * (1 - x0f) * (1 - x0f);
                float x1f = xb - xbc + 1;
                float am = 0.5f * s * x1f * x1f;
                row[xai] += d * a0;
                if (xbi == xai + 2) {
                    row[xai + 1] += d * (1 - a0 - am);
                } else {
                    float a1 = s * (1.5f - x0f);
                    row[xai + 1] += d * (a1 - a0);
                    for (int xi = xai + 2; xi < xbi - 1; ++xi) row[xi] += d * s;
                    float a2 = a1 + float(xbi - xai - 3) * s;
                    row[xbi - 1] += d * (1 - a2 - am);
                }
                row[xbi] += d * am;
            }
            x = xnext;
        }
    }

    void fill(Image& dst, Color c) const {
        float pr = c.r * c.a, pg = c.g * c.a, pb = c.b * c.a;
        for (int y = 0; y < h_; ++y) {
            const float* row = acc_.data() + size_t(y) * (w_ + 2);
            uint8_t* out = dst.at(x0_, y0_ + y);
            float acc = 0;
            for (int x = 0; x < w_; ++x, out += 4) {
                acc += row[x];
                float cov = std::min(1.0f, std::abs(acc));
                if (cov <= 0.002f) continue;
                float sa = c.a * cov, inv = 1 - sa;
                out[0] = uint8_t(std::min(255.0f, pr * cov * 255 + out[0] * inv + 0.5f));
                out[1] = uint8_t(std::min(255.0f, pg * cov * 255 + out[1] * inv + 0.5f));
                out[2] = uint8_t(std::min(255.0f, pb * cov * 255 + out[2] * inv + 0.5f));
                out[3] = uint8_t(std::min(255.0f, sa * 255 + out[3] * inv + 0.5f));
            }
        }
    }

private:
    Vec2 local(Vec2 p) const { return {p.x - x0_, p.y - y0_}; }
    int x0_, y0_, w_, h_;
    std::vector<float> acc_;
};

void fillRect(Image& dst, float x, float y, float w, float h, Color c) {
    Vec2 pts[4] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    fillPolygon(dst, pts, c);
}

}  // namespace

void fillPolygon(Image& dst, std::span<const Vec2> pts, Color color) {
    if (pts.size() < 3 || dst.empty() || color.a <= 0) return;
    float minx = pts[0].x, maxx = minx, miny = pts[0].y, maxy = miny;
    for (auto p : pts) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return;
        minx = std::min(minx, p.x);
        maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y);
        maxy = std::max(maxy, p.y);
    }
    int x0 = std::max(0, int(std::floor(minx))), y0 = std::max(0, int(std::floor(miny)));
    int x1 = std::min(dst.w, int(std::ceil(maxx)) + 1), y1 = std::min(dst.h, int(std::ceil(maxy)) + 1);
    if (x0 >= x1 || y0 >= y1) return;
    // Edges left of the box still contribute: clamp keeps their area at column 0.
    Coverage cov(x0, y0, x1 - x0, y1 - y0);
    for (size_t i = 0; i < pts.size(); ++i) cov.line(pts[i], pts[(i + 1) % pts.size()]);
    cov.fill(dst, color);
}

TextBlock layoutTextBlock(Font& font, const std::string& text, float px) {
    TextBlock b;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) b.lines.push_back(line);
    if (b.lines.empty()) return b;
    b.lineHeight = font.metrics(px).lineHeight;
    float width = 0;
    for (const auto& l : b.lines) width = std::max(width, font.measure(l, px));
    b.w = std::max(2, int(std::ceil(width)) + 2);
    b.h = std::max(2, int(std::ceil(b.lineHeight * b.lines.size())) + 2);
    return b;
}

void drawText(Image& dst, Font& font, std::string_view text, float x, float y, float px, Color c) {
    float baseline = y + font.metrics(px).ascent;
    float pen = x;
    float pr = c.r * c.a, pg = c.g * c.a, pb = c.b * c.a;
    for (size_t i = 0; i < text.size();) {
        const Glyph& g = font.glyph(nextCodepoint(text, i), px);
        int gx = int(std::lround(pen)) + g.left, gy = int(std::lround(baseline)) - g.top;
        for (int row = 0; row < g.h; ++row) {
            int dy = gy + row;
            if (dy < 0 || dy >= dst.h) continue;
            for (int col = 0; col < g.w; ++col) {
                int dx = gx + col;
                if (dx < 0 || dx >= dst.w) continue;
                float cov = g.alpha[size_t(row) * g.w + col] / 255.0f;
                if (cov <= 0) continue;
                uint8_t* o = dst.at(dx, dy);
                float sa = c.a * cov, inv = 1 - sa;
                o[0] = uint8_t(std::min(255.0f, pr * cov * 255 + o[0] * inv + 0.5f));
                o[1] = uint8_t(std::min(255.0f, pg * cov * 255 + o[1] * inv + 0.5f));
                o[2] = uint8_t(std::min(255.0f, pb * cov * 255 + o[2] * inv + 0.5f));
                o[3] = uint8_t(std::min(255.0f, sa * 255 + o[3] * inv + 0.5f));
            }
        }
        pen += g.advance;
    }
}

void drawList(Image& dst, const DrawList& list, float s) {
    for (const auto& cmd : list.cmds) {
        switch (cmd.type) {
            case DrawCmd::Type::Clear: {
                Color c = cmd.color;
                uint8_t px[4] = {uint8_t(c.r * c.a * 255 + 0.5f), uint8_t(c.g * c.a * 255 + 0.5f),
                                 uint8_t(c.b * c.a * 255 + 0.5f), uint8_t(c.a * 255 + 0.5f)};
                for (size_t i = 0; i < dst.px.size(); i += 4) std::copy_n(px, 4, dst.px.data() + i);
                break;
            }
            case DrawCmd::Type::Rect: fillRect(dst, cmd.a * s, cmd.b * s, cmd.c * s, cmd.d * s, cmd.color); break;
            case DrawCmd::Type::Circle: {
                float r = cmd.c * s;
                int n = std::clamp(int(r * 0.75f), 16, 256);
                std::vector<Vec2> pts(n);
                for (int i = 0; i < n; ++i) {
                    float a = 2 * std::numbers::pi_v<float> * i / n;
                    pts[i] = {cmd.a * s + r * std::cos(a), cmd.b * s + r * std::sin(a)};
                }
                fillPolygon(dst, pts, cmd.color);
                break;
            }
            case DrawCmd::Type::Line: {
                Vec2 p0{cmd.a * s, cmd.b * s}, p1{cmd.c * s, cmd.d * s};
                float dx = p1.x - p0.x, dy = p1.y - p0.y, len = std::hypot(dx, dy);
                if (len <= 0) break;
                float hw = std::max(0.5f, cmd.e * s * 0.5f);
                float nx = -dy / len * hw, ny = dx / len * hw;
                Vec2 pts[4] = {{p0.x + nx, p0.y + ny}, {p1.x + nx, p1.y + ny}, {p1.x - nx, p1.y - ny}, {p0.x - nx, p0.y - ny}};
                fillPolygon(dst, pts, cmd.color);
                break;
            }
            case DrawCmd::Type::Text:
                if (auto f = Font::sans()) drawText(dst, *f, cmd.text, cmd.a * s, cmd.b * s, cmd.c * s, cmd.color);
                break;
            case DrawCmd::Type::Poly: {
                std::vector<Vec2> pts;
                for (size_t i = 0; i + 1 < cmd.points.size(); i += 2) pts.push_back({cmd.points[i] * s, cmd.points[i + 1] * s});
                fillPolygon(dst, pts, cmd.color);
                break;
            }
        }
    }
}

}  // namespace mf
