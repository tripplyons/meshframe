#include "ui/painter.hpp"

#include <cmath>
#include <cstring>

namespace mf::ui {

Texture::~Texture() {
    if (tex_) SDL_DestroyTexture(tex_);
}

void Texture::update(SDL_Renderer* r, const Image& img) {
    if (img.empty()) return;
    if (!tex_ || w_ != img.w || h_ != img.h) {
        if (tex_) SDL_DestroyTexture(tex_);
        tex_ = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, img.w, img.h);
        if (!tex_) return;
        SDL_SetTextureBlendMode(tex_, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        SDL_SetTextureScaleMode(tex_, SDL_SCALEMODE_LINEAR);
        w_ = img.w;
        h_ = img.h;
    }
    SDL_UpdateTexture(tex_, nullptr, img.px.data(), img.w * 4);
}

Painter::Painter(SDL_Renderer* r) : r_(r) {
    atlas_ = SDL_CreateTexture(r_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, atlasSize_, atlasSize_);
    SDL_SetTextureBlendMode(atlas_, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(atlas_, SDL_SCALEMODE_NEAREST);
    setFont(FontRole::Ui, Font::sans(), 13);
    setFont(FontRole::Mono, Font::mono(), 13);
}

Painter::~Painter() {
    if (atlas_) SDL_DestroyTexture(atlas_);
}

void Painter::setFont(FontRole role, std::shared_ptr<Font> font, float px) {
    auto& slot = fonts_[int(role)];
    if (slot.font == font && slot.px == px) return;
    slot.font = std::move(font);
    slot.px = px;
    slot.metrics = slot.font ? slot.font->metrics(px) : FontMetrics{};
    atlasFull_ = true;  // reset on the next begin(); old entries are keyed by the old font
}

void Painter::begin(int width, int height) {
    width_ = width;
    height_ = height;
    clips_.clear();
    SDL_SetRenderClipRect(r_, nullptr);
    SDL_SetRenderDrawBlendMode(r_, SDL_BLENDMODE_BLEND);
    if (atlasFull_) {
        glyphs_.clear();
        shelfX_ = shelfY_ = shelfH_ = 0;
        atlasFull_ = false;
    }
}

void Painter::fill(const Rect& r, const Color& c) {
    if (r.empty()) return;
    SDL_SetRenderDrawColorFloat(r_, c.r, c.g, c.b, c.a);
    SDL_FRect fr{r.x, r.y, r.w, r.h};
    SDL_RenderFillRect(r_, &fr);
}

void Painter::hline(float x0, float x1, float y, const Color& c) { fill({x0, y, x1 - x0, 1}, c); }
void Painter::vline(float x, float y0, float y1, const Color& c) { fill({x, y0, 1, y1 - y0}, c); }

void Painter::outline(const Rect& r, const Color& c) {
    if (r.empty()) return;
    hline(r.x, r.r(), r.y, c);
    hline(r.x, r.r(), r.b() - 1, c);
    vline(r.x, r.y + 1, r.b() - 1, c);
    vline(r.r() - 1, r.y + 1, r.b() - 1, c);
}

void Painter::line(Vec a, Vec b, const Color& c, float width) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    float nx = -dy / len * width * 0.5f, ny = dx / len * width * 0.5f;
    SDL_FColor fc = toF(c);
    SDL_Vertex v[4] = {{{a.x + nx, a.y + ny}, fc, {}}, {{b.x + nx, b.y + ny}, fc, {}},
                       {{b.x - nx, b.y - ny}, fc, {}}, {{a.x - nx, a.y - ny}, fc, {}}};
    int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(r_, nullptr, v, 4, idx, 6);
}

void Painter::polyline(std::span<const Vec> pts, const Color& c, float width) {
    if (pts.size() < 2) return;
    verts_.clear();
    indices_.clear();
    SDL_FColor fc = toF(c);
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        Vec a = pts[i], b = pts[i + 1];
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f) continue;
        float nx = -dy / len * width * 0.5f, ny = dx / len * width * 0.5f;
        int base = int(verts_.size());
        verts_.push_back({{a.x + nx, a.y + ny}, fc, {}});
        verts_.push_back({{b.x + nx, b.y + ny}, fc, {}});
        verts_.push_back({{b.x - nx, b.y - ny}, fc, {}});
        verts_.push_back({{a.x - nx, a.y - ny}, fc, {}});
        for (int k : {0, 1, 2, 0, 2, 3}) indices_.push_back(base + k);
    }
    if (!verts_.empty()) SDL_RenderGeometry(r_, nullptr, verts_.data(), int(verts_.size()), indices_.data(), int(indices_.size()));
}

void Painter::diamond(Vec c, float rad, const Color& col) {
    SDL_FColor fc = toF(col);
    SDL_Vertex v[4] = {{{c.x, c.y - rad}, fc, {}}, {{c.x + rad, c.y}, fc, {}}, {{c.x, c.y + rad}, fc, {}}, {{c.x - rad, c.y}, fc, {}}};
    int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(r_, nullptr, v, 4, idx, 6);
}

void Painter::triangle(Vec a, Vec b, Vec c, const Color& col) {
    SDL_FColor fc = toF(col);
    SDL_Vertex v[3] = {{{a.x, a.y}, fc, {}}, {{b.x, b.y}, fc, {}}, {{c.x, c.y}, fc, {}}};
    SDL_RenderGeometry(r_, nullptr, v, 3, nullptr, 0);
}

