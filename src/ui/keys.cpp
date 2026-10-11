#include "ui/keys.hpp"

#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <cctype>

namespace mf::ui {

uint8_t modsFromSdl(uint16_t m) {
    uint8_t out = 0;
    if (m & SDL_KMOD_CTRL) out |= ModCtrl;
    if (m & SDL_KMOD_SHIFT) out |= ModShift;
    if (m & SDL_KMOD_ALT) out |= ModAlt;
    if (m & SDL_KMOD_GUI) out |= ModGui;
    return out;
}

std::string formatChord(const KeyChord& c) {
    std::string s;
    if (c.mods & ModCtrl) s += "Ctrl+";
    if (c.mods & ModAlt) s += "Alt+";
    if (c.mods & ModShift) s += "Shift+";
    if (c.mods & ModGui) s += "Cmd+";
    const char* name = SDL_GetScancodeName(c.key);
    s += (name && *name) ? name : "?";
    return s;
}

std::optional<KeyChord> parseChord(std::string_view text) {
    KeyChord c;
    auto lower = [](std::string_view s) {
        std::string out(s);
        std::ranges::transform(out, out.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        return out;
    };
    while (true) {
        size_t plus = text.find('+');
        // A trailing "+" names the plus key itself ("Cmd++" is not supported; use "=").
        if (plus == std::string_view::npos || plus + 1 == text.size()) break;
        std::string mod = lower(text.substr(0, plus));
        if (mod == "ctrl" || mod == "control") c.mods |= ModCtrl;
        else if (mod == "shift") c.mods |= ModShift;
        else if (mod == "alt" || mod == "option" || mod == "opt") c.mods |= ModAlt;
        else if (mod == "cmd" || mod == "command" || mod == "gui" || mod == "super" || mod == "meta") c.mods |= ModGui;
        else return std::nullopt;
        text.remove_prefix(plus + 1);
    }
    if (text.empty()) return std::nullopt;
    c.key = SDL_GetScancodeFromName(std::string(text).c_str());
    if (c.key == SDL_SCANCODE_UNKNOWN) return std::nullopt;
    return c;
}

bool isModifierKey(SDL_Scancode sc) { return sc >= SDL_SCANCODE_LCTRL && sc <= SDL_SCANCODE_RGUI; }

}  // namespace mf::ui
