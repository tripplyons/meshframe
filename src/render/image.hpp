#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

namespace mf {

namespace detail {
// On macOS an allocation occupies whole VM pages so Metal can share it.
size_t pixelAllocationSize(size_t bytes);
size_t pixelAlignment();
void* allocatePixels(size_t bytes);
void freePixels(void* pointer, size_t bytes) noexcept;

template <class T> struct PixelAllocator {
    using value_type = T;
    // Construction policy does not affect allocation/deallocation compatibility.
    using is_always_equal = std::true_type;
    PixelAllocator() = default;
    explicit PixelAllocator(bool initialize) noexcept : initialize_(initialize) {}
    template <class U> PixelAllocator(const PixelAllocator<U>&) noexcept {}
    T* allocate(size_t n) {
        if (n > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
        return static_cast<T*>(allocatePixels(n * sizeof(T)));
    }
    void deallocate(T* p, size_t n) noexcept { freePixels(p, n * sizeof(T)); }
    template <class U> void construct(U* p) {
        if (initialize_) std::construct_at(p);
        else ::new (static_cast<void*>(p)) U;
    }
    template <class U> bool operator==(const PixelAllocator<U>&) const noexcept { return true; }

private:
    bool initialize_ = true;
};

using PixelBytes = std::vector<uint8_t, PixelAllocator<uint8_t>>;
PixelBytes overwritePixels(size_t bytes);
}  // namespace detail

// RGBA8 image with premultiplied alpha, rows tightly packed.
struct Image {
    int w = 0, h = 0;
    detail::PixelBytes px;

    Image() = default;
    Image(int width, int height) : w(width), h(height), px(size_t(width) * height * 4, 0) {}
    // The caller must overwrite every pixel before reading or exposing it.
    static Image forOverwrite(int width, int height);

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
