#include "model/serialize.hpp"

#include "model/schema.hpp"

#include <fmt/format.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace mf {

namespace {
constexpr int kFormatVersion = 1;

// Media paths are stored relative to the project file, so a project and its
// media can move together. The absolute path is stored too, so a project file
// moved without its media still finds them.
void storeMediaPath(json& jm, const std::filesystem::path& base) {
    std::string path = jm.value("path", "");
    if (path.empty() || !std::filesystem::path(path).is_absolute()) return;
    std::error_code ec;
    auto rel = std::filesystem::relative(path, base, ec);
    if (ec || rel.empty()) return;
    jm["path"] = rel.string();
    jm["absolutePath"] = path;
}

// Resolves a stored media path to an absolute one: the relative path if the
// file exists there, else the stored absolute path if that file exists.
std::string resolveMediaPath(const json& jm, const std::filesystem::path& base) {
    std::string path = jm.value("path", "");
    if (path.empty() || std::filesystem::path(path).is_absolute()) return path;
    auto resolved = (base / path).lexically_normal();
    std::error_code ec;
    if (std::filesystem::exists(resolved, ec)) return resolved.string();
    std::string fallback = jm.value("absolutePath", "");
    if (!fallback.empty() && std::filesystem::exists(fallback, ec)) return fallback;
    return resolved.string();
}

void coerceProps(PropertyMap& props, std::span<const PropSpec> specs) {
    for (auto& [name, prop] : props) {
        if (prop.mode != PropMode::Constant) continue;
        for (const auto& s : specs) {
            if (name != s.name) continue;
            try {
                prop.constant = coerce(s, prop.constant);
            } catch (const std::exception&) {
                prop.constant = s.def;
            }
        }
    }
    applyDefaults(props, specs);
}
}  // namespace

json valueToJson(const Value& v) {
    switch (typeOf(v)) {
        case ValueType::None: return nullptr;
        case ValueType::Number: return std::get<double>(v);
        case ValueType::Color: return formatColor(std::get<Color>(v));
        case ValueType::String: return std::get<std::string>(v);
    }
    return nullptr;
}

Value valueFromJson(const json& j) {
    if (j.is_number()) return j.get<double>();
    if (j.is_boolean()) return j.get<bool>() ? 1.0 : 0.0;
    if (j.is_string()) return j.get<std::string>();
    if (j.is_array() && (j.size() == 3 || j.size() == 4)) {
        Color c;
        c.r = j[0].get<float>();
        c.g = j[1].get<float>();
        c.b = j[2].get<float>();
        if (j.size() == 4) c.a = j[3].get<float>();
        return c;
    }
    return std::monostate{};
}

json curveToJson(const Curve& c) {
    json keys = json::array();
    for (const auto& k : c.keys) {
        json jk = {{"t", k.t}, {"v", k.v}, {"interp", interpName(k.interp)}};
        if (k.interp == Interp::Bezier) jk["bezier"] = {k.x1, k.y1, k.x2, k.y2};
        keys.push_back(jk);
    }
    return keys;
}

Curve curveFromJson(const json& j) {
    Curve c;
    if (!j.is_array()) throw std::runtime_error("curve must be an array of keys");
    for (const auto& jk : j) {
        Keyframe k;
        if (jk.is_array() && jk.size() >= 2) {
            k.t = jk[0].get<double>();
            k.v = jk[1].get<double>();
        } else {
            k.t = jk.at("t").get<double>();
            k.v = jk.at("v").get<double>();
            if (jk.contains("interp")) {
                auto i = parseInterp(jk["interp"].get<std::string>());
                if (!i) throw std::runtime_error("unknown interp " + jk["interp"].get<std::string>());
                k.interp = *i;
            }
            if (jk.contains("ease")) {
                const EasePreset* e = findEase(jk["ease"].get<std::string>());
                if (!e) throw std::runtime_error("unknown ease " + jk["ease"].get<std::string>());
                k.interp = Interp::Bezier;
                k.x1 = e->x1; k.y1 = e->y1; k.x2 = e->x2; k.y2 = e->y2;
            }
            if (jk.contains("bezier")) {
                const auto& b = jk["bezier"];
                k.x1 = b.at(0).get<float>();
                k.y1 = b.at(1).get<float>();
                k.x2 = b.at(2).get<float>();
                k.y2 = b.at(3).get<float>();
                k.interp = Interp::Bezier;
            }
        }
        c.keys.push_back(k);
    }
    c.sort();
    return c;
}

