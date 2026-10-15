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
// Keeps timeline bounds and source offsets, and updates linked clips by default.
void setClipSpeed(Project& p, Id clip, double speed, bool withLinked = true);
void unlinkClip(Project& p, Id clip);

// Ripple edits shift every clip that starts at or after the edit point, on
// all tracks, and the markers there too. A clip that starts before the point
// stays, even when it runs past it. Shifting a clip on a locked track fails.
//
// Removes the clip (and its linked clips) and pulls later clips left by the
// time it covered. Fails when another clip starts inside that time.
void rippleRemoveClip(Project& p, Id clip, bool withLinked = true);
// Makes room for clips just added at one start: later clips move right by
// their length. A clip that spans the start on the same track is split first.
void rippleInsert(Project& p, const std::vector<Id>& clips);
// Trims like trimClip, then shifts the clips after the old end by the
// change in end time.
void rippleTrimClip(Project& p, Id clip, double start, double duration, bool withLinked = true);
// Removes times between start and end where no track has a clip. Returns
// the removed ranges as [start, end) pairs in the original timing.
std::vector<std::pair<double, double>> closeGaps(Project& p, double start, double end);

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
void setTranscript(Project& p, Id media, std::shared_ptr<const Transcript> transcript);

// Marker names are Lua identifiers, so expressions can read marker.NAME.
// An empty name picks the next free M1, M2, ...
Id addMarker(Project& p, std::string name, double time);
void moveMarker(Project& p, Id marker, double time);
void renameMarker(Project& p, Id marker, const std::string& name);
void removeMarker(Project& p, Id marker);

void setScript(Project& p, std::string source);
void setSettings(Project& p, const ProjectSettings& s);

}  // namespace edit
}  // namespace mf
