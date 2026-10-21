#include "model/chapters.hpp"

#include <fmt/format.h>

#include <cmath>

namespace mf {

std::vector<Chapter> chaptersIn(const Project& p, double start, double end) {
    std::vector<Chapter> out;
    for (const Marker& m : p.markers) {  // sorted by time
        if (m.title.empty()) continue;
        if (!out.empty()) out.back().end = m.time - start;
        out.push_back({m.time - start, end - start, m.title});
    }
    std::erase_if(out, [&](const Chapter& c) { return c.end <= 1e-9 || c.start >= end - start - 1e-9; });
    for (Chapter& c : out) c.start = std::max(0.0, c.start);
    return out;
}

namespace {

std::string clock(double t) {
    auto s = int64_t(std::floor(t + 1e-6));
    if (s >= 3600) return fmt::format("{}:{:02d}:{:02d}", s / 3600, s / 60 % 60, s % 60);
    return fmt::format("{}:{:02d}", s / 60, s % 60);
}

}  // namespace

std::string youtubeChapterList(const std::vector<Chapter>& chapters) {
    std::string out;
    for (const Chapter& c : chapters) out += fmt::format("{} {}\n", clock(c.start), c.title);
    return out;
}

std::vector<std::string> youtubeChapterProblems(const std::vector<Chapter>& chapters) {
    std::vector<std::string> out;
    if (chapters.empty()) return {"no markers have a title"};
    if (std::floor(chapters[0].start + 1e-6) > 0) out.push_back(fmt::format("the first chapter starts at {}, not 0:00", clock(chapters[0].start)));
    if (chapters.size() < 3) out.push_back(fmt::format("{} chapters; YouTube needs at least 3", chapters.size()));
    for (const Chapter& c : chapters)
        if (c.end - c.start < 10) out.push_back(fmt::format("'{}' lasts {:.1f} s; YouTube needs at least 10", c.title, c.end - c.start));
    return out;
}

}  // namespace mf
