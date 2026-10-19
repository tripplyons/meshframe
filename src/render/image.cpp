#include "render/image.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#if defined(__APPLE__) && defined(MF_METAL)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace mf {

namespace detail {
size_t pixelAlignment() {
#if defined(__APPLE__) && defined(MF_METAL)
    static const size_t page = [] {
        long size = sysconf(_SC_PAGESIZE);
        return size > 0 ? size_t(size) : size_t(16384);
    }();
    return page;
#else
    return alignof(std::max_align_t);
#endif
}

size_t pixelAllocationSize(size_t bytes) {
    const size_t alignment = pixelAlignment();
    if (bytes > std::numeric_limits<size_t>::max() - (alignment - 1)) throw std::bad_alloc();
    return (bytes + alignment - 1) / alignment * alignment;
}

void* allocatePixels(size_t bytes) {
#if defined(__APPLE__) && defined(MF_METAL)
    if (!bytes) return nullptr;
    void* p = mmap(nullptr, pixelAllocationSize(bytes), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) throw std::bad_alloc();
    return p;
#else
    return ::operator new(bytes);
#endif
}

void freePixels(void* p, size_t bytes) noexcept {
#if defined(__APPLE__) && defined(MF_METAL)
    if (p) munmap(p, pixelAllocationSize(bytes));
#else
    ::operator delete(p);
#endif
}
}  // namespace detail

bool savePng(const Image& img, const char* path) {
    // Unpremultiply into a straight-alpha surface for the file.
    SDL_Surface* s = SDL_CreateSurface(img.w, img.h, SDL_PIXELFORMAT_RGBA32);
    if (!s) return false;
    for (int y = 0; y < img.h; ++y) {
        const uint8_t* src = img.row(y);
        auto* dst = static_cast<uint8_t*>(s->pixels) + size_t(y) * s->pitch;
        for (int x = 0; x < img.w; ++x) {
            uint8_t a = src[x * 4 + 3];
            for (int c = 0; c < 3; ++c) {
                int v = a ? (src[x * 4 + c] * 255 + a / 2) / a : 0;
                dst[x * 4 + c] = uint8_t(std::min(v, 255));
            }
            dst[x * 4 + 3] = a;
        }
    }
    bool ok = SDL_SavePNG(s, path);
    SDL_DestroySurface(s);
    return ok;
}

}  // namespace mf
