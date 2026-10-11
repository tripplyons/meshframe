#pragma once

#include "core/value.hpp"
#include "model/project.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace mf {

// Describes one property: its type, default, range, and how the UI edits it.
struct PropSpec {
    const char* name;
    ValueType type;
    Value def;
    double min = 0, max = 1, step = 0.01;
    const char* unit = "";
    std::vector<const char*> choices = {};  // for string enums
    bool animatable = true;
};

struct EffectSpec {
    const char* type;
    const char* label;
    std::vector<PropSpec> props;
};

std::span<const PropSpec> clipPropSpecs(ClipKind kind, TrackKind track);
const PropSpec* findClipProp(ClipKind kind, TrackKind track, std::string_view name);

const std::vector<EffectSpec>& videoEffectSpecs();
const std::vector<EffectSpec>& audioEffectSpecs();
const EffectSpec* findVideoEffect(std::string_view type);
const EffectSpec* findAudioEffect(std::string_view type);

const std::vector<EffectSpec>& transitionSpecs();
const EffectSpec* findTransition(std::string_view type);

const PropSpec* findSpec(const std::vector<PropSpec>& specs, std::string_view name);

// Fills missing properties with their defaults.
void applyDefaults(PropertyMap& props, std::span<const PropSpec> specs);

// Converts a value to the spec's type. Throws on a mismatch that cannot convert.
Value coerce(const PropSpec& spec, const Value& v);

}  // namespace mf
