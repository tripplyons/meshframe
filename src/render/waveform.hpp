#pragma once

#include "audio/analysis.hpp"
#include "render/image.hpp"

#include <string>
#include <utility>
#include <vector>

namespace mf {

// Draws an analysis's waveform columns over a time axis. Silent ranges are
// shaded, clipping is red, and `marks` (time, name) are labeled lines.
Image drawWaveform(const AudioAnalysis& a, int height, const std::vector<std::pair<double, std::string>>& marks = {});

}  // namespace mf
