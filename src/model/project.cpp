#include "model/project.hpp"

#include <algorithm>

namespace mf {

const char* propModeName(PropMode m) {
    switch (m) {
        case PropMode::Constant: return "constant";
        case PropMode::Curve: return "curve";
        case PropMode::Expression: return "expr";
    }
    return "constant";
}

std::optional<PropMode> parsePropMode(std::string_view s) {
    if (s == "constant") return PropMode::Constant;
    if (s == "curve") return PropMode::Curve;
    if (s == "expr" || s == "expression") return PropMode::Expression;
    return std::nullopt;
}

const char* mediaKindName(MediaKind k) {
    switch (k) {
        case MediaKind::Video: return "video";
        case MediaKind::Audio: return "audio";
        case MediaKind::Image: return "image";
    }
    return "video";
}

const char* trackKindName(TrackKind k) { return k == TrackKind::Video ? "video" : "audio"; }

const char* clipKindName(ClipKind k) {
    switch (k) {
        case ClipKind::Media: return "media";
        case ClipKind::Text: return "text";
        case ClipKind::Solid: return "solid";
        case ClipKind::Generator: return "generator";
        case ClipKind::Synth: return "synth";
    }
    return "media";
}

std::optional<ClipKind> parseClipKind(std::string_view s) {
    if (s == "media") return ClipKind::Media;
    if (s == "text") return ClipKind::Text;
    if (s == "solid") return ClipKind::Solid;
    if (s == "generator") return ClipKind::Generator;
    if (s == "synth") return ClipKind::Synth;
    return std::nullopt;
}

double Project::duration() const {
    double d = 0;
    for (const auto& c : clips) d = std::max(d, c.end());
    return d;
}

MediaSource* Project::findMedia(Id id) {
    for (auto& m : media) if (m.id == id) return &m;
    return nullptr;
}
const MediaSource* Project::findMedia(Id id) const { return const_cast<Project*>(this)->findMedia(id); }

const MediaSource* Project::findMediaByName(std::string_view n) const {
    for (const auto& m : media) if (m.name == n) return &m;
    return nullptr;
}

Track* Project::findTrack(Id id) {
    for (auto& t : tracks) if (t.id == id) return &t;
    return nullptr;
}
const Track* Project::findTrack(Id id) const { return const_cast<Project*>(this)->findTrack(id); }

const Track* Project::findTrackByName(std::string_view n) const {
    for (const auto& t : tracks) if (t.name == n) return &t;
    return nullptr;
}

int Project::trackIndex(Id id) const {
    for (size_t i = 0; i < tracks.size(); ++i) if (tracks[i].id == id) return int(i);
    return -1;
}

Clip* Project::findClip(Id id) {
    for (auto& c : clips) if (c.id == id) return &c;
    return nullptr;
}
const Clip* Project::findClip(Id id) const { return const_cast<Project*>(this)->findClip(id); }

Transition* Project::findTransition(Id id) {
    for (auto& t : transitions) if (t.id == id) return &t;
    return nullptr;
}
const Transition* Project::findTransition(Id id) const { return const_cast<Project*>(this)->findTransition(id); }

SignalDef* Project::findSignal(std::string_view n) {
    for (auto& s : signals) if (s.name == n) return &s;
    return nullptr;
}
const SignalDef* Project::findSignal(std::string_view n) const { return const_cast<Project*>(this)->findSignal(n); }

Effect* Project::findEffect(Id id, Clip** owner) {
    for (auto& c : clips) {
        for (auto& e : c.videoEffects) if (e.id == id) { if (owner) *owner = &c; return &e; }
        for (auto& e : c.audioEffects) if (e.id == id) { if (owner) *owner = &c; return &e; }
    }
    for (auto& e : masterAudio) if (e.id == id) { if (owner) *owner = nullptr; return &e; }
    return nullptr;
}

Project Project::makeDefault() {
    Project p;
    auto addTrack = [&](const char* name, TrackKind kind) {
        Track t;
        t.id = p.newId();
        t.name = name;
        t.kind = kind;
        p.tracks.push_back(t);
    };
    addTrack("A1", TrackKind::Audio);
    addTrack("A2", TrackKind::Audio);
    addTrack("V1", TrackKind::Video);
    addTrack("V2", TrackKind::Video);
    addTrack("V3", TrackKind::Video);
    p.script =
        "-- Project script. Runs again whenever it changes.\n"
        "-- mf.signal(name, fn) defines a reactive signal; read it as sig.name.\n"
        "-- Read time as t. See the meshframe skill for the full API.\n"
        "\n"
        "mf.signal(\"pulse\", function()\n"
        "  return 0.5 + 0.5 * math.sin(t * math.pi * 2)\n"
        "end)\n";
    return p;
}

}  // namespace mf
