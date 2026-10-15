#include "core/timecode.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mf {

namespace {

std::optional<double> parseNumber(std::string_view s) {
    double d = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), d);
    if (ec != std::errc() || p != s.data() + s.size() || s.empty()) return std::nullopt;
    return d;
}

}  // namespace

std::string formatTimecode(double seconds, double fps) {
    if (fps <= 0) fps = 30;
    int perSecond = std::max(1, int(std::round(fps)));
    int64_t frames = int64_t(std::floor(std::max(0.0, seconds) * fps + 1e-6));
    int64_t s = frames / perSecond;
    return fmt::format("{:02}:{:02}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60, frames % perSecond);
}

std::optional<double> parseTime(std::string_view s, double fps) {
    if (s.starts_with('@')) return std::nullopt;
    if (s.ends_with('s')) return parseNumber(s.substr(0, s.size() - 1));
    if (s.ends_with('f')) {
        auto frames = parseNumber(s.substr(0, s.size() - 1));
        if (!frames || fps <= 0) return std::nullopt;
        return *frames / fps;
    }
    std::vector<std::string_view> parts;
    for (size_t start = 0;;) {
        size_t colon = s.find(':', start);
        parts.push_back(s.substr(start, colon == std::string_view::npos ? colon : colon - start));
        if (colon == std::string_view::npos) break;
        start = colon + 1;
    }
    if (parts.size() == 1) return parseNumber(s);
    if (parts.size() > 4) return std::nullopt;
    std::vector<double> v;
    for (size_t i = 0; i < parts.size(); ++i) {
        auto n = parseNumber(parts[i]);
        bool whole = parts.size() == 4 || i + 1 < parts.size();
        if (!n || *n < 0 || parts[i].starts_with('+') || (whole && *n != std::floor(*n))) return std::nullopt;
        v.push_back(*n);
    }
    if (parts.size() == 4) {
        if (fps <= 0 || v[1] >= 60 || v[2] >= 60 || v[3] >= std::ceil(fps)) return std::nullopt;
        return v[0] * 3600 + v[1] * 60 + v[2] + v[3] / fps;
    }
    if (v.back() >= 60 || (parts.size() == 3 && v[1] >= 60)) return std::nullopt;
    return parts.size() == 3 ? v[0] * 3600 + v[1] * 60 + v[2] : v[0] * 60 + v[1];
}

}  // namespace mf
