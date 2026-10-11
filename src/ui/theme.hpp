#pragma once

#include "core/value.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string_view>

namespace mf::ui {

// Every color the UI draws comes from one of these slots. Defaults are
// grayscale; settings may set any color.
#define MF_THEME_SLOTS(X)                                  \
    X(Background, "background", 0.110f)                    \
    X(Panel, "panel", 0.140f)                              \
    X(Border, "border", 0.250f)                            \
    X(Text, "text", 0.860f)                                \
    X(TextDim, "text-dim", 0.580f)                         \
    X(TextFaint, "text-faint", 0.400f)                     \
    X(Field, "field", 0.075f)                              \
    X(Hover, "hover", 0.190f)                              \
    X(Pressed, "pressed", 0.240f)                          \
    X(Selection, "selection", 0.300f)                      \
    X(Accent, "accent", 0.920f)                            \
    X(Caret, "caret", 0.950f)                              \
    X(TextSelection, "text-selection", 0.320f)             \
    X(Ruler, "ruler", 0.130f)                              \
    X(Grid, "grid", 0.165f)                                \
    X(Playhead, "playhead", 0.960f)                        \
    X(ClipMedia, "clip-media", 0.300f)                     \
    X(ClipAudio, "clip-audio", 0.240f)                     \
    X(ClipText, "clip-text", 0.360f)                       \
    X(ClipSolid, "clip-solid", 0.330f)                     \
    X(ClipGenerator, "clip-generator", 0.270f)             \
    X(ClipScript, "clip-script", 0.200f)                   \
    X(ClipLabel, "clip-label", 0.900f)                     \
    X(ClipSelected, "clip-selected", 0.950f)               \
    X(Waveform, "waveform", 0.560f)                        \
    X(Transition, "transition", 0.700f)                    \
    X(Keyframe, "keyframe", 0.900f)                        \
    X(Curve, "curve", 0.800f)                              \
    X(Meter, "meter", 0.620f)                              \
    X(MeterPeak, "meter-peak", 0.950f)                     \
    X(Error, "error", 1.000f)                              \
    X(ErrorBackground, "error-background", 0.270f)         \
    X(Warning, "warning", 0.750f)                          \
    X(CodeKeyword, "code-keyword", 0.960f)                 \
    X(CodeString, "code-string", 0.680f)                   \
    X(CodeNumber, "code-number", 0.780f)                   \
    X(CodeComment, "code-comment", 0.450f)                 \
    X(LineNumber, "line-number", 0.350f)                   \
    X(PreviewBackground, "preview-background", 0.050f)     \
    X(GraphNode, "graph-node", 0.180f)                     \
    X(GraphEdge, "graph-edge", 0.420f)                     \
    X(GraphActive, "graph-active", 0.900f)

enum class Slot : int {
#define MF_SLOT_ENUM(id, name, gray) id,
    MF_THEME_SLOTS(MF_SLOT_ENUM)
#undef MF_SLOT_ENUM
    Count
};

constexpr int kSlotCount = int(Slot::Count);

const char* slotName(Slot s);
std::optional<Slot> parseSlot(std::string_view name);
Color defaultColor(Slot s);

struct Theme {
    std::array<Color, kSlotCount> colors;

    Theme();
    const Color& operator[](Slot s) const { return colors[size_t(s)]; }
    Color& operator[](Slot s) { return colors[size_t(s)]; }
    bool operator==(const Theme&) const = default;

    // Only colors that differ from the defaults are written.
    nlohmann::json toJson() const;
    // Unknown names and bad colors are ignored.
    static Theme fromJson(const nlohmann::json& j);
};

}  // namespace mf::ui
