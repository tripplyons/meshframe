#include "render/compositor.hpp"

#include "core/thread_pool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace mf {

namespace {

inline uint8_t u8(float v) { return uint8_t(std::clamp(v, 0.0f, 255.0f) + 0.5f); }

void forRows(int h, const std::function<void(int, int)>& fn) {
    ThreadPool::shared().parallelFor(0, size_t(h), [&](size_t b, size_t e) { fn(int(b), int(e)); }, 8);
}

// Blends one premultiplied source pixel (0..1 floats) into dst.
inline void blendPixel(uint8_t* d, float sr, float sg, float sb, float sa, Blend mode) {
    float dr = d[0] / 255.0f, dg = d[1] / 255.0f, db = d[2] / 255.0f, da = d[3] / 255.0f;
    float r, g, b, a;
    switch (mode) {
        case Blend::Normal:
            r = sr + dr * (1 - sa);
            g = sg + dg * (1 - sa);
            b = sb + db * (1 - sa);
            a = sa + da * (1 - sa);
            break;
        case Blend::Add:
            r = std::min(1.0f, sr + dr);
            g = std::min(1.0f, sg + dg);
            b = std::min(1.0f, sb + db);
            a = std::min(1.0f, sa + da);
            break;
        case Blend::Multiply:
            r = sr * dr + sr * (1 - da) + dr * (1 - sa);
            g = sg * dg + sg * (1 - da) + dg * (1 - sa);
            b = sb * db + sb * (1 - da) + db * (1 - sa);
            a = sa + da * (1 - sa);
            break;
        case Blend::Screen:
            r = sr + dr - sr * dr;
            g = sg + dg - sg * dg;
            b = sb + db - sb * db;
            a = sa + da * (1 - sa);
            break;
    }
    d[0] = u8(r * 255);
    d[1] = u8(g * 255);
    d[2] = u8(b * 255);
    d[3] = u8(a * 255);
}

double numParam(const ParamFn& p, std::string_view name, double fallback) { return asNumber(p(name), fallback); }

float luma(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// Calls fn(r, g, b, a) on straight-alpha floats in place, for every pixel with alpha.
template <class Fn>
void mapStraight(Image& img, Fn fn) {
    forRows(img.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            uint8_t* p = img.row(y);
            for (int x = 0; x < img.w; ++x, p += 4) {
                if (!p[3]) continue;
                float a = p[3] / 255.0f;
                float r = p[0] / 255.0f / a, g = p[1] / 255.0f / a, b = p[2] / 255.0f / a;
                fn(r, g, b, x, y);
                p[0] = u8(std::clamp(r, 0.0f, 1.0f) * a * 255);
                p[1] = u8(std::clamp(g, 0.0f, 1.0f) * a * 255);
                p[2] = u8(std::clamp(b, 0.0f, 1.0f) * a * 255);
            }
        }
    });
}

// One horizontal box-blur pass on rows; the transposed call blurs columns.
void boxBlurRows(const Image& src, Image& dst, int r) {
    forRows(src.h, [&](int y0, int y1) {
        std::vector<int> sum(4);
        float norm = 1.0f / float(2 * r + 1);
        for (int y = y0; y < y1; ++y) {
            const uint8_t* s = src.row(y);
            uint8_t* d = dst.row(y);
            std::fill(sum.begin(), sum.end(), 0);
            auto px = [&](int x) { return s + std::clamp(x, 0, src.w - 1) * 4; };
            for (int x = -r; x <= r; ++x)
                for (int c = 0; c < 4; ++c) sum[c] += px(x)[c];
            for (int x = 0; x < src.w; ++x) {
                for (int c = 0; c < 4; ++c) d[x * 4 + c] = uint8_t(sum[c] * norm + 0.5f);
                const uint8_t* add = px(x + r + 1);
                const uint8_t* sub = px(x - r);
                for (int c = 0; c < 4; ++c) sum[c] += add[c] - sub[c];
            }
        }
    });
}

void transpose(const Image& src, Image& dst) {
    dst.resize(src.h, src.w);
    forRows(src.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < src.w; ++x) std::copy_n(src.at(x, y), 4, dst.at(y, x));
    });
}

void blur(Image& img, double radius) {
    // Three box passes approximate a gaussian with sigma near radius / 2.
    int r = int(std::lround(radius / 2));
    if (r < 1) return;
    Image tmp(img.w, img.h), t2;
    for (int i = 0; i < 3; ++i) {
        boxBlurRows(img, tmp, r);
        std::swap(img, tmp);
    }
    transpose(img, t2);
    tmp.resize(t2.w, t2.h);
    for (int i = 0; i < 3; ++i) {
        boxBlurRows(t2, tmp, r);
        std::swap(t2, tmp);
    }
    transpose(t2, img);
}

