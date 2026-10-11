#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace mf {

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
    bool operator==(const Color&) const = default;

    static constexpr Color gray(float v, float a = 1) { return {v, v, v, a}; }
};

std::optional<Color> parseColor(std::string_view text);
std::string formatColor(const Color& c);

// A value flowing through the reactive graph or stored in a property.
using Value = std::variant<std::monostate, double, Color, std::string>;

enum class ValueType { None, Number, Color, String };

ValueType typeOf(const Value& v);
const char* typeName(ValueType t);
double asNumber(const Value& v, double fallback = 0.0);
Color asColor(const Value& v, Color fallback = {});
std::string asString(const Value& v);
std::string describe(const Value& v);

// Looks up a named parameter of an effect or transition.
using ParamFn = std::function<Value(std::string_view)>;

}  // namespace mf
