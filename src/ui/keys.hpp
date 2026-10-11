#pragma once

#include <SDL3/SDL_scancode.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mf::ui {

enum Mod : uint8_t { ModCtrl = 1, ModShift = 2, ModAlt = 4, ModGui = 8 };

// A key with modifiers. Scancodes keep bindings independent of keyboard
// layout and of what Option or Shift would type.
struct KeyChord {
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    uint8_t mods = 0;

    bool valid() const { return key != SDL_SCANCODE_UNKNOWN; }
    bool operator==(const KeyChord&) const = default;
};

uint8_t modsFromSdl(uint16_t sdlMods);
// "Alt+Shift+1", "Cmd+Z", "Space". Gui prints as Cmd and Alt as Alt.
std::string formatChord(const KeyChord& c);
// Accepts the format above; also Ctrl, Control, Option, Opt, Super, Meta, Command.
std::optional<KeyChord> parseChord(std::string_view text);
// True for keys that only modify (Shift, Ctrl, Alt, Gui).
bool isModifierKey(SDL_Scancode sc);

}  // namespace mf::ui