void shiftInto(Image& out, const Image& src, int dx, int dy) {
    forRows(out.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            int sy = y - dy;
            if (sy < 0 || sy >= src.h) continue;
            for (int x = 0; x < out.w; ++x) {
                int sx = x - dx;
                if (sx < 0 || sx >= src.w) continue;
                const uint8_t* s = src.at(sx, sy);
                blendPixel(out.at(x, y), s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f, s[3] / 255.0f, Blend::Normal);
            }
        }
    });
}

// Multiplies each premultiplied pixel by f(x, y).
template <class F>
void scaleAlpha(Image& img, F f) {
    forRows(img.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            uint8_t* p = img.row(y);
            for (int x = 0; x < img.w; ++x, p += 4) {
                float k = f(x, y);
                if (k >= 1) continue;
                for (int c = 0; c < 4; ++c) p[c] = u8(p[c] * k);
            }
        }
    });
}

}  // namespace

Blend parseBlend(std::string_view s) {
    if (s == "add") return Blend::Add;
    if (s == "multiply") return Blend::Multiply;
    if (s == "screen") return Blend::Screen;
    return Blend::Normal;
}

void fill(Image& img, Color c) {
    uint8_t px[4] = {u8(c.r * c.a * 255), u8(c.g * c.a * 255), u8(c.b * c.a * 255), u8(c.a * 255)};
    for (size_t i = 0; i < img.px.size(); i += 4) std::copy_n(px, 4, img.px.data() + i);
}

void drawLayer(Image& dst, const Image& src, const Placement& at, float opacity, Blend blend) {
    if (src.empty() || dst.empty() || opacity <= 0 || at.sx == 0 || at.sy == 0) return;
    double cs = std::cos(at.rotation), sn = std::sin(at.rotation);
    double hw = src.w / 2.0, hh = src.h / 2.0;
    // Destination bounding box of the transformed layer.
    double minx = 1e30, maxx = -1e30, miny = 1e30, maxy = -1e30;
    for (auto [u, v] : std::array<std::pair<double, double>, 4>{{{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}}}) {
        double x = at.cx + (u * at.sx) * cs - (v * at.sy) * sn;
        double y = at.cy + (u * at.sx) * sn + (v * at.sy) * cs;
        minx = std::min(minx, x), maxx = std::max(maxx, x), miny = std::min(miny, y), maxy = std::max(maxy, y);
    }
    int x0 = std::max(0, int(std::floor(minx))), x1 = std::min(dst.w, int(std::ceil(maxx)));
    int y0 = std::max(0, int(std::floor(miny))), y1 = std::min(dst.h, int(std::ceil(maxy)));
    if (x0 >= x1 || y0 >= y1) return;

    double ox = at.cx - hw, oy = at.cy - hh;
    bool aligned = at.rotation == 0 && at.sx == 1 && at.sy == 1 && ox == std::floor(ox) && oy == std::floor(oy);
    forRows(y1 - y0, [&](int r0, int r1) {
        for (int y = y0 + r0; y < y0 + r1; ++y) {
            uint8_t* d = dst.at(x0, y);
            for (int x = x0; x < x1; ++x, d += 4) {
                float s[4];
                if (aligned) {
                    int sx = x - int(ox), sy = y - int(oy);
                    if (sx < 0 || sy < 0 || sx >= src.w || sy >= src.h) continue;
                    const uint8_t* p = src.at(sx, sy);
                    if (!p[3]) continue;
                    for (int c = 0; c < 4; ++c) s[c] = p[c] / 255.0f;
                } else {
                    // Inverse transform from the pixel center into source pixel space.
                    double px = x + 0.5 - at.cx, py = y + 0.5 - at.cy;
                    double u = (px * cs + py * sn) / at.sx + hw - 0.5;
                    double v = (-px * sn + py * cs) / at.sy + hh - 0.5;
                    if (u <= -1 || v <= -1 || u >= src.w || v >= src.h) continue;
                    int iu = int(std::floor(u)), iv = int(std::floor(v));
                    float fu = float(u - iu), fv = float(v - iv);
                    std::fill_n(s, 4, 0.0f);
                    auto tap = [&](int tx, int ty, float wgt) {
                        if (tx < 0 || ty < 0 || tx >= src.w || ty >= src.h || wgt == 0) return;
                        const uint8_t* p = src.at(tx, ty);
                        for (int c = 0; c < 4; ++c) s[c] += p[c] * wgt;
                    };
                    tap(iu, iv, (1 - fu) * (1 - fv));
                    tap(iu + 1, iv, fu * (1 - fv));
                    tap(iu, iv + 1, (1 - fu) * fv);
                    tap(iu + 1, iv + 1, fu * fv);
                    for (float& c : s) c /= 255.0f;
                    if (s[3] <= 0) continue;
                }
                blendPixel(d, s[0] * opacity, s[1] * opacity, s[2] * opacity, s[3] * opacity, blend);
            }
        }
    });
}

