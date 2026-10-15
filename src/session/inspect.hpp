#pragma once

#include "audio/analysis.hpp"
#include "session/session.hpp"

#include <nlohmann/json.hpp>

namespace mf {

// Where a video clip draws at a time, in project pixels from the frame's
// top-left corner.
struct ClipBounds {
    bool visible = false;
    double left = 0, top = 0, right = 0, bottom = 0;
    double inFrame = 0;      // fraction of covered pixels inside the frame
    bool truncated = false;  // content reaches past the margin rendered around the frame

    // Inside the central 90% of the frame.
    bool titleSafe(const ProjectSettings& s) const;
};

// Renders the clip alone at `scale` of the project size (smaller is faster and
// coarser) and finds the pixels it covers, after effects.
ClipBounds measureClip(Session& s, const Clip& c, double t, double scale = 1);

// Measures the timeline mix from start to end.
AudioAnalysis analyzeMix(Session& s, double start, double end, const AudioAnalysisOptions& options);

struct CheckOptions {
    double step = 0.5;  // seconds between sampled frames
    double lufs = -14, lufsTolerance = 2;
    double truePeak = -1;  // dBTP
};

// Looks for common problems across the project. Returns {"issues": [...],
// "samples": N, "errors": N, "warnings": N}; each issue has severity, kind,
// start, end, a message, and the clip it concerns when there is one.
nlohmann::json checkProject(Session& s, const CheckOptions& options);

}  // namespace mf
