#include "render/raster.hpp"

#include "core/work_counter.hpp"
#include "core/thread_pool.hpp"
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
        // Split where the segment crosses the box's left or right side. Each
        // part then lies on one side, and clamping moves only outside parts
        // onto the border, where they still count for pixels to their right.
        float splits[2];
        int count = 0;
        for (float side : {0.0f, float(w_)}) {
            if ((p0.x < side) == (p1.x < side)) continue;
            float t = (side - p0.x) / (p1.x - p0.x);
            if (t > 0 && t < 1) splits[count++] = t;
        }
        if (count == 2 && splits[0] > splits[1]) std::swap(splits[0], splits[1]);
        Vec2 from = p0;
        for (int i = 0; i < count; ++i) {
            float t = splits[i];
            Vec2 to{p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t};
            to.x = std::abs(to.x) < std::abs(to.x - float(w_)) ? 0.0f : float(w_);
            segment(from, to);
            from = to;
        }
        segment(from, p1);
    }

    void fill(Image& dst, Color c) const {
        float pr = c.r * c.a, pg = c.g * c.a, pb = c.b * c.a;
        work::addRaster(uint64_t(w_) * h_);
        auto rows = [&](size_t begin, size_t end) {
            for (size_t y = begin; y < end; ++y) {
                const float* row = acc_.data() + size_t(y) * (w_ + 2);
                uint8_t* out = dst.at(x0_, y0_ + int(y));
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
        };
        if (uint64_t(w_) * h_ >= 65536)
            ThreadPool::shared().parallelFor(0, size_t(h_), rows, 32);
        else rows(0, size_t(h_));
    }

private:
    // Accumulates one segment in box coordinates. Parts left or right of the
    // box are clamped onto its sides.
    void segment(Vec2 p0, Vec2 p1) {
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
        uint64_t edges = uint64_t(std::max(0, yend - ystart));
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
                    edges += uint64_t(std::max(0, xbi - 1 - (xai + 2)));
                    for (int xi = xai + 2; xi < xbi - 1; ++xi) row[xi] += d * s;
                    float a2 = a1 + float(xbi - xai - 3) * s;
                    row[xbi - 1] += d * (1 - a2 - am);
                }
                row[xbi] += d * am;
            }
            x = xnext;
        }
        work::addEdge(edges);
    }

    Vec2 local(Vec2 p) const { return {p.x - x0_, p.y - y0_}; }
    int x0_, y0_, w_, h_;
    std::vector<float> acc_;
};

void fillRect(Image& dst, float x, float y, float w, float h, Color c) {
    if (dst.empty() || c.a <= 0) return;
    float left = std::min(x, x + w), right = std::max(x, x + w);
    float top = std::min(y, y + h), bottom = std::max(y, y + h);
    if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) || !std::isfinite(bottom) ||
        left >= right || top >= bottom) return;
    int x0 = int(std::clamp(std::floor(left), 0.0f, float(dst.w)));
    int x1 = int(std::clamp(std::ceil(right), 0.0f, float(dst.w)));
    int y0 = int(std::clamp(std::floor(top), 0.0f, float(dst.h)));
    int y1 = int(std::clamp(std::ceil(bottom), 0.0f, float(dst.h)));
    if (x0 >= x1 || y0 >= y1) return;
    const int fullStart = int(std::clamp(std::ceil(left), float(x0), float(x1)));
    const int fullEnd = int(std::clamp(std::floor(right), float(x0), float(x1)));
    const float pr = c.r * c.a, pg = c.g * c.a, pb = c.b * c.a;
    auto fringe = [&](int px, int py, float rowCoverage) {
        float columnCoverage = std::clamp(std::min(float(px + 1), right) - std::max(float(px), left), 0.0f, 1.0f);
        float cov = rowCoverage * columnCoverage;
        if (cov <= 0.002f) return;
        uint8_t* out = dst.at(px, py);
        float sa = c.a * cov, inv = 1 - sa;
        out[0] = uint8_t(std::min(255.0f, pr * cov * 255 + out[0] * inv + 0.5f));
        out[1] = uint8_t(std::min(255.0f, pg * cov * 255 + out[1] * inv + 0.5f));
        out[2] = uint8_t(std::min(255.0f, pb * cov * 255 + out[2] * inv + 0.5f));
        out[3] = uint8_t(std::min(255.0f, sa * 255 + out[3] * inv + 0.5f));
    };
    work::addRaster(uint64_t(x1 - x0) * (y1 - y0));
    for (int py = y0; py < y1; ++py) {
        float cov = std::clamp(std::min(float(py + 1), bottom) - std::max(float(py), top), 0.0f, 1.0f);
        if (fullStart >= fullEnd) {
            // A subpixel rectangle can have two fringes and no whole texel.
            for (int px = x0; px < x1; ++px) fringe(px, py, cov);
            continue;
        }
        if (x0 < fullStart) fringe(x0, py, cov);
        if (fullEnd < x1) fringe(fullEnd, py, cov);
        if (cov <= 0.002f) continue;
        float sa = c.a * cov, inv = 1 - sa;
        const float source[4] = {pr * cov * 255, pg * cov * 255, pb * cov * 255, sa * 255};
        uint8_t* out = dst.at(fullStart, py);
        if (inv == 0) {
            uint8_t pixel[4];
            for (int channel = 0; channel < 4; ++channel)
                pixel[channel] = uint8_t(std::min(255.0f, source[channel] + 0.5f));
            for (int px = fullStart; px < fullEnd; ++px, out += 4) std::copy_n(pixel, 4, out);
        } else {
            for (int px = fullStart; px < fullEnd; ++px, out += 4)
                for (int channel = 0; channel < 4; ++channel)
                    out[channel] = uint8_t(std::min(255.0f, source[channel] + out[channel] * inv + 0.5f));
        }
    }
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

