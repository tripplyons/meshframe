#pragma once

#include "ui/keys.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mf::ui {

// A named command the user can bind to keys. The GUI, menus, and the CLI
// ("ui.action") run actions by id.
struct Action {
    std::string id;     // "space.1", "edit.undo"
    std::string label;  // "Switch to space 1"
    std::string group;  // "Spaces", "Edit"
    std::vector<KeyChord> defaults;
    std::function<void()> run;
    // Chords that a focused text field also uses (plain keys, Cmd+C) are not
    // dispatched while text input has focus unless this is set.
    bool whileTyping = false;
};

class Actions {
public:
    void add(Action a);
    const Action* find(const std::string& id) const;
    const std::vector<Action>& all() const { return actions_; }
    bool run(const std::string& id) const;

    // Current bindings: overrides where set, defaults otherwise.
    const std::vector<KeyChord>& bindings(const std::string& id) const;
    void setBindings(const std::string& id, std::vector<KeyChord> chords);
    void resetBindings(const std::string& id);
    void resetAll() { overrides_.clear(); }
    // Ids of actions bound to a chord.
    std::vector<std::string> bound(const KeyChord& c) const;
    // Runs the first action bound to the chord. `typing` means a text field has focus.
    bool dispatch(const KeyChord& c, bool typing) const;

    // Overrides only, as {"action": ["Cmd+Z", ...]}. Unknown chords are skipped on load.
    nlohmann::json overridesJson() const;
    void loadOverrides(const nlohmann::json& j);

private:
    std::vector<Action> actions_;
    std::map<std::string, std::vector<KeyChord>> overrides_;
};

// Chords a text field consumes itself while focused.
bool textFieldChord(const KeyChord& c);

}  // namespace mf::ui
