#pragma once

#include "ui/theme.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace mf::ui {

// User settings, saved as JSON in the SDL pref path. Unknown or invalid
// entries fall back to defaults so an old or hand-edited file still loads.
struct Settings {
    Theme theme;
    nlohmann::json bindings = nlohmann::json::object();  // Actions::overridesJson
    std::string uiFont;    // empty: system sans
    std::string monoFont;  // empty: system monospace
    float uiSize = 13;
    float monoSize = 13;
    float uiScale = 1;     // multiplies the display scale
    nlohmann::json workspace;  // spaces, layouts, and pane views

    nlohmann::json toJson() const;
    static Settings fromJson(const nlohmann::json& j);

    // MESHFRAME_SETTINGS overrides the default path.
    static std::string defaultPath();
    static Settings load(const std::string& path, std::string* err = nullptr);
    bool save(const std::string& path, std::string* err = nullptr) const;
};

}  // namespace mf::ui
