#include "model/path_edit.hpp"

namespace mf::edit {
std::vector<Id> addImportedPaths(Project& project, const std::vector<ImportedPath>& paths,
                                 Id track, double start, double duration, const std::string& name) {
    if (paths.empty()) throw EditError("file contains no paths");
    const Track* destination = project.findTrack(track);
    if (destination && destination->locked) throw EditError("destination track is locked");
    Project next = project;
    std::vector<Id> ids;
    for (size_t i = 0; i < paths.size(); ++i) {
        const ImportedPath& path = paths[i];
        try { (void)parsePath(path.d); }
        catch (const std::exception& e) { throw EditError(e.what()); }
        Clip clip;
        clip.kind = ClipKind::Path;
        clip.track = track;
        clip.start = start;
        clip.duration = duration;
        clip.name = name.empty() ? path.name : name + (paths.size() > 1 ? " " + std::to_string(i + 1) : "");
        clip.props = {{"shape", Property::of(std::string("path"))}, {"path", Property::of(path.d)},
                      {"fill", Property::of(path.fill)}, {"stroke", Property::of(path.stroke)},
                      {"strokeWidth", Property::of(path.strokeWidth)}, {"fillRule", Property::of(path.fillRule)},
                      {"strokeLinecap", Property::of(path.linecap)}, {"strokeLinejoin", Property::of(path.linejoin)},
                      {"strokeMiterLimit", Property::of(path.miterLimit)}};
        ids.push_back(addClip(next, std::move(clip)));
    }
    project = std::move(next);
    return ids;
}
}