const Painter::AtlasGlyph* Painter::glyph(FontRole role, uint32_t cp) {
    auto& slot = fonts_[int(role)];
    if (!slot.font) return nullptr;
    uint64_t key = (uint64_t(role) << 56) | (uint64_t(std::lround(slot.px * 4)) << 32) | cp;
    if (auto it = glyphs_.find(key); it != glyphs_.end()) return &it->second;
    if (atlasFull_) return nullptr;
    const Glyph& g = slot.font->glyph(cp, slot.px);
    AtlasGlyph a{0, 0, 0, 0, g.w, g.h, g.left, g.top, g.advance};
    if (g.w > 0 && g.h > 0) {
        if (shelfX_ + g.w + 1 > atlasSize_) {
            shelfX_ = 0;
            shelfY_ += shelfH_ + 1;
            shelfH_ = 0;
        }
        if (shelfY_ + g.h + 1 > atlasSize_ || g.w + 1 > atlasSize_) {
            atlasFull_ = true;
            return nullptr;
        }
        std::vector<uint8_t> rgba(size_t(g.w) * g.h * 4);
        for (int i = 0; i < g.w * g.h; ++i) {
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = g.alpha[i];
        }
        SDL_Rect dst{shelfX_, shelfY_, g.w, g.h};
        SDL_UpdateTexture(atlas_, &dst, rgba.data(), g.w * 4);
        float s = 1.0f / atlasSize_;
        a.u0 = shelfX_ * s;
        a.v0 = shelfY_ * s;
        a.u1 = (shelfX_ + g.w) * s;
        a.v1 = (shelfY_ + g.h) * s;
        shelfX_ += g.w + 1;
        shelfH_ = std::max(shelfH_, g.h);
    }
    return &glyphs_.emplace(key, a).first->second;
}

float Painter::text(float x, float y, std::string_view s, const Color& c, FontRole role) {
    const auto& slot = fonts_[int(role)];
    float baseline = std::round(y + slot.metrics.ascent);
    float pen = std::round(x);
    verts_.clear();
    indices_.clear();
    SDL_FColor fc = toF(c);
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = nextCodepoint(s, i);
        if (cp == '\t') cp = ' ';
        const AtlasGlyph* g = glyph(role, cp);
        if (!g) continue;
        if (g->w > 0) {
            float x0 = std::round(pen) + g->left, y0 = baseline - g->top;
            float x1 = x0 + g->w, y1 = y0 + g->h;
            int base = int(verts_.size());
            verts_.push_back({{x0, y0}, fc, {g->u0, g->v0}});
            verts_.push_back({{x1, y0}, fc, {g->u1, g->v0}});
            verts_.push_back({{x1, y1}, fc, {g->u1, g->v1}});
            verts_.push_back({{x0, y1}, fc, {g->u0, g->v1}});
            for (int k : {0, 1, 2, 0, 2, 3}) indices_.push_back(base + k);
        }
        pen += g->advance;
    }
    if (!verts_.empty()) SDL_RenderGeometry(r_, atlas_, verts_.data(), int(verts_.size()), indices_.data(), int(indices_.size()));
    return pen - std::round(x);
}

float Painter::measure(std::string_view s, FontRole role) {
    float w = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = nextCodepoint(s, i);
        if (cp == '\t') cp = ' ';
        if (const AtlasGlyph* g = glyph(role, cp)) w += g->advance;
    }
    return w;
}

std::vector<float> Painter::caretStops(std::string_view s, FontRole role) {
    std::vector<float> stops(s.size() + 1, 0);
    float w = 0;
    for (size_t i = 0; i < s.size();) {
        size_t start = i;
        uint32_t cp = nextCodepoint(s, i);
        if (cp == '\t') cp = ' ';
        for (size_t k = start; k < i; ++k) stops[k] = w;
        if (const AtlasGlyph* g = glyph(role, cp)) w += g->advance;
    }
    stops[s.size()] = w;
    return stops;
}

float Painter::lineHeight(FontRole role) const { return std::ceil(fonts_[int(role)].metrics.lineHeight); }

void Painter::textIn(const Rect& r, std::string_view s, const Color& c, Align align, FontRole role) {
    float y = r.y + std::floor((r.h - lineHeight(role)) / 2);
    float w = measure(s, role);
    std::string clipped;
    if (w > r.w) {
        // Trim whole code points until the text plus an ellipsis fits.
        const std::string_view ell = "\xE2\x80\xA6";
        float ew = measure(ell, role);
        auto stops = caretStops(s, role);
        size_t n = s.size();
        while (n > 0 && stops[n] + ew > r.w) {
            --n;
            while (n > 0 && (uint8_t(s[n]) & 0xC0) == 0x80) --n;
        }
        clipped = std::string(s.substr(0, n)) + std::string(ell);
        s = clipped;
        w = stops[n] + ew;
        align = Align::Left;
    }
    float x = r.x;
    if (align == Align::Center) x = r.x + std::floor((r.w - w) / 2);
    else if (align == Align::Right) x = r.r() - w;
    pushClip(r);
    text(x, y, s, c, role);
    popClip();
}

void Painter::image(const Texture& t, const Rect& dst) {
    if (!t.get()) return;
    SDL_FRect d{dst.x, dst.y, dst.w, dst.h};
    SDL_RenderTexture(r_, t.get(), nullptr, &d);
}

void Painter::pushClip(const Rect& r) {
    clips_.push_back(clip().intersect(r));
    applyClip();
}

void Painter::popClip() {
    if (!clips_.empty()) clips_.pop_back();
    applyClip();
}

void Painter::applyClip() {
    if (clips_.empty()) {
        SDL_SetRenderClipRect(r_, nullptr);
        return;
    }
    const Rect& c = clips_.back();
    int x0 = int(std::floor(c.x)), y0 = int(std::floor(c.y));
    int x1 = int(std::ceil(c.r())), y1 = int(std::ceil(c.b()));
    SDL_Rect r{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
    SDL_SetRenderClipRect(r_, &r);
}

}  // namespace mf::ui