json propertyToJson(const Property& p) {
    switch (p.mode) {
        case PropMode::Constant: return valueToJson(p.constant);
        case PropMode::Curve: return {{"curve", curveToJson(p.curve)}};
        case PropMode::Expression: return {{"expr", p.expr}};
    }
    return nullptr;
}

Property propertyFromJson(const json& j) {
    Property p;
    if (j.is_object()) {
        if (j.contains("curve")) {
            p.mode = PropMode::Curve;
            p.curve = curveFromJson(j["curve"]);
        } else if (j.contains("expr")) {
            p.mode = PropMode::Expression;
            p.expr = j["expr"].get<std::string>();
        } else if (j.contains("value")) {
            p.constant = valueFromJson(j["value"]);
        } else {
            throw std::runtime_error("property object needs 'curve', 'expr', or 'value'");
        }
    } else {
        p.constant = valueFromJson(j);
    }
    return p;
}

namespace {
json propsToJson(const PropertyMap& props) {
    json o = json::object();
    for (const auto& [k, v] : props) o[k] = propertyToJson(v);
    return o;
}

PropertyMap propsFromJson(const json& j) {
    PropertyMap m;
    if (!j.is_object()) return m;
    for (auto it = j.begin(); it != j.end(); ++it) m[it.key()] = propertyFromJson(it.value());
    return m;
}

Effect effectFromJson(const json& j, bool audio) {
    Effect e;
    e.id = j.at("id").get<Id>();
    e.type = j.at("type").get<std::string>();
    e.enabled = j.value("enabled", true);
    e.props = propsFromJson(j.value("props", json::object()));
    const EffectSpec* s = audio ? findAudioEffect(e.type) : findVideoEffect(e.type);
    if (s) coerceProps(e.props, s->props);
    return e;
}
}  // namespace

json effectToJson(const Effect& e) {
    return {{"id", e.id}, {"type", e.type}, {"enabled", e.enabled}, {"props", propsToJson(e.props)}};
}

json clipToJson(const Clip& c) {
    json j = {{"id", c.id},       {"track", c.track},       {"kind", clipKindName(c.kind)},
              {"name", c.name},   {"start", c.start},       {"duration", c.duration},
              {"in", c.in},       {"speed", c.speed},       {"props", propsToJson(c.props)}};
    if (c.kind == ClipKind::Media) j["media"] = c.media;
    if (c.link) j["link"] = c.link;
    if (!c.videoEffects.empty()) {
        j["videoEffects"] = json::array();
        for (const auto& e : c.videoEffects) j["videoEffects"].push_back(effectToJson(e));
    }
    if (!c.audioEffects.empty()) {
        j["audioEffects"] = json::array();
        for (const auto& e : c.audioEffects) j["audioEffects"].push_back(effectToJson(e));
    }
    if (c.scriptOwned()) j["script"] = true;
    return j;
}

json transitionToJson(const Transition& t) {
    return {{"id", t.id},       {"clipA", t.clipA},       {"clipB", t.clipB},           {"type", t.type},
            {"duration", t.duration}, {"curve", curveToJson(t.curve)}, {"props", propsToJson(t.props)}};
}

json trackToJson(const Track& t) {
    return {{"id", t.id}, {"name", t.name}, {"kind", trackKindName(t.kind)},
            {"muted", t.muted}, {"hidden", t.hidden}, {"locked", t.locked}};
}

json mediaToJson(const MediaSource& m) {
    return {{"id", m.id},           {"name", m.name},         {"path", m.path},
            {"kind", mediaKindName(m.kind)}, {"duration", m.duration}, {"width", m.width},
            {"height", m.height},   {"fps", m.fps},           {"hasVideo", m.hasVideo},
            {"hasAudio", m.hasAudio}, {"sampleRate", m.sampleRate}, {"channels", m.channels}};
}

json settingsToJson(const ProjectSettings& s) {
    return {{"width", s.width}, {"height", s.height}, {"fps", s.fps}, {"sampleRate", s.sampleRate},
            {"background", formatColor(s.background)}};
}

