#pragma once

#include "model/project.hpp"

#include <string>
#include <vector>

namespace mf {

// A titled stretch of the timeline. Markers with a title start chapters; each
// runs to the next titled marker or the end.
struct Chapter {
    double start = 0, end = 0;
    std::string title;
};

// The chapters between start and end, with times measured from start, as an
// export of that range plays them. A chapter that starts before `start` but
// is still running there begins at 0.
std::vector<Chapter> chaptersIn(const Project& p, double start, double end);

// "M:SS Title" lines (or "H:MM:SS" from an hour on), the format YouTube reads
// from a video description.
std::string youtubeChapterList(const std::vector<Chapter>& chapters);

// What keeps YouTube from showing the chapters: the first must start at 0:00,
// there must be at least three, and each must last at least 10 seconds.
std::vector<std::string> youtubeChapterProblems(const std::vector<Chapter>& chapters);

}  // namespace mf
