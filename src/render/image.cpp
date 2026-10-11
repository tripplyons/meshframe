#include "render/image.hpp"

#include <SDL3/SDL.h>

#include <algorithm>

namespace mf {

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
