#include "model/schema.hpp"

#include <limits>
#include <stdexcept>

namespace mf {

namespace {

using VT = ValueType;

PropSpec num(const char* name, double def, double min, double max, double step, const char* unit = "") {
    return PropSpec{name, VT::Number, def, min, max, step, unit};
}
PropSpec color(const char* name, Color def) { return PropSpec{name, VT::Color, def}; }
PropSpec str(const char* name, std::string def, std::vector<const char*> choices = {}, bool animatable = true) {
    PropSpec s{name, VT::String, std::move(def)};
    s.choices = std::move(choices);
    s.animatable = animatable;
    return s;
}

std::vector<PropSpec> visualProps() {
    return {
        num("x", 0, -4000, 4000, 1, "px"),
        num("y", 0, -4000, 4000, 1, "px"),
        num("scale", 1, 0, 10, 0.01),
        num("rotation", 0, -360, 360, 0.1, "deg"),
        num("opacity", 1, 0, 1, 0.01),
        str("blend", "normal", {"normal", "add", "multiply", "screen"}),
    };
}

std::vector<PropSpec> audioProps() {
    return {
        num("gain", 0, -60, 12, 0.1, "dB"),
        num("pan", 0, -1, 1, 0.01),
    };
}

std::vector<PropSpec> concat(std::vector<PropSpec> a, std::vector<PropSpec> b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

struct ClipSpecs {
    std::vector<PropSpec> mediaVideo = concat(visualProps(), {str("fit", "contain", {"contain", "cover", "stretch", "none"}, false)});
    std::vector<PropSpec> mediaAudio = audioProps();
    std::vector<PropSpec> text = concat(visualProps(), {
        str("text", "Text"),
        num("size", 96, 4, 800, 1, "px"),
        color("color", Color::gray(1.0f)),
        str("font", "", {}, false),
        str("align", "center", {"left", "center", "right"}, false),
        num("lineHeight", 1, 0.5, 4, 0.01),
        num("wrap", 0, 0, 8000, 1, "px"),
        num("outline", 0, 0, 100, 0.5, "px"),
        color("outlineColor", Color::gray(0.0f)),
    });
    std::vector<PropSpec> solid = concat(visualProps(), {color("color", Color::gray(0.5f))});
    std::vector<PropSpec> generator = concat(visualProps(), {str("generator", "", {}, false)});
    std::vector<PropSpec> synth = concat(audioProps(), {str("synth", "", {}, false)});
};

const ClipSpecs& clipSpecs() {
    static const ClipSpecs specs;
    return specs;
}

}  // namespace

std::span<const PropSpec> clipPropSpecs(ClipKind kind, TrackKind track) {
    const auto& s = clipSpecs();
    switch (kind) {
        case ClipKind::Media: return track == TrackKind::Audio ? s.mediaAudio : s.mediaVideo;
        case ClipKind::Text: return s.text;
        case ClipKind::Solid: return s.solid;
        case ClipKind::Generator: return s.generator;
        case ClipKind::Synth: return s.synth;
    }
    return {};
}

const PropSpec* findClipProp(ClipKind kind, TrackKind track, std::string_view name) {
    for (const auto& p : clipPropSpecs(kind, track))
        if (name == p.name) return &p;
    return nullptr;
}

const std::vector<EffectSpec>& videoEffectSpecs() {
    static const std::vector<EffectSpec> specs = {
        {"brightness_contrast", "Brightness / contrast", {num("brightness", 0, -1, 1, 0.01), num("contrast", 1, 0, 4, 0.01)}},
        {"saturation", "Saturation", {num("amount", 1, 0, 4, 0.01)}},
        {"blur", "Blur", {num("radius", 4, 0, 200, 0.5, "px")}},
        {"invert", "Invert", {num("mix", 1, 0, 1, 0.01)}},
        {"tint", "Tint", {color("color", Color{1.0f, 0.8f, 0.6f, 1.0f}), num("amount", 0.5, 0, 1, 0.01)}},
        {"vignette", "Vignette", {num("amount", 0.5, 0, 1, 0.01), num("softness", 0.5, 0.01, 1, 0.01)}},
        {"threshold", "Threshold", {num("level", 0.5, 0, 1, 0.01)}},
        {"drop_shadow", "Drop shadow",
         {color("color", Color::gray(0.0f)), num("opacity", 0.6, 0, 1, 0.01), num("x", 6, -500, 500, 1, "px"),
          num("y", 6, -500, 500, 1, "px"), num("blur", 8, 0, 200, 0.5, "px")}},
        {"crop", "Crop",
         {num("left", 0, 0, 1, 0.001), num("right", 0, 0, 1, 0.001), num("top", 0, 0, 1, 0.001), num("bottom", 0, 0, 1, 0.001)}},
        {"mask", "Mask",
         {str("shape", "rectangle", {"rectangle", "ellipse"}, false), num("x", 0, -4000, 4000, 1, "px"),
          num("y", 0, -4000, 4000, 1, "px"), num("width", 400, 0, 8000, 1, "px"), num("height", 300, 0, 8000, 1, "px"),
          num("feather", 0, 0, 1000, 0.5, "px"), str("keep", "inside", {"inside", "outside"}, false)}},
    };
    return specs;
}

const std::vector<EffectSpec>& audioEffectSpecs() {
    static const std::vector<EffectSpec> specs = {
        {"gain", "Gain", {num("gain", 0, -60, 24, 0.1, "dB")}},
        {"pan", "Pan", {num("pan", 0, -1, 1, 0.01)}},
        {"lowpass", "Low-pass", {num("cutoff", 2000, 20, 20000, 1, "Hz"), num("q", 0.707, 0.1, 10, 0.01)}},
        {"highpass", "High-pass", {num("cutoff", 200, 20, 20000, 1, "Hz"), num("q", 0.707, 0.1, 10, 0.01)}},
        {"delay", "Delay", {num("time", 0.25, 0.001, 2, 0.001, "s"), num("feedback", 0.35, 0, 0.95, 0.01), num("mix", 0.3, 0, 1, 0.01)}},
        {"compressor", "Compressor",
         {num("threshold", -18, -60, 0, 0.1, "dB"), num("ratio", 4, 1, 20, 0.1), num("attack", 0.01, 0.0005, 0.5, 0.0005, "s"),
          num("release", 0.15, 0.01, 2, 0.01, "s"), num("makeup", 0, 0, 24, 0.1, "dB")}},
    };
    return specs;
}

const std::vector<EffectSpec>& transitionSpecs() {
    static const std::vector<EffectSpec> specs = {
        {"crossfade", "Crossfade", {}},
        {"wipe", "Wipe", {str("direction", "left", {"left", "right", "up", "down"}, false), num("softness", 0.02, 0, 0.5, 0.005)}},
        {"dip", "Dip to color", {color("color", Color::gray(0.0f))}},
        {"slide", "Slide", {str("direction", "left", {"left", "right", "up", "down"}, false)}},
    };
    return specs;
}

namespace {
const EffectSpec* findIn(const std::vector<EffectSpec>& specs, std::string_view type) {
    for (const auto& s : specs)
        if (type == s.type) return &s;
    return nullptr;
}
}  // namespace

const EffectSpec* findVideoEffect(std::string_view type) { return findIn(videoEffectSpecs(), type); }
const EffectSpec* findAudioEffect(std::string_view type) { return findIn(audioEffectSpecs(), type); }
const EffectSpec* findTransition(std::string_view type) { return findIn(transitionSpecs(), type); }

const PropSpec* findSpec(const std::vector<PropSpec>& specs, std::string_view name) {
    for (const auto& s : specs)
        if (name == s.name) return &s;
    return nullptr;
}

void applyDefaults(PropertyMap& props, std::span<const PropSpec> specs) {
    for (const auto& s : specs)
        if (!props.contains(s.name)) props.emplace(s.name, Property::of(s.def));
}

Value coerce(const PropSpec& spec, const Value& v) {
    switch (spec.type) {
        case VT::Number:
            if (typeOf(v) == VT::Number) return v;
            if (typeOf(v) == VT::String) {
                double fallback = std::numeric_limits<double>::quiet_NaN();
                double d = asNumber(v, fallback);
                if (d == d) return d;
            }
            break;
        case VT::Color:
            if (typeOf(v) == VT::Color) return v;
            if (typeOf(v) == VT::String) {
                if (auto c = parseColor(std::get<std::string>(v))) return *c;
            }
            if (typeOf(v) == VT::Number) return asColor(v);
            break;
        case VT::String:
            if (typeOf(v) == VT::String) {
                if (!spec.choices.empty()) {
                    for (auto c : spec.choices)
                        if (std::get<std::string>(v) == c) return v;
                    break;
                }
                return v;
            }
            if (spec.choices.empty()) return asString(v);
            break;
        case VT::None: return v;
    }
    throw std::runtime_error(std::string("invalid value for ") + spec.name + ": " + describe(v));
}

}  // namespace mf
