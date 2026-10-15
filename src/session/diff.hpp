#pragma once

#include "model/project.hpp"

#include <nlohmann/json.hpp>

namespace mf {

// Compares two versions of a project. Returns {"added": [...], "removed":
// [...], "changed": [...]}. Added and removed entries have kind, id, and
// name; changed entries also have "fields", a map from a field path (such as
// "props.opacity" or "videoEffects[12].enabled") to {"old", "new"}. Project
// name, settings, and script changes have kind "project" and no id.
nlohmann::json diffProjects(const Project& before, const Project& after);

}  // namespace mf
