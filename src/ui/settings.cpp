#include "ui/settings.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace mf::ui {

nlohmann::json Settings::toJson() const {
    nlohmann::json j = {
        {"format", "meshframe-settings"},
        {"version", 1},
        {"theme", theme.toJson()},
        {"bindings", bindings},
        {"fonts", {{"ui", uiFont}, {"mono", monoFont}, {"uiSize", uiSize}, {"monoSize", monoSize}, {"scale", uiScale}}},
    };
    if (!workspace.is_null()) j["workspace"] = workspace;
    return j;
}

Settings Settings::fromJson(const nlohmann::json& j) {
    Settings s;
    if (!j.is_object()) return s;
    if (j.contains("theme")) s.theme = Theme::fromJson(j["theme"]);
    if (j.contains("bindings") && j["bindings"].is_object()) s.bindings = j["bindings"];
    if (auto f = j.find("fonts"); f != j.end() && f->is_object()) {
        auto str = [&](const char* k) { return f->contains(k) && (*f)[k].is_string() ? (*f)[k].get<std::string>() : std::string(); };
        auto num = [&](const char* k, float def, float lo, float hi) {
            return f->contains(k) && (*f)[k].is_number() ? std::clamp((*f)[k].get<float>(), lo, hi) : def;
        };
        s.uiFont = str("ui");
        s.monoFont = str("mono");
        s.uiSize = num("uiSize", 13, 8, 32);
        s.monoSize = num("monoSize", 13, 8, 32);
        s.uiScale = num("scale", 1, 0.5f, 3);
    }
    if (j.contains("workspace")) s.workspace = j["workspace"];
    return s;
}

std::string Settings::defaultPath() {
    if (const char* env = std::getenv("MESHFRAME_SETTINGS"); env && *env) return env;
    char* pref = SDL_GetPrefPath("meshframe", "meshframe");
    if (!pref) return "meshframe-settings.json";
    std::string path = std::string(pref) + "settings.json";
    SDL_free(pref);
    return path;
}

Settings Settings::load(const std::string& path, std::string* err) {
    std::ifstream in(path);
    if (!in) return {};
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded()) {
        if (err) *err = "settings file is not valid JSON: " + path;
        return {};
    }
    return fromJson(j);
}

bool Settings::save(const std::string& path, std::string* err) const {
    std::error_code ec;
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) {
            if (err) *err = "cannot write " + path;
            return false;
        }
        out << toJson().dump(2) << '\n';
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        if (err) *err = ec.message();
        return false;
    }
    return true;
}

}  // namespace mf::ui