int effectMargin(std::string_view type, const ParamFn& param, double unit) {
    if (type != "drop_shadow") return 0;
    double offset = std::max(std::abs(numParam(param, "x", 6)), std::abs(numParam(param, "y", 6)));
    return int(std::ceil((offset + 1.5 * std::max(0.0, numParam(param, "blur", 8))) * unit)) + 1;
}

void applyEffect(Image& img, std::string_view type, const ParamFn& param, double unit, PixelBox content) {
    if (content.w <= 0 || content.h <= 0) content = {0, 0, img.w, img.h};
    if (type == "brightness_contrast") {
        float bright = float(numParam(param, "brightness", 0)), contrast = float(numParam(param, "contrast", 1));
        mapStraight(img, [&](float& r, float& g, float& b, int, int) {
            for (float* c : {&r, &g, &b}) *c = (*c - 0.5f) * contrast + 0.5f + bright;
        });
    } else if (type == "saturation") {
        float amount = float(numParam(param, "amount", 1));
        mapStraight(img, [&](float& r, float& g, float& b, int, int) {
            float l = luma(r, g, b);
            for (float* c : {&r, &g, &b}) *c = l + (*c - l) * amount;
        });
    } else if (type == "blur") {
        blur(img, numParam(param, "radius", 4) * unit);
    } else if (type == "invert") {
        float mix = float(numParam(param, "mix", 1));
        mapStraight(img, [&](float& r, float& g, float& b, int, int) {
            for (float* c : {&r, &g, &b}) *c += (1 - 2 * *c) * mix;
        });
    } else if (type == "tint") {
        Color tint = asColor(param("color"), Color{1, 0.8f, 0.6f, 1});
        float amount = float(numParam(param, "amount", 0.5));
        mapStraight(img, [&](float& r, float& g, float& b, int, int) {
            float l = luma(r, g, b);
            r += (l * tint.r - r) * amount;
            g += (l * tint.g - g) * amount;
            b += (l * tint.b - b) * amount;
        });
    } else if (type == "vignette") {
        float amount = float(numParam(param, "amount", 0.5)), soft = float(numParam(param, "softness", 0.5));
        float cx = content.x + content.w / 2.0f, cy = content.y + content.h / 2.0f;
        float maxd = std::hypot(content.w / 2.0f, content.h / 2.0f);
        float inner = 1 - std::clamp(soft, 0.01f, 1.0f);
        mapStraight(img, [&](float& r, float& g, float& b, int x, int y) {
            float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy) / maxd;
            float t = std::clamp((d - inner) / (1 - inner + 1e-6f), 0.0f, 1.0f);
            float f = 1 - amount * t * t * (3 - 2 * t);
            r *= f, g *= f, b *= f;
        });
    } else if (type == "threshold") {
        float level = float(numParam(param, "level", 0.5));
        mapStraight(img, [&](float& r, float& g, float& b, int, int) {
            float v = luma(r, g, b) >= level ? 1.0f : 0.0f;
            r = g = b = v;
        });
    } else if (type == "drop_shadow") {
        Color c = asColor(param("color"), Color::gray(0));
        float a = float(std::clamp(numParam(param, "opacity", 0.6), 0.0, 1.0)) * c.a;
        Image shadow(img.w, img.h);
        for (size_t i = 0; i < img.px.size(); i += 4) {
            float k = img.px[i + 3] / 255.0f * a;
            shadow.px[i] = u8(c.r * k * 255), shadow.px[i + 1] = u8(c.g * k * 255), shadow.px[i + 2] = u8(c.b * k * 255);
            shadow.px[i + 3] = u8(k * 255);
        }
        blur(shadow, numParam(param, "blur", 8) * unit);
        Image out(img.w, img.h);
        shiftInto(out, shadow, int(std::lround(numParam(param, "x", 6) * unit)), int(std::lround(numParam(param, "y", 6) * unit)));
        shiftInto(out, img, 0, 0);
        img = std::move(out);
    } else if (type == "crop") {
        // Edges as fractions of the content; partly covered pixels fade.
        auto clamp01 = [&](const char* name) { return std::clamp(numParam(param, name, 0), 0.0, 1.0); };
        double l = content.x + clamp01("left") * content.w, r = content.x + (1 - clamp01("right")) * content.w;
        double t = content.y + clamp01("top") * content.h, b = content.y + (1 - clamp01("bottom")) * content.h;
        auto overlap = [](int i, double lo, double hi) { return float(std::clamp(std::min(hi, i + 1.0) - std::max(lo, double(i)), 0.0, 1.0)); };
        scaleAlpha(img, [&](int x, int y) { return overlap(x, l, r) * overlap(y, t, b); });
    } else if (type == "mask") {
        bool ellipse = asString(param("shape")) == "ellipse", outside = asString(param("keep")) == "outside";
        double cx = content.x + content.w / 2.0 + numParam(param, "x", 0) * unit;
        double cy = content.y + content.h / 2.0 + numParam(param, "y", 0) * unit;
        double hw = std::max(0.0, numParam(param, "width", 400)) * unit / 2, hh = std::max(0.0, numParam(param, "height", 300)) * unit / 2;
        double feather = std::max(1.0, numParam(param, "feather", 0) * unit);
        scaleAlpha(img, [&](int x, int y) {
            double px = x + 0.5 - cx, py = y + 0.5 - cy, d;
            if (ellipse) {
                // Distance to the ellipse, scaled near its edge by the smaller radius.
                double rx = std::max(hw, 1e-6), ry = std::max(hh, 1e-6);
                d = (std::hypot(px / rx, py / ry) - 1) * std::min(rx, ry);
            } else {
                double qx = std::abs(px) - hw, qy = std::abs(py) - hh;
                d = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0);
            }
            float k = float(std::clamp(0.5 - d / feather, 0.0, 1.0));
            return outside ? 1 - k : k;
        });
    }
}