ProjectSettings settingsFromJson(const json& j, ProjectSettings s) {
    s.width = j.value("width", s.width);
    s.height = j.value("height", s.height);
    s.fps = j.value("fps", s.fps);
    s.sampleRate = j.value("sampleRate", s.sampleRate);
    if (j.contains("background")) s.background = asColor(valueFromJson(j["background"]), s.background);
    return s;
}

json transcriptToJson(const Transcript& t) {
    // Spans are [text, start, end] to keep long transcripts compact.
    auto spans = [](const std::vector<Transcript::Span>& v) {
        json out = json::array();
        for (const auto& s : v) out.push_back({s.text, s.start, s.end});
        return out;
    };
    return {{"model", t.model}, {"segments", spans(t.segments)}, {"words", spans(t.words)}};
}

Transcript transcriptFromJson(const json& j) {
    auto spans = [](const json& v) {
        std::vector<Transcript::Span> out;
        for (const auto& s : v) out.push_back({s.at(0).get<std::string>(), s.at(1).get<double>(), s.at(2).get<double>()});
        return out;
    };
    return {j.value("model", ""), spans(j.value("segments", json::array())), spans(j.value("words", json::array()))};
}

json markerToJson(const Marker& m) {
    json j = {{"id", m.id}, {"name", m.name}, {"time", m.time}};
    if (!m.title.empty()) j["title"] = m.title;
    return j;
}

json projectToJson(const Project& p) {
    json j;
    j["format"] = "meshframe";
    j["version"] = kFormatVersion;
    j["name"] = p.name;
    j["settings"] = settingsToJson(p.settings);
    j["nextId"] = p.nextId;
    j["media"] = json::array();
    for (const auto& m : p.media) {
        json jm = mediaToJson(m);
        if (m.transcript) jm["transcript"] = transcriptToJson(*m.transcript);
        j["media"].push_back(std::move(jm));
    }
    j["tracks"] = json::array();
    for (const auto& t : p.tracks) j["tracks"].push_back(trackToJson(t));
    j["clips"] = json::array();
    for (const auto& c : p.clips) j["clips"].push_back(clipToJson(c));
    j["transitions"] = json::array();
    for (const auto& t : p.transitions) j["transitions"].push_back(transitionToJson(t));
    j["signals"] = json::array();
    for (const auto& s : p.signals) j["signals"].push_back({{"id", s.id}, {"name", s.name}, {"value", propertyToJson(s.value)}});
    j["markers"] = json::array();
    for (const auto& m : p.markers) j["markers"].push_back(markerToJson(m));
    j["masterAudio"] = json::array();
    for (const auto& e : p.masterAudio) j["masterAudio"].push_back(effectToJson(e));
    j["script"] = p.script;
    return j;
}

