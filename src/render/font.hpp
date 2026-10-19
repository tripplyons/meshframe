#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct FT_FaceRec_;

namespace mf {

// Decodes one UTF-8 code point at s[i] and advances i. Invalid bytes yield U+FFFD.
uint32_t nextCodepoint(std::string_view s, size_t& i);

struct Glyph {
    int w = 0, h = 0;
    int left = 0, top = 0;  // bitmap offset from the pen position; top is above the baseline
    float advance = 0;
    std::vector<uint8_t> alpha;
};

struct FontMetrics {
    float ascent = 0, descent = 0, lineHeight = 0;
};

// A FreeType face with a glyph cache keyed by code point and pixel size.
// Thread-safe; glyph references stay valid for the font's lifetime.
class Font {
public:
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    // Loads (or returns the already loaded) font at path. Null on failure.
    static std::shared_ptr<Font> load(const std::string& path, std::string* err = nullptr);
    // System UI font and monospace font; never null when any fallback exists.
    static std::shared_ptr<Font> sans();
    static std::shared_ptr<Font> mono();

    FontMetrics metrics(float px);
    const Glyph& glyph(uint32_t codepoint, float px);
    // Width of a single line at px.
    float measure(std::string_view text, float px);
    const std::string& path() const { return path_; }

private:
    Font() = default;
    void setSize(float px);
    const Glyph& glyphAtSize(uint32_t codepoint, int size);

    FT_FaceRec_* face_ = nullptr;
    std::string path_;
    std::mutex mu_;
    float currentPx_ = 0;
    std::unordered_map<uint64_t, std::unique_ptr<Glyph>> glyphs_;
    std::unordered_map<int, FontMetrics> metrics_;
};

}  // namespace mf
