#include "render/scopes.hpp"

#include <algorithm>
#include <cmath>

namespace mf {

FrameScopes analyzeFrameScopes(const Image& image, Color color) {
    FrameScopes result;
    if (image.empty()) return result;
    constexpr int levels = 128;
    std::vector<uint32_t> counts(size_t(image.w) * levels, 0);
    for (int y = 0; y < image.h; ++y)
        for (int x = 0; x < image.w; ++x) {
            const uint8_t* pixel = image.at(x, y);
            float luma = (0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2]) / 255.0f;
            result.histogram[std::min(255, int(luma * 255 + 0.5f))]++;
            counts[size_t(std::min(levels - 1, int(luma * (levels - 1) + 0.5f))) * image.w + x]++;
        }
    result.waveform = Image::forOverwrite(image.w, levels);
    std::vector<std::array<uint8_t, 4>> colors(size_t(image.h) + 1);
    for (int count = 0; count <= image.h; ++count) {
        float value = std::min(1.0f, std::sqrt(float(count) / float(image.h)) * 2.5f);
        colors[size_t(count)] = {uint8_t(color.r * value * 255), uint8_t(color.g * value * 255),
                                 uint8_t(color.b * value * 255), uint8_t(value * 255)};
    }
    for (int row = 0; row < levels; ++row)
        for (int x = 0; x < image.w; ++x) {
            uint8_t* pixel = result.waveform.at(x, levels - 1 - row);
            const auto& rgba = colors[counts[size_t(row) * image.w + x]];
            std::copy(rgba.begin(), rgba.end(), pixel);
        }
    return result;
}

} // namespace mf