Project projectFromJson(const json& j) {
    if (j.value("format", "") != "meshframe") throw std::runtime_error("not a meshframe project");
    if (j.value("version", 0) > kFormatVersion) throw std::runtime_error("project was saved by a newer meshframe");
    Project p;
    p.name = j.value("name", "untitled");
    p.settings = settingsFromJson(j.value("settings", json::object()));
    p.nextId = j.value("nextId", Id(1));
    for (const auto& jm : j.value("media", json::array())) {
        MediaSource m;
        m.id = jm.at("id").get<Id>();
        m.name = jm.value("name", "");
        m.path = jm.value("path", "");
        std::string kind = jm.value("kind", "video");
        m.kind = kind == "audio" ? MediaKind::Audio : kind == "image" ? MediaKind::Image : MediaKind::Video;
        m.duration = jm.value("duration", 0.0);
        m.width = jm.value("width", 0);
        m.height = jm.value("height", 0);
        m.fps = jm.value("fps", 0.0);
        m.hasVideo = jm.value("hasVideo", false);
        m.hasAudio = jm.value("hasAudio", false);
        m.sampleRate = jm.value("sampleRate", 0);
        m.channels = jm.value("channels", 0);
        if (jm.contains("transcript")) m.transcript = std::make_shared<const Transcript>(transcriptFromJson(jm["transcript"]));
        p.media.push_back(m);
    }
    for (const auto& jt : j.value("tracks", json::array())) {
        Track t;
        t.id = jt.at("id").get<Id>();
        t.name = jt.value("name", "");
        t.kind = jt.value("kind", "video") == "audio" ? TrackKind::Audio : TrackKind::Video;
        t.muted = jt.value("muted", false);
        t.hidden = jt.value("hidden", false);
        t.locked = jt.value("locked", false);
        p.tracks.push_back(t);
    }
    for (const auto& jc : j.value("clips", json::array())) {
        Clip c;
        c.id = jc.at("id").get<Id>();
        c.track = jc.at("track").get<Id>();
        auto kind = parseClipKind(jc.value("kind", "media"));
        if (!kind) throw std::runtime_error("unknown clip kind");
        c.kind = *kind;
        c.name = jc.value("name", "");
        c.media = jc.value("media", Id(0));
        c.start = jc.value("start", 0.0);
        c.duration = jc.value("duration", 1.0);
        c.in = jc.value("in", 0.0);
        c.speed = jc.value("speed", 1.0);
        c.link = jc.value("link", Id(0));
        c.props = propsFromJson(jc.value("props", json::object()));
        for (const auto& je : jc.value("videoEffects", json::array())) c.videoEffects.push_back(effectFromJson(je, false));
        for (const auto& je : jc.value("audioEffects", json::array())) c.audioEffects.push_back(effectFromJson(je, true));
        const Track* t = p.findTrack(c.track);
        coerceProps(c.props, clipPropSpecs(c.kind, t ? t->kind : TrackKind::Video));
        p.clips.push_back(std::move(c));
    }
    for (const auto& jt : j.value("transitions", json::array())) {
        Transition t;
        t.id = jt.at("id").get<Id>();
        t.clipA = jt.at("clipA").get<Id>();
        t.clipB = jt.at("clipB").get<Id>();
        t.type = jt.value("type", "crossfade");
        t.duration = jt.value("duration", 1.0);
        if (jt.contains("curve")) t.curve = curveFromJson(jt["curve"]);
        t.props = propsFromJson(jt.value("props", json::object()));
        if (const EffectSpec* s = findTransition(t.type)) coerceProps(t.props, s->props);
        p.transitions.push_back(std::move(t));
    }
    for (const auto& js : j.value("signals", json::array())) {
        SignalDef s;
        s.id = js.value("id", Id(0));
        s.name = js.at("name").get<std::string>();
        s.value = propertyFromJson(js.at("value"));
        p.signals.push_back(std::move(s));
    }
    for (const auto& jm : j.value("markers", json::array()))
        p.markers.push_back({jm.at("id").get<Id>(), jm.at("name").get<std::string>(), jm.value("time", 0.0), jm.value("title", "")});
    std::ranges::stable_sort(p.markers, {}, &Marker::time);
    for (const auto& je : j.value("masterAudio", json::array())) p.masterAudio.push_back(effectFromJson(je, true));
    p.script = j.value("script", "");
    // Guard against hand-edited files with stale counters.
    Id maxId = 0;
    for (const auto& m : p.media) maxId = std::max(maxId, m.id);
    for (const auto& t : p.tracks) maxId = std::max(maxId, t.id);
    for (const auto& c : p.clips) {
        maxId = std::max({maxId, c.id, c.link});
        for (const auto& e : c.videoEffects) maxId = std::max(maxId, e.id);
        for (const auto& e : c.audioEffects) maxId = std::max(maxId, e.id);
    }
    for (const auto& t : p.transitions) maxId = std::max(maxId, t.id);
    for (const auto& s : p.signals) maxId = std::max(maxId, s.id);
    for (const auto& m : p.markers) maxId = std::max(maxId, m.id);
    for (const auto& e : p.masterAudio) maxId = std::max(maxId, e.id);
    p.nextId = std::max(p.nextId, maxId + 1);
    return p;
}

void saveProject(const Project& p, const std::string& path) {
    json j = projectToJson(p);
    auto base = std::filesystem::absolute(path).parent_path();
    for (auto& m : j["media"]) storeMediaPath(m, base);
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + path);
        out << j.dump(2) << '\n';
        if (!out) throw std::runtime_error("cannot write " + path);
    }
    std::filesystem::rename(tmp, path);
}

Project loadProject(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    json j;
    try {
        j = json::parse(ss.str());
    } catch (const json::parse_error& e) {
        throw std::runtime_error(fmt::format("{}: invalid JSON: {}", path, e.what()));
    }
    auto base = std::filesystem::absolute(path).parent_path();
    if (j.contains("media") && j["media"].is_array())
        for (auto& m : j["media"]) m["path"] = resolveMediaPath(m, base);
    return projectFromJson(j);
}

}  // namespace mf
