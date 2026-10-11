#pragma once

#include "core/curve.hpp"
#include "core/value.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mf {

using Id = uint64_t;

// Ids at or above this value belong to clips the project script derives.
constexpr Id kScriptIdBase = Id(1) << 40;

enum class PropMode { Constant, Curve, Expression };
const char* propModeName(PropMode m);
std::optional<PropMode> parsePropMode(std::string_view s);

// A property is a constant, a keyframed curve (in clip-local seconds), or a
// LuaJIT expression evaluated reactively.
struct Property {
    PropMode mode = PropMode::Constant;
    Value constant;
    Curve curve;
    std::string expr;

    static Property of(Value v) {
        Property p;
        p.constant = std::move(v);
        return p;
    }
    static Property expression(std::string e) {
        Property p;
        p.mode = PropMode::Expression;
        p.expr = std::move(e);
        return p;
    }
    bool operator==(const Property&) const = default;
};

using PropertyMap = std::map<std::string, Property>;

enum class MediaKind { Video, Audio, Image };
const char* mediaKindName(MediaKind k);

struct MediaSource {
    Id id = 0;
    std::string name;
    std::string path;
    MediaKind kind = MediaKind::Video;
    double duration = 0;
    int width = 0, height = 0;
    double fps = 0;
    bool hasVideo = false, hasAudio = false;
    int sampleRate = 0, channels = 0;
};

enum class TrackKind { Video, Audio };
const char* trackKindName(TrackKind k);

struct Track {
    Id id = 0;
    std::string name;
    TrackKind kind = TrackKind::Video;
    bool muted = false;
    bool hidden = false;
    bool locked = false;
};

enum class ClipKind { Media, Text, Solid, Generator, Synth };
const char* clipKindName(ClipKind k);
std::optional<ClipKind> parseClipKind(std::string_view s);

struct Effect {
    Id id = 0;
    std::string type;
    bool enabled = true;
    PropertyMap props;
};

struct Clip {
    Id id = 0;
    Id track = 0;
    ClipKind kind = ClipKind::Media;
    std::string name;
    Id media = 0;
    double start = 0;     // timeline seconds
    double duration = 1;  // timeline seconds
    double in = 0;        // source offset in seconds
    double speed = 1;
    Id link = 0;          // clips sharing a nonzero link move together
    PropertyMap props;
    std::vector<Effect> videoEffects;
    std::vector<Effect> audioEffects;

    double end() const { return start + duration; }
    bool scriptOwned() const { return id >= kScriptIdBase; }
};

// A transition blends the end of clip A into the start of clip B across the
// cut, centered on B's start. Its curve maps 0..1 progress to 0..1 mix.
struct Transition {
    Id id = 0;
    Id clipA = 0, clipB = 0;
    std::string type = "crossfade";
    double duration = 1;
    Curve curve = Curve::ramp("ease-in-out");
    PropertyMap props;
};

struct SignalDef {
    Id id = 0;
    std::string name;
    Property value;
};

struct ProjectSettings {
    int width = 1920, height = 1080;
    double fps = 30;
    int sampleRate = 48000;
    Color background = Color::gray(0.0f);
};

struct Project {
    std::string name = "untitled";
    ProjectSettings settings;
    std::vector<MediaSource> media;
    std::vector<Track> tracks;  // index 0 draws at the bottom
    std::vector<Clip> clips;
    std::vector<Transition> transitions;
    std::vector<SignalDef> signals;
    std::vector<Effect> masterAudio;
    std::string script;
    Id nextId = 1;

    Id newId() { return nextId++; }
    double duration() const;

    MediaSource* findMedia(Id id);
    const MediaSource* findMedia(Id id) const;
    const MediaSource* findMediaByName(std::string_view name) const;
    Track* findTrack(Id id);
    const Track* findTrack(Id id) const;
    const Track* findTrackByName(std::string_view name) const;
    int trackIndex(Id id) const;
    Clip* findClip(Id id);
    const Clip* findClip(Id id) const;
    Transition* findTransition(Id id);
    const Transition* findTransition(Id id) const;
    SignalDef* findSignal(std::string_view name);
    const SignalDef* findSignal(std::string_view name) const;
    // Finds an effect in any clip chain or the master chain.
    Effect* findEffect(Id id, Clip** owner = nullptr);

    static Project makeDefault();
};

}  // namespace mf
