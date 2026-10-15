#pragma once

#include <string>
#include <vector>

namespace mf {

struct SceneCut {
    double time = 0;   // source seconds of the first frame of the new shot
    double score = 0;  // 0..1: mean color change from the previous frame
};

// Decodes every frame of the first `duration` seconds at low resolution and
// reports frames whose picture changes by more than `threshold` from the one
// before. False on a decode error.
bool detectCuts(const std::string& path, double duration, double threshold, std::vector<SceneCut>& out, std::string* err);

}  // namespace mf
