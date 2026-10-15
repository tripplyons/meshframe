#include "render/waveform.hpp"

#include "render/font.hpp"
#include "render/raster.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>

namespace mf {

Image drawWaveform(const AudioAnalysis& a, int height, const std::vector<std::pair<double, std::string>>& marks) {
    int w = int(a.columns.size());
    if (w == 0 || a.duration <= 0) return {};
    float px = 12;
    int axis = int(px * 1.8f);
    int h = std::max(16, height);
    Image out(w, h + axis);
    auto rect = [&](int x0, int y0, int x1, int y1, Color c) {
        x0 = std::clamp(x0, 0, out.w), x1 = std::clamp(x1, 0, out.w);
        y0 = std::clamp(y0, 0, out.h), y1 = std::clamp(y1, 0, out.h);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                uint8_t* p = out.at(x, y);
                p[0] = uint8_t(c.r * 255), p[1] = uint8_t(c.g * 255), p[2] = uint8_t(c.b * 255), p[3] = 255;
            }
    };
    auto xAt = [&](double t) { return int(std::floor((t - a.start) / a.duration * w)); };
    auto yAt = [&](float v) { return int(std::lround((1 - std::clamp(v, -1.0f, 1.0f)) * 0.5f * (h - 1))); };

    rect(0, 0, w, out.h, Color::gray(0.08f));
    for (const auto& s : a.silence) rect(xAt(s.start), 0, std::max(xAt(s.end), xAt(s.start) + 1), h, Color::gray(0.15f));
    rect(0, h / 2, w, h / 2 + 1, Color::gray(0.25f));
    for (int x = 0; x < w; ++x) {
        auto [lo, hi] = a.columns[size_t(x)];
        rect(x, yAt(hi), x + 1, yAt(lo) + 1, Color::gray(0.75f));
    }
    Color red{0.9f, 0.25f, 0.25f, 1};
    for (const auto& c : a.clipping) {
        int x0 = xAt(c.start), x1 = std::max(xAt(c.end), x0 + 1);
        rect(x0, 0, x1, 4, red);
        rect(x0, h - 4, x1, h, red);
    }

    std::shared_ptr<Font> font = Font::sans();
    for (const auto& [t, name] : marks) {
        int x = xAt(t);
        if (x < 0 || x >= w) continue;
        rect(x, 0, x + 1, h, Color::gray(0.55f));
        if (!font) continue;
        int tw = int(std::ceil(font->measure(name, px)));
        rect(x + 1, 6, x + tw + 7, 6 + int(px * 1.5f), Color::gray(0.55f));
        drawText(out, *font, name, float(x + 4), 6 + px * 0.15f, px, Color::gray(0.05f));
    }

    // Time axis with labeled ticks about 100 px apart.
    rect(0, h, w, h + 1, Color::gray(0.3f));
    static const double steps[] = {0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
    double step = steps[std::size(steps) - 1];
    for (double s : steps)
        if (s / a.duration * w >= 100) {
            step = s;
            break;
        }
    for (double t = std::ceil(a.start / step) * step; t <= a.start + a.duration + 1e-9; t += step) {
        int x = xAt(t);
        rect(x, h, x + 1, h + axis / 3, Color::gray(0.5f));
        std::string label = step < 1 ? fmt::format("{:.2f}", t) : fmt::format("{}:{:02}", int(t) / 60, int(t) % 60);
        if (font && x + 3 < w) drawText(out, *font, label, float(x + 3), float(h + 2), px, Color::gray(0.7f));
    }
    return out;
}

}  // namespace mf
