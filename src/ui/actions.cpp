#include "ui/actions.hpp"

#include <algorithm>

namespace mf::ui {

void Actions::add(Action a) {
    auto it = std::ranges::find(actions_, a.id, &Action::id);
    if (it != actions_.end()) *it = std::move(a);
    else actions_.push_back(std::move(a));
}

const Action* Actions::find(const std::string& id) const {
    auto it = std::ranges::find(actions_, id, &Action::id);
    return it == actions_.end() ? nullptr : &*it;
}

bool Actions::run(const std::string& id) const {
    const Action* a = find(id);
    if (!a || !a->run) return false;
    a->run();
    return true;
}

const std::vector<KeyChord>& Actions::bindings(const std::string& id) const {
    static const std::vector<KeyChord> none;
    if (auto it = overrides_.find(id); it != overrides_.end()) return it->second;
    const Action* a = find(id);
    return a ? a->defaults : none;
}

void Actions::setBindings(const std::string& id, std::vector<KeyChord> chords) {
    const Action* a = find(id);
    if (a && a->defaults == chords) overrides_.erase(id);
    else overrides_[id] = std::move(chords);
}

void Actions::resetBindings(const std::string& id) { overrides_.erase(id); }

std::vector<std::string> Actions::bound(const KeyChord& c) const {
    std::vector<std::string> out;
    for (const auto& a : actions_)
        if (std::ranges::contains(bindings(a.id), c)) out.push_back(a.id);
    return out;
}

bool Actions::dispatch(const KeyChord& c, bool typing) const {
    for (const auto& a : actions_) {
        if (!std::ranges::contains(bindings(a.id), c)) continue;
        if (typing && !a.whileTyping && textFieldChord(c)) continue;
        if (a.run) a.run();
        return true;
    }
    return false;
}

nlohmann::json Actions::overridesJson() const {
    auto j = nlohmann::json::object();
    for (const auto& [id, chords] : overrides_) {
        auto list = nlohmann::json::array();
        for (const auto& c : chords) list.push_back(formatChord(c));
        j[id] = list;
    }
    return j;
}

void Actions::loadOverrides(const nlohmann::json& j) {
    overrides_.clear();
    if (!j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_array()) continue;
        std::vector<KeyChord> chords;
        for (const auto& s : it.value())
            if (s.is_string())
                if (auto c = parseChord(s.get<std::string>())) chords.push_back(*c);
        overrides_[it.key()] = std::move(chords);
    }
}

bool textFieldChord(const KeyChord& c) {
    // Plain and shifted keys type text or move the caret.
    if (!(c.mods & (ModCtrl | ModGui | ModAlt))) return true;
    // Word movement and deletion with Alt (but not Alt+digit, which switches spaces).
    if ((c.mods & ModAlt) && !(c.mods & (ModCtrl | ModGui))) {
        switch (c.key) {
            case SDL_SCANCODE_LEFT:
            case SDL_SCANCODE_RIGHT:
            case SDL_SCANCODE_BACKSPACE:
            case SDL_SCANCODE_DELETE: return true;
            default: return false;
        }
    }
    if ((c.mods & (ModGui | ModCtrl)) && !(c.mods & ModAlt)) {
        switch (c.key) {
            case SDL_SCANCODE_A:
            case SDL_SCANCODE_C:
            case SDL_SCANCODE_V:
            case SDL_SCANCODE_X:
            case SDL_SCANCODE_Z:
            case SDL_SCANCODE_LEFT:
            case SDL_SCANCODE_RIGHT:
            case SDL_SCANCODE_UP:
            case SDL_SCANCODE_DOWN:
            case SDL_SCANCODE_BACKSPACE: return true;
            default: return false;
        }
    }
    return false;
}

}  // namespace mf::ui
