#include "render/font.hpp"

#include "core/work_counter.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cmath>
#include <filesystem>
#include <map>

namespace mf {

namespace {

std::mutex& libraryMutex() {
    static std::mutex m;
    return m;
}

FT_Library library() {
    static FT_Library lib = [] {
        FT_Library l = nullptr;
        FT_Init_FreeType(&l);
        return l;
    }();
    return lib;
}

std::shared_ptr<Font> firstAvailable(std::initializer_list<const char*> paths) {
    for (const char* p : paths)
        if (std::filesystem::exists(p))
            if (auto f = Font::load(p)) return f;
    return nullptr;
}

int sizeKey(float px) { return int(std::lround(px * 4)); }

}  // namespace

uint32_t nextCodepoint(std::string_view s, size_t& i) {
    auto b = uint8_t(s[i++]);
    if (b < 0x80) return b;
    int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    uint32_t cp = b & (0x3F >> extra);
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || (uint8_t(s[i]) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (uint8_t(s[i++]) & 0x3F);
    }
    return cp;
}

Font::~Font() {
    std::lock_guard lock(libraryMutex());
    if (face_) FT_Done_Face(face_);
}

std::shared_ptr<Font> Font::load(const std::string& path, std::string* err) {
    static std::map<std::string, std::weak_ptr<Font>> loaded;
    std::lock_guard lock(libraryMutex());
    if (auto f = loaded[path].lock()) return f;
    FT_Face face = nullptr;
    if (FT_New_Face(library(), path.c_str(), 0, &face) != 0) {
        if (err) *err = "cannot load font " + path;
        return nullptr;
    }
    std::shared_ptr<Font> f(new Font());
    f->face_ = face;
    f->path_ = path;
    loaded[path] = f;
    return f;
}

std::shared_ptr<Font> Font::sans() {
    static std::shared_ptr<Font> f = firstAvailable({
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    });
    return f;
}

std::shared_ptr<Font> Font::mono() {
    static std::shared_ptr<Font> f = [] {
        auto m = firstAvailable({
            "/System/Library/Fonts/SFNSMono.ttf",
            "/System/Library/Fonts/Menlo.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        });
        return m ? m : sans();
    }();
    return f;
}

void Font::setSize(float px) {
    if (px == currentPx_) return;
    FT_Set_Char_Size(face_, 0, FT_F26Dot6(std::lround(px * 64)), 72, 72);
    currentPx_ = px;
}

FontMetrics Font::metrics(float px) {
    std::lock_guard lock(mu_);
    int key = sizeKey(px);
    if (auto it = metrics_.find(key); it != metrics_.end()) return it->second;
    setSize(key / 4.0f);
    const auto& m = face_->size->metrics;
    FontMetrics fm{m.ascender / 64.0f, -m.descender / 64.0f, m.height / 64.0f};
    metrics_[key] = fm;
    return fm;
}

const Glyph& Font::glyph(uint32_t codepoint, float px) {
    std::lock_guard lock(mu_);
    return glyphAtSize(codepoint, sizeKey(px));
}

const Glyph& Font::glyphAtSize(uint32_t codepoint, int sk) {
    uint64_t key = (uint64_t(sk) << 32) | codepoint;
    if (auto it = glyphs_.find(key); it != glyphs_.end()) return *it->second;
    setSize(sk / 4.0f);
    auto g = std::make_unique<Glyph>();
    FT_UInt index = FT_Get_Char_Index(face_, codepoint);
    if (index == 0 && codepoint != ' ') index = FT_Get_Char_Index(face_, 0xFFFD);
    if (FT_Load_Glyph(face_, index, FT_LOAD_DEFAULT) == 0 &&
        FT_Render_Glyph(face_->glyph, FT_RENDER_MODE_NORMAL) == 0) {
        const FT_GlyphSlot slot = face_->glyph;
        const FT_Bitmap& bm = slot->bitmap;
        g->w = int(bm.width);
        g->h = int(bm.rows);
        g->left = slot->bitmap_left;
        g->top = slot->bitmap_top;
        g->advance = slot->advance.x / 64.0f;
        g->alpha.resize(size_t(g->w) * g->h);
        work::addGlyph(g->alpha.size());
        for (int y = 0; y < g->h; ++y)
            std::copy_n(bm.buffer + y * bm.pitch, g->w, g->alpha.data() + size_t(y) * g->w);
    }
    return *glyphs_.emplace(key, std::move(g)).first->second;
}

float Font::measure(std::string_view text, float px) {
    std::lock_guard lock(mu_);
    const int size = sizeKey(px);
    float w = 0;
    for (size_t i = 0; i < text.size();) w += glyphAtSize(nextCodepoint(text, i), size).advance;
    return w;
}

}  // namespace mf
