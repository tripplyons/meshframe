#pragma once

#include "core/value.hpp"
#include "render/font.hpp"
#include "render/image.hpp"
#include "ui/geometry.hpp"

#include <SDL3/SDL_render.h>

#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mf::ui {

enum class FontRole { Ui, Mono };
enum class Align { Left, Center, Right };

// A texture that mirrors an Image (premultiplied RGBA8).
class Texture {
public:
    Texture() = default;
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    void update(SDL_Renderer* r, const Image& img);
    SDL_Texture* get() const { return tex_; }
    int w() const { return w_; }
    int h() const { return h_; }

private:
    SDL_Texture* tex_ = nullptr;
    int w_ = 0, h_ = 0;
};

// Draws UI primitives and text with an SDL renderer in physical pixels.
// Text comes from a glyph atlas texture filled on demand.
class Painter {
public:
    explicit Painter(SDL_Renderer* r);
    ~Painter();
    Painter(const Painter&) = delete;
    Painter& operator=(const Painter&) = delete;

    SDL_Renderer* renderer() const { return r_; }
    void setFont(FontRole role, std::shared_ptr<Font> font, float px);
    float fontPx(FontRole role) const { return fonts_[int(role)].px; }

    // Starts a frame: resets clipping and the atlas if it filled up.
    void begin(int width, int height);
    int width() const { return width_; }
    int height() const { return height_; }

    void fill(const Rect& r, const Color& c);
    // 1 px lines.
    void hline(float x0, float x1, float y, const Color& c);
    void vline(float x, float y0, float y1, const Color& c);
    void outline(const Rect& r, const Color& c);
    // A thick line as a quad.
    void line(Vec a, Vec b, const Color& c, float width = 1);
    void polyline(std::span<const Vec> pts, const Color& c, float width = 1);
    void diamond(Vec center, float radius, const Color& c);
    void triangle(Vec a, Vec b, Vec c, const Color& col);

    // Draws a single line of text with its line box's top at y. Returns the advance.
    float text(float x, float y, std::string_view s, const Color& c, FontRole role = FontRole::Ui);
    // Vertically centered in r, clipped with an ellipsis when too wide.
    void textIn(const Rect& r, std::string_view s, const Color& c, Align align = Align::Left, FontRole role = FontRole::Ui);
    float measure(std::string_view s, FontRole role = FontRole::Ui);
    float lineHeight(FontRole role = FontRole::Ui) const;
    // x offset of each byte boundary in s (size s.size() + 1), for caret placement.
    std::vector<float> caretStops(std::string_view s, FontRole role = FontRole::Ui);

    void image(const Texture& t, const Rect& dst);

    void pushClip(const Rect& r);
    void popClip();
    Rect clip() const { return clips_.empty() ? Rect{0, 0, float(width_), float(height_)} : clips_.back(); }

private:
    struct FontSlot {
        std::shared_ptr<Font> font;
        float px = 13;
        FontMetrics metrics;
    };
    struct AtlasGlyph {
        float u0, v0, u1, v1;
        int w, h, left, top;
        float advance;
    };
    const AtlasGlyph* glyph(FontRole role, uint32_t cp);
    void applyClip();

    SDL_Renderer* r_;
    SDL_Texture* atlas_ = nullptr;
    int atlasSize_ = 1024;
    int shelfX_ = 0, shelfY_ = 0, shelfH_ = 0;
    bool atlasFull_ = false;
    std::unordered_map<uint64_t, AtlasGlyph> glyphs_;
    FontSlot fonts_[2];
    std::vector<Rect> clips_;
    std::vector<SDL_Vertex> verts_;
    std::vector<int> indices_;
    int width_ = 0, height_ = 0;
};

inline SDL_FColor toF(const Color& c) { return {c.r, c.g, c.b, c.a}; }

}  // namespace mf::ui