TextBlock layoutTextBlock(Font& font, const std::string& text, const TextLayout& layout) {
    TextBlock b;
    float px = layout.px;
    std::istringstream in(text);
    for (std::string para; std::getline(in, para);) {
        if (layout.wrap <= 0) {
            b.lines.push_back(para);
            continue;
        }
        // Greedy wrap at spaces; a word wider than the limit keeps its own line.
        std::istringstream words(para);
        std::string line;
        for (std::string word; words >> word;) {
            std::string next = line.empty() ? word : line + " " + word;
            if (!line.empty() && font.measure(next, px) > layout.wrap) {
                b.lines.push_back(line);
                line = word;
            } else {
                line = std::move(next);
            }
        }
        b.lines.push_back(line);
    }
    if (b.lines.empty()) return b;
    float base = font.metrics(px).lineHeight;
    b.lineHeight = base * std::max(0.0f, layout.lineSpacing);
    b.margin = 1 + std::max(0.0f, layout.outline);
    float width = 0;
    for (const auto& l : b.lines) {
        b.widths.push_back(font.measure(l, px));
        width = std::max(width, b.widths.back());
    }
    b.w = std::max(2, int(std::ceil(width + 2 * b.margin)));
    b.h = std::max(2, int(std::ceil(b.lineHeight * float(b.lines.size() - 1) + base + 2 * b.margin)));
    return b;
}

Image outlineOf(const Image& src, float width, Color color) {
    // Squared distance from each pixel to the nearest mostly covered pixel,
    // by two passes of the 1D lower-envelope transform (Felzenszwalb).
    int w = src.w, h = src.h;
    constexpr float kFar = 1e20f;
    std::vector<float> d(size_t(w) * h);
    for (size_t i = 0; i < d.size(); ++i) d[i] = src.px[i * 4 + 3] >= 128 ? 0 : kFar;
    auto axis = [&](bool vertical) {
        const int length = vertical ? h : w, count = vertical ? w : h;
        ThreadPool::shared().parallelFor(0, size_t(count), [&](size_t begin, size_t end) {
            std::vector<float> line(length), z(length + 1);
            std::vector<int> v(length);
            auto index = [&](int column, int q) { return vertical ? size_t(q) * w + column : size_t(column) * w + q; };
            for (size_t column = begin; column < end; ++column) {
                for (int q = 0; q < length; ++q) line[q] = d[index(int(column), q)];
                auto meet = [&](int q, int p) {
                    return ((line[q] + float(q) * q) - (line[p] + float(p) * p)) / (2.0f * float(q - p));
                };
                int k = 0;
                v[0] = 0;
                z[0] = -kFar, z[1] = kFar;
                for (int q = 1; q < length; ++q) {
                    float crossing = meet(q, v[k]);
                    while (k > 0 && crossing <= z[k]) crossing = meet(q, v[--k]);
                    v[++k] = q;
                    z[k] = crossing, z[k + 1] = kFar;
                }
                k = 0;
                for (int q = 0; q < length; ++q) {
                    while (z[k + 1] < float(q)) ++k;
                    d[index(int(column), q)] = float(q - v[k]) * float(q - v[k]) + line[v[k]];
                }
                work::addDistance(uint64_t(length));
            }
        }, 32);
    };
    if (w == 0 || h == 0) return Image(w, h);
    axis(true);
    axis(false);
    Image out = Image::forOverwrite(w, h);
    work::addDistance(d.size());
    for (size_t i = 0; i < d.size(); ++i) {
        float a = std::clamp(width + 0.5f - std::sqrt(d[i]), 0.0f, 1.0f);
        a = std::max(a, src.px[i * 4 + 3] / 255.0f) * color.a;
        uint8_t* o = out.px.data() + i * 4;
        o[0] = uint8_t(color.r * a * 255 + 0.5f);
        o[1] = uint8_t(color.g * a * 255 + 0.5f);
        o[2] = uint8_t(color.b * a * 255 + 0.5f);
        o[3] = uint8_t(a * 255 + 0.5f);
    }
    return out;
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
            work::addGlyph(uint64_t(g.w) * uint64_t(g.h));
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
                work::addCopy(dst.px.size() / 4);
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
