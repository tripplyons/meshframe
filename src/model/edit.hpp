#pragma once

#include "model/project.hpp"
#include "model/schema.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mf {

struct EditError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Validated mutations of a Project. Every function either applies the whole
// change or throws EditError and leaves the project unchanged. GUI, CLI, and
// scripts share these rules.
namespace edit {

Id addTrack(Project& p, TrackKind kind, std::string name = {});
void removeTrack(Project& p, Id track);
struct TrackFlags {
    std::optional<std::string> name;
    std::optional<bool> muted, hidden, locked;
};
void setTrack(Project& p, Id track, const TrackFlags& flags);
void moveTrack(Project& p, Id track, int newIndex);

Id addMedia(Project& p, MediaSource info);
void removeMedia(Project& p, Id media);

// Adds a clip. Fills default properties and an id; `c.track` must exist and
// accept the clip kind.
Id addClip(Project& p, Clip c);
// Places a media source at `start`. Sources with video and audio get a linked
// pair of clips. Tracks default to the first track of the needed kind.
std::vector<Id> addMediaClips(Project& p, Id media, double start, Id videoTrack = 0, Id audioTrack = 0);
void moveClip(Project& p, Id clip, double start, std::optional<Id> track = std::nullopt, bool withLinked = true);
// Sets new timeline bounds. Moving the start also moves the source offset.
void trimClip(Project& p, Id clip, double start, double duration, bool withLinked = true);
// Splits at timeline time t. Returns ids of the new right-hand clips.
std::vector<Id> splitClip(Project& p, Id clip, double t, bool withLinked = true);
void removeClip(Project& p, Id clip, bool withLinked = true);
void renameClip(Project& p, Id clip, std::string name);
void setClipSpeed(Project& p, Id clip, double speed);
void unlinkClip(Project& p, Id clip);

// Properties live on clips, effects, and transitions. The target id names one.
PropertyMap& propsOf(Project& p, Id target);
std::vector<PropSpec> specsOf(const Project& p, Id target);
void setProp(Project& p, Id target, const std::string& name, Property prop);
void setPropConstant(Project& p, Id target, const std::string& name, const Value& v);
void setPropExpr(Project& p, Id target, const std::string& name, std::string expr);
// Adds or replaces a key (clip-local time). Converts a constant to a curve.
void setKeyframe(Project& p, Id target, const std::string& name, double t, double v,
                 std::optional<Interp> interp = std::nullopt);
void removeKeyframe(Project& p, Id target, const std::string& name, double t);
void setKeyframeEase(Project& p, Id target, const std::string& name, size_t index, Interp interp,
                     float x1, float y1, float x2, float y2);

// `clip` 0 means the master audio chain.
Id addEffect(Project& p, Id clip, bool audio, const std::string& type, int index = -1);
void removeEffect(Project& p, Id effect);
void moveEffect(Project& p, Id effect, int delta);
void setEffectEnabled(Project& p, Id effect, bool enabled);

Id addTransition(Project& p, Id clipA, Id clipB, const std::string& type, double duration);
void removeTransition(Project& p, Id transition);
void setTransition(Project& p, Id transition, std::optional<std::string> type, std::optional<double> duration);
void setTransitionCurve(Project& p, Id transition, Curve curve);

void setSignal(Project& p, const std::string& name, Property value);
void removeSignal(Project& p, const std::string& name);
void setScript(Project& p, std::string source);
void setSettings(Project& p, const ProjectSettings& s);

}  // namespace edit
}  // namespace mf
