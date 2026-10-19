#pragma once

#include "core/value.hpp"
#include "render/image.hpp"

#include <array>

namespace mf {

struct FrameScopes {
    std::array<uint32_t, 256> histogram{};
    Image waveform;
};

// Luma histogram and a 128-level waveform, colored with premultiplied alpha.
FrameScopes analyzeFrameScopes(const Image& frame, Color color);

} // namespace mf