void mixTransition(Image& out, const Image& a, const Image& b, std::string_view type, double m, const ParamFn& param) {
    out.resize(a.w, a.h);
    float mf = float(std::clamp(m, 0.0, 1.0));
    auto lerpImages = [&](const Image& x, const Image& y, float t) {
        forRows(out.h, [&](int y0, int y1) {
            for (int row = y0; row < y1; ++row) {
                const uint8_t *p = x.row(row), *q = y.row(row);
                uint8_t* o = out.row(row);
                for (int i = 0; i < out.w * 4; ++i) o[i] = u8(p[i] + (q[i] - p[i]) * t);
            }
        });
    };
    if (type == "wipe") {
        auto dir = asString(param("direction"));
        float soft = float(numParam(param, "softness", 0.02));
        forRows(out.h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const uint8_t *p = a.row(y), *q = b.row(y);
                uint8_t* o = out.row(y);
                for (int x = 0; x < out.w; ++x) {
                    float nx = (x + 0.5f) / out.w, ny = (y + 0.5f) / out.h;
                    // s is how far along the wipe this pixel lies; B covers pixels with s < m.
                    float s = dir == "right" ? nx : dir == "up" ? 1 - ny : dir == "down" ? ny : 1 - nx;
                    float k = soft > 0 ? std::clamp((mf * (1 + soft) - s) / soft, 0.0f, 1.0f) : (s < mf ? 1.0f : 0.0f);
                    for (int c = 0; c < 4; ++c) o[x * 4 + c] = u8(p[x * 4 + c] + (q[x * 4 + c] - p[x * 4 + c]) * k);
                }
            }
        });
    } else if (type == "dip") {
        Image solid(a.w, a.h);
        fill(solid, asColor(param("color"), Color::gray(0)));
        if (mf < 0.5f) lerpImages(a, solid, mf * 2);
        else lerpImages(solid, b, mf * 2 - 1);
    } else if (type == "slide") {
        auto dir = asString(param("direction"));
        int dx = 0, dy = 0;
        if (dir == "right") dx = -1;
        else if (dir == "up") dy = 1;
        else if (dir == "down") dy = -1;
        else dx = 1;
        // B enters from the side the motion comes from and pushes A out.
        int ax = -int(std::lround(dx * mf * out.w)), ay = -int(std::lround(dy * mf * out.h));
        int bx = int(std::lround(dx * (1 - mf) * out.w)), by = int(std::lround(dy * (1 - mf) * out.h));
        std::fill(out.px.begin(), out.px.end(), 0);
        shiftInto(out, a, ax, ay);
        shiftInto(out, b, bx, by);
    } else {
        lerpImages(a, b, mf);
    }
}

}  // namespace mf
