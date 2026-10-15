#pragma once

#include "model/edit.hpp"
#include "render/path.hpp"

namespace mf::edit {
// Embeds imported geometry and styles as clips, in SVG paint order. The change
// is atomic, including invalid timing, track, and property failures.
std::vector<Id> addImportedPaths(Project& project, const std::vector<ImportedPath>& paths,
                                 Id track, double start, double duration, const std::string& name = {});
}
