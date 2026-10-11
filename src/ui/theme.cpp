#include "ui/theme.hpp"

#include <cmath>

namespace mf::ui {

namespace {

struct SlotInfo {
    const char* name;
    float gray;
};

constexpr SlotInfo kSlots[] = {
#define MF_SLOT_INFO(id, name, gray) {name, gray},
    MF_THEME_SLOTS(MF_SLOT_INFO)
#undef MF_SLOT_INFO
};

}  // namespace

const char* slotName(Slot s) { return kSlots[int(s)].name; }

std::optional<Slot> parseSlot(std::string_view name) {
    for (int i = 0; i < kSlotCount; ++i)
        if (name == kSlots[i].name) return Slot(i);
    return std::nullopt;
}

// Quantized to 8 bits so defaults survive a hex round trip.
Color defaultColor(Slot s) { return Color::gray(std::round(kSlots[int(s)].gray * 255.0f) / 255.0f); }

Theme::Theme() {
    for (int i = 0; i < kSlotCount; ++i) colors[i] = defaultColor(Slot(i));
}

nlohmann::json Theme::toJson() const {
    auto j = nlohmann::json::object();
    for (int i = 0; i < kSlotCount; ++i)
        if (colors[i] != defaultColor(Slot(i))) j[kSlots[i].name] = formatColor(colors[i]);
    return j;
}

Theme Theme::fromJson(const nlohmann::json& j) {
    Theme t;
    if (!j.is_object()) return t;
    for (auto it = j.begin(); it != j.end(); ++it) {
        auto slot = parseSlot(it.key());
        if (!slot || !it.value().is_string()) continue;
        if (auto c = parseColor(it.value().get<std::string>())) t[*slot] = *c;
    }
    return t;
}

}  // namespace mf::ui
