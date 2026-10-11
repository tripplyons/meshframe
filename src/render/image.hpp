#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace mf {

// RGBA8 image with premultiplied alpha, rows tightly packed.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;

    Image() = default;
    Image(int width, int height) : w(width), h(height), px(size_t(width) * height * 4, 0) {}

    void resize(int width, int height) {
        w = width;
        h = height;
        px.assign(size_t(width) * height * 4, 0);
    }
    uint8_t* row(int y) { return px.data() + size_t(y) * w * 4; }
    const uint8_t* row(int y) const { return px.data() + size_t(y) * w * 4; }
    uint8_t* at(int x, int y) { return px.data() + (size_t(y) * w + x) * 4; }
    const uint8_t* at(int x, int y) const { return px.data() + (size_t(y) * w + x) * 4; }
    bool empty() const { return w == 0 || h == 0; }
    size_t bytes() const { return px.size(); }
};

using ImagePtr = std::shared_ptr<const Image>;

bool savePng(const Image& img, const char* path);

}  // namespace mf
