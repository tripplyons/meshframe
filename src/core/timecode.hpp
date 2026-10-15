#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace mf {

// "00:01:02:15" (hours, minutes, seconds, frames).
std::string formatTimecode(double seconds, double fps);

// Parses a time in seconds from "2.5", "90f" (frames), "1:15.5" or
// "1:02:03.5" (clock), or "00:00:03:12" (HH:MM:SS:FF timecode).
std::optional<double> parseTime(std::string_view s, double fps);

}  // namespace mf
