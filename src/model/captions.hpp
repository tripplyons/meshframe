#pragma once

#include "model/project.hpp"

#include <map>
#include <string>
#include <vector>

namespace mf {

// One caption on screen: one or two lines from start to end, in timeline time.
struct Caption {
    double start = 0, end = 0;
    std::vector<std::string> lines;
};

struct CaptionOptions {
    int lineLength = 42;                          // most characters on a line; a caption has up to two
    std::map<std::string, std::string> replace;  // whole words to respell, such as {"VLLM", "vLLM"}
};

// Captions for the speech heard on the timeline: the transcript words of each
// media clip on an unmuted audio track, placed where the clip plays them.
// Captions follow sentences and pauses; a long sentence splits into even
// parts, at a comma when one is near.
std::vector<Caption> timelineCaptions(const Project& p, const CaptionOptions& options);

// SubRip text, or WebVTT when `vtt` is set.
std::string formatCaptions(const std::vector<Caption>& captions, bool vtt);

}  // namespace mf
