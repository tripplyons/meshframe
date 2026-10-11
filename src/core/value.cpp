#include "core/value.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cmath>

namespace mf {

namespace {
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

std::optional<Color> parseColor(std::string_view text) {
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    if (text.size() != 6 && text.size() != 8) return std::nullopt;
    float channels[4] = {0, 0, 0, 1};
    for (size_t i = 0; i < text.size() / 2; ++i) {
        int hi = hexDigit(text[i * 2]);
        int lo = hexDigit(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        channels[i] = float(hi * 16 + lo) / 255.0f;
    }
    return Color{channels[0], channels[1], channels[2], channels[3]};
}

std::string formatColor(const Color& c) {
    auto byte = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    if (byte(c.a) == 255) return fmt::format("#{:02x}{:02x}{:02x}", byte(c.r), byte(c.g), byte(c.b));
    return fmt::format("#{:02x}{:02x}{:02x}{:02x}", byte(c.r), byte(c.g), byte(c.b), byte(c.a));
}

ValueType typeOf(const Value& v) { return static_cast<ValueType>(v.index()); }

const char* typeName(ValueType t) {
    switch (t) {
        case ValueType::None: return "none";
        case ValueType::Number: return "number";
        case ValueType::Color: return "color";
        case ValueType::String: return "string";
    }
    return "none";
}

double asNumber(const Value& v, double fallback) {
    if (auto d = std::get_if<double>(&v)) return *d;
    if (auto s = std::get_if<std::string>(&v)) {
        double out = fallback;
        auto res = std::from_chars(s->data(), s->data() + s->size(), out);
        if (res.ec == std::errc()) return out;
    }
    return fallback;
}

Color asColor(const Value& v, Color fallback) {
    if (auto c = std::get_if<Color>(&v)) return *c;
    if (auto s = std::get_if<std::string>(&v)) return parseColor(*s).value_or(fallback);
    if (auto d = std::get_if<double>(&v)) {
        float g = float(std::clamp(*d, 0.0, 1.0));
        return Color{g, g, g, 1};
    }
    return fallback;
}

std::string asString(const Value& v) {
    if (auto s = std::get_if<std::string>(&v)) return *s;
    return describe(v);
}

std::string describe(const Value& v) {
    switch (typeOf(v)) {
        case ValueType::None: return "-";
        case ValueType::Number: {
            double d = std::get<double>(v);
            if (std::abs(d - std::round(d)) < 1e-9 && std::abs(d) < 1e12) return fmt::format("{}", int64_t(std::round(d)));
            return fmt::format("{:.4g}", d);
        }
        case ValueType::Color: return formatColor(std::get<Color>(v));
        case ValueType::String: return std::get<std::string>(v);
    }
    return "-";
}

}  // namespace mf
