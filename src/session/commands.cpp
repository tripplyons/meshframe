#include "session/commands.hpp"

#include "model/edit.hpp"
#include "model/schema.hpp"
#include "render/image.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace mf {

namespace {

namespace fs = std::filesystem;

std::optional<double> parseNumber(std::string_view s) {
    double d = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), d);
    if (ec != std::errc() || p != s.data() + s.size() || s.empty()) return std::nullopt;
    return d;
}

std::optional<Id> idOf(const json& v) {
    if (v.is_number_unsigned() || v.is_number_integer()) return v.get<Id>();
    if (v.is_number_float()) return Id(v.get<double>());
    if (v.is_string()) {
        const auto& s = v.get_ref<const std::string&>();
        if (s.starts_with('#')) return idOf(json(s.substr(1)));
        Id id = 0;
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), id);
        if (ec == std::errc() && p == s.data() + s.size() && !s.empty()) return id;
    }
    return std::nullopt;
}

std::string text(const json& v) { return v.is_string() ? v.get<std::string>() : v.dump(); }

const Effect* findEffectIn(const Project& p, Id id, const Clip** owner = nullptr) {
    Clip* c = nullptr;
    const Effect* e = const_cast<Project&>(p).findEffect(id, &c);
    if (owner) *owner = c;
    return e;
}

std::string usage(const CommandSpec& spec) {
    std::string u = spec.name;
    for (const auto& a : spec.args) u += a.required ? fmt::format(" <{}>", a.name) : fmt::format(" [{}]", a.name);
    return u;
}

json specToJson(const CommandSpec& spec) {
    json args = json::array();
    for (const auto& a : spec.args)
        args.push_back({{"name", a.name}, {"type", a.type}, {"required", a.required}, {"help", a.help}});
    return {{"name", spec.name}, {"summary", spec.summary}, {"usage", usage(spec)}, {"args", args}};
}

json coerceArg(const ArgSpec& a, const json& v, const std::string& cwd) {
    auto fail = [&]() -> json { throw EditError(fmt::format("argument {}: expected {}, got {}", a.name, a.type, text(v))); };
    const std::string& t = a.type;
    if (t == "number") {
        if (v.is_number()) return v;
        if (v.is_string())
            if (auto d = parseNumber(v.get<std::string>())) return *d;
        return fail();
    }
    if (t == "int") {
        if (v.is_number()) return int64_t(v.get<double>());
        if (v.is_string())
            if (auto d = parseNumber(v.get<std::string>()); d && *d == double(int64_t(*d))) return int64_t(*d);
        return fail();
    }
    if (t == "bool") {
        if (v.is_boolean()) return v;
        if (v.is_number()) return v.get<double>() != 0;
        if (v.is_string()) {
            std::string s = v.get<std::string>();
            if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
            if (s == "false" || s == "0" || s == "no" || s == "off") return false;
        }
        return fail();
    }
    if (t == "string") return v.is_string() ? v : json(v.dump());
    if (t == "path") {
        if (!v.is_string()) return fail();
        fs::path p = v.get<std::string>();
        if (p.is_relative() && !cwd.empty()) p = fs::path(cwd) / p;
        return p.lexically_normal().string();
    }
    if (t == "value" || t == "json") {
        if (!v.is_string()) return v;
        json parsed = json::parse(v.get<std::string>(), nullptr, false);
        if (!parsed.is_discarded()) return parsed;
        if (t == "json") return fail();
        return v;
    }
    if (t == "id") {
        if (auto id = idOf(v)) return *id;
        if (v.is_string()) return v;
        return fail();
    }
    return v;
}

}  // namespace

// ---- id resolution ----------------------------------------------------------

Id resolveClip(const Project& p, const json& v) {
    if (auto id = idOf(v)) {
        if (p.findClip(*id)) return *id;
        throw EditError(fmt::format("no clip {}", *id));
    }
    std::string name = text(v);
    Id found = 0;
    for (const auto& c : p.clips) {
        if (c.name != name) continue;
        if (found) throw EditError(fmt::format("clip name '{}' is ambiguous; use an id", name));
        found = c.id;
    }
    if (!found) throw EditError(fmt::format("no clip named '{}'", name));
    return found;
}

Id resolveTrack(const Project& p, const json& v) {
    if (auto id = idOf(v)) {
        if (p.findTrack(*id)) return *id;
        throw EditError(fmt::format("no track {}", *id));
    }
    if (const Track* t = p.findTrackByName(text(v))) return t->id;
    throw EditError(fmt::format("no track named '{}'", text(v)));
}

Id resolveMedia(const Project& p, const json& v) {
    if (auto id = idOf(v)) {
        if (p.findMedia(*id)) return *id;
        throw EditError(fmt::format("no media {}", *id));
    }
    std::string name = text(v);
    if (const MediaSource* m = p.findMediaByName(name)) return m->id;
    for (const auto& m : p.media)
        if (m.path == name || fs::path(m.path).filename() == name) return m.id;
    throw EditError(fmt::format("no media named '{}'", name));
}

Id resolveTarget(const Project& p, const json& v) {
    if (auto id = idOf(v)) {
        if (p.findClip(*id) || p.findTransition(*id) || findEffectIn(p, *id)) return *id;
        throw EditError(fmt::format("no clip, effect, or transition {}", *id));
    }
    return resolveClip(p, v);
}

// ---- dispatch -----------------------------------------------------------------

Commands::Commands(Session& s) : session_(s) { registerBuiltins(); }

void Commands::add(CommandSpec spec) {
    std::string name = spec.name;
    commands_[name] = std::move(spec);
}

json Commands::requestFromWords(const std::vector<std::string>& words, const std::string& cwd) {
    json r = {{"cmd", words.empty() ? "" : words[0]}, {"argv", json::array()}, {"cwd", cwd}};
    for (size_t i = 1; i < words.size(); ++i) r["argv"].push_back(words[i]);
    return r;
}

std::vector<std::string> Commands::splitWords(std::string_view line) {
    std::vector<std::string> out;
    std::string cur;
    bool inWord = false;
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\\' && i + 1 < line.size() && quote != '\'') {
            cur += line[++i];
            inWord = true;
        } else if (quote) {
            if (c == quote) quote = 0;
            else cur += c;
        } else if (c == '"' || c == '\'') {
            quote = c;
            inWord = true;
        } else if (std::isspace(uint8_t(c))) {
            if (inWord) out.push_back(std::move(cur)), cur.clear(), inWord = false;
        } else {
            cur += c;
            inWord = true;
        }
    }
    if (inWord) out.push_back(std::move(cur));
    return out;
}

json Commands::normalize(const CommandSpec& spec, const json& request) const {
    std::string cwd = request.value("cwd", "");
    json raw = json::object();
    if (request.contains("args")) {
        if (!request["args"].is_object()) throw EditError("args must be an object");
        raw = request["args"];
    }
    if (request.contains("argv")) {
        size_t next = 0;
        for (const auto& w : request["argv"]) {
            std::string word = text(w);
            // key=value fills a named argument when the key is declared.
            if (auto eq = word.find('='); eq != std::string::npos && eq > 0) {
                std::string key = word.substr(0, eq);
                if (std::ranges::any_of(spec.args, [&](const ArgSpec& a) { return a.name == key; })) {
                    raw[key] = word.substr(eq + 1);
                    continue;
                }
            }
            while (next < spec.args.size() && raw.contains(spec.args[next].name)) ++next;
            if (next >= spec.args.size())
                throw EditError(fmt::format("too many arguments; usage: {}", usage(spec)));
            raw[spec.args[next++].name] = w;
        }
    }
    json args = json::object();
    for (auto it = raw.begin(); it != raw.end(); ++it) {
        auto a = std::ranges::find(spec.args, it.key(), &ArgSpec::name);
        if (a == spec.args.end()) throw EditError(fmt::format("unknown argument '{}'; usage: {}", it.key(), usage(spec)));
        args[it.key()] = coerceArg(*a, it.value(), cwd);
    }
    for (const auto& a : spec.args)
        if (a.required && !args.contains(a.name))
            throw EditError(fmt::format("missing argument '{}'; usage: {}", a.name, usage(spec)));
    return args;
}

void Commands::run(const json& request, Reply reply) {
    // A request "id" is copied to its reply so clients can match them.
    if (request.is_object() && request.contains("id") && !request["id"].is_null()) {
        reply = [inner = std::move(reply), id = request["id"]](json r) {
            r["id"] = id;
            inner(std::move(r));
        };
    }
    auto fail = [&](const std::string& msg) { reply({{"ok", false}, {"error", msg}}); };
    if (!request.is_object() || !request.contains("cmd") || !request["cmd"].is_string()) {
        fail("request needs a string 'cmd'");
        return;
    }
    std::string name = request["cmd"];
    auto it = commands_.find(name);
    if (it == commands_.end()) {
        fail(fmt::format("unknown command '{}'; run 'help' for the list", name));
        return;
    }
    CommandContext ctx(session_, [reply](json result) {
        if (result.is_object() && result.contains("error") && result.size() == 1)
            reply({{"ok", false}, {"error", result["error"]}});
        else
            reply({{"ok", true}, {"result", std::move(result)}});
    });
    try {
        json args = normalize(it->second, request);
        json result = it->second.fn(args, ctx);
        if (!ctx.deferred()) reply({{"ok", true}, {"result", std::move(result)}});
    } catch (const std::exception& e) {
        if (ctx.deferred()) session_.log(LogLine::Level::Error, fmt::format("{}: {}", name, e.what()));
        fail(e.what());
    }
}

json Commands::runSync(const json& request) {
    std::optional<json> out;
    run(request, [&](json r) { out = std::move(r); });
    while (!out) {
        session_.tick();
        if (!out) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return *out;
}

// ---- builtin commands -----------------------------------------------------------

namespace {

ArgSpec req(std::string name, std::string type, std::string help = {}) { return {std::move(name), std::move(type), true, std::move(help)}; }
ArgSpec opt(std::string name, std::string type, std::string help = {}) { return {std::move(name), std::move(type), false, std::move(help)}; }

template <class T>
T get(const json& args, const char* name, T fallback) {
    return args.contains(name) ? args[name].get<T>() : fallback;
}

json summaryJson(Session& s) {
    const Project& p = s.project();
    json tracks = json::array();
    for (auto it = p.tracks.rbegin(); it != p.tracks.rend(); ++it) {
        json t = trackToJson(*it);
        json clips = json::array();
        std::vector<const Clip*> cs;
        for (const auto& c : p.clips)
            if (c.track == it->id) cs.push_back(&c);
        std::ranges::sort(cs, {}, &Clip::start);
        for (const Clip* c : cs) {
            json cj = {{"id", c->id}, {"kind", clipKindName(c->kind)}, {"name", c->name},
                       {"start", c->start}, {"end", c->end()}};
            if (c->link) cj["link"] = c->link;
            size_t fx = c->videoEffects.size() + c->audioEffects.size();
            if (fx) cj["effects"] = fx;
            clips.push_back(cj);
        }
        t["clips"] = clips;
        tracks.push_back(t);
    }
    return tracks;
}

json selectionJson(const Selection& sel) {
    return {{"clips", sel.clips}, {"effect", sel.effect}, {"transition", sel.transition},
            {"track", sel.track}, {"media", sel.media},   {"prop", sel.prop}};
}

// Errors after evaluating the frame at time t, so expression errors on
// visible clips are current.
json errorsJson(Session& s, double t) {
    Evaluator& ev = s.evaluator();
    ev.setTime(t);
    ev.refreshOutputs();
    json out = json::array();
    for (const auto& e : ev.errors()) {
        json j = {{"node", e.key}, {"message", e.message}};
        if (e.line) j["line"] = e.line;
        out.push_back(j);
    }
    return out;
}

json exportStatus(Session& s) {
    auto& job = s.exportJob();
    json j = {{"running", job.running()}, {"progress", job.progress()}, {"finished", job.finished()},
              {"path", job.options().path}};
    if (job.finished() && !job.error().empty()) j["error"] = job.error();
    return j;
}

// Evaluated value of a property at time t, using the effective project.
Value evaluatedProp(Session& s, Id target, const std::string& name, double t) {
    Evaluator& ev = s.evaluator();
    ev.setTime(t);
    const Project& ep = ev.project();
    if (const Clip* c = ep.findClip(target)) return ev.prop(*c, name);
    if (const Transition* tr = ep.findTransition(target)) return ev.transitionParams(*tr)(name);
    const Clip* owner = nullptr;
    if (const Effect* e = findEffectIn(ep, target, &owner)) {
        bool audio = owner ? std::ranges::any_of(owner->audioEffects, [&](const Effect& x) { return x.id == target; }) : true;
        return ev.effectParams(*e, audio)(name);
    }
    return {};
}

// The session time if the target is active then, otherwise the start of the
// span where it is: its clip's, or its transition's window.
double activeTime(Session& s, Id target) {
    const Project& ep = s.evaluator().project();
    double t = s.time();
    const Clip* owner = ep.findClip(target);
    if (!owner) findEffectIn(ep, target, &owner);
    if (owner) return t >= owner->start && t < owner->end() ? t : owner->start;
    if (const Transition* tr = ep.findTransition(target)) {
        auto [ws, we] = Evaluator::window(ep, *tr);
        return t >= ws && t < we ? t : ws;
    }
    return t;
}

const Property& propOf(const Project& p, Id target, const std::string& name) {
    const auto& props = edit::propsOf(const_cast<Project&>(p), target);
    auto it = props.find(name);
    if (it == props.end()) throw EditError(fmt::format("no property '{}'", name));
    return it->second;
}

Curve curveArg(const json& v) {
    if (v.is_string()) {
        std::string name = v;
        if (!findEase(name) && !parseInterp(name)) throw EditError(fmt::format("unknown ease '{}'", name));
        return Curve::ramp(name);
    }
    return curveFromJson(v);
}

}  // namespace

void Commands::registerBuiltins() {
    using A = const json&;
    using C = CommandContext&;
    auto edit = [](Session& s, const std::string& label, const std::function<void(Project&)>& fn) { s.apply(label, fn); };

    // ---- project ----
    add({"help", "List commands, or describe one", {opt("command", "string")}, [this](A a, C) -> json {
             if (a.contains("command")) {
                 auto it = commands_.find(a["command"]);
                 if (it == commands_.end()) throw EditError(fmt::format("unknown command '{}'", text(a["command"])));
                 return specToJson(it->second);
             }
             json out = json::array();
             for (const auto& [name, spec] : commands_) out.push_back({{"name", name}, {"usage", usage(spec)}, {"summary", spec.summary}});
             return out;
         }});
    add({"status", "Session summary: file, time, size, counts, errors, export", {}, [](A, C c) -> json {
             Session& s = c.session;
             const Project& p = s.project();
             Evaluator& ev = s.evaluator();
             return {{"path", s.path()},
                     {"dirty", s.dirty()},
                     {"revision", s.revision()},
                     {"name", p.name},
                     {"time", s.time()},
                     {"playing", s.player().playing()},
                     {"duration", ev.project().duration()},
                     {"settings", settingsToJson(p.settings)},
                     {"tracks", p.tracks.size()},
                     {"clips", p.clips.size()},
                     {"scriptClips", ev.project().clips.size() - p.clips.size()},
                     {"media", p.media.size()},
                     {"transitions", p.transitions.size()},
                     {"errors", errorsJson(s, s.time())},
                     {"undo", s.history().undoLabels().empty() ? json() : json(s.history().undoLabels().back())},
                     {"export", exportStatus(s)}};
         }});
    add({"show", "Show part of the project as JSON",
         {opt("what", "string", "summary (default), project, tracks, clips, media, transitions, signals, master, settings, script, selection")},
         [](A a, C c) -> json {
             Session& s = c.session;
             const Project& p = s.project();
             std::string what = get<std::string>(a, "what", "summary");
             auto list = [](const auto& items, auto fn) {
                 json out = json::array();
                 for (const auto& x : items) out.push_back(fn(x));
                 return out;
             };
             if (what == "summary") return summaryJson(s);
             if (what == "project") return projectToJson(p);
             if (what == "tracks") return list(p.tracks, trackToJson);
             if (what == "clips") return list(s.evaluator().project().clips, clipToJson);
             if (what == "media") return list(p.media, mediaToJson);
             if (what == "transitions") return list(p.transitions, transitionToJson);
             if (what == "signals")
                 return list(p.signals, [](const SignalDef& d) { return json{{"id", d.id}, {"name", d.name}, {"value", propertyToJson(d.value)}}; });
             if (what == "master") return list(p.masterAudio, effectToJson);
             if (what == "settings") return settingsToJson(p.settings);
             if (what == "script") return p.script;
             if (what == "selection") return selectionJson(s.selection);
             throw EditError(fmt::format("unknown section '{}'", what));
         }});
    add({"new", "Replace the session with a new default project", {}, [](A, C c) -> json {
             c.session.newProject();
             return nullptr;
         }});
    add({"open", "Open a project file", {req("path", "path")}, [](A a, C c) -> json {
             std::string err;
             if (!c.session.open(a["path"], &err)) throw EditError(err);
             return c.session.path();
         }});
    add({"save", "Save the project (to path, or its current file)", {opt("path", "path")}, [](A a, C c) -> json {
             std::string err;
             if (!c.session.save(get<std::string>(a, "path", ""), &err)) throw EditError(err);
             return c.session.path();
         }});
    add({"settings", "Show or change project settings",
         {opt("width", "int"), opt("height", "int"), opt("fps", "number"), opt("sampleRate", "int"), opt("background", "value")},
         [edit](A a, C c) -> json {
             if (!a.empty()) {
                 json patch = a;
                 ProjectSettings next = settingsFromJson(patch, c.session.project().settings);
                 edit(c.session, "settings", [&](Project& p) { edit::setSettings(p, next); });
             }
             return settingsToJson(c.session.project().settings);
         }});

    // ---- media and tracks ----
    add({"media.import", "Probe a file and add it to the media pool", {req("path", "path")}, [](A a, C c) -> json {
             std::string err;
             Id id = c.session.importMedia(a["path"], &err);
             if (!id) throw EditError(err);
             return mediaToJson(*c.session.project().findMedia(id));
         }});
    add({"media.list", "List media sources", {}, [](A, C c) -> json {
             json out = json::array();
             for (const auto& m : c.session.project().media) {
                 json j = mediaToJson(m);
                 std::string err = c.session.cache().error(m.path);
                 if (!err.empty()) j["error"] = err;
                 out.push_back(j);
             }
             return out;
         }});
    add({"media.remove", "Remove a media source and its clips", {req("media", "id")}, [edit](A a, C c) -> json {
             Id id = resolveMedia(c.session.project(), a["media"]);
             edit(c.session, "remove media", [&](Project& p) { edit::removeMedia(p, id); });
             return nullptr;
         }});
    add({"track.add", "Add a track", {req("kind", "string", "video or audio"), opt("name", "string")}, [edit](A a, C c) -> json {
             std::string kind = a["kind"];
             if (kind != "video" && kind != "audio") throw EditError("track kind must be video or audio");
             Id id = 0;
             edit(c.session, "add track", [&](Project& p) {
                 id = edit::addTrack(p, kind == "audio" ? TrackKind::Audio : TrackKind::Video, get<std::string>(a, "name", ""));
             });
             return trackToJson(*c.session.project().findTrack(id));
         }});
    add({"track.remove", "Remove a track and its clips", {req("track", "id")}, [edit](A a, C c) -> json {
             Id id = resolveTrack(c.session.project(), a["track"]);
             edit(c.session, "remove track", [&](Project& p) { edit::removeTrack(p, id); });
             return nullptr;
         }});
    add({"track.set", "Rename, mute, hide, or lock a track",
         {req("track", "id"), opt("name", "string"), opt("muted", "bool"), opt("hidden", "bool"), opt("locked", "bool")},
         [edit](A a, C c) -> json {
             Id id = resolveTrack(c.session.project(), a["track"]);
             edit::TrackFlags f;
             if (a.contains("name")) f.name = a["name"].get<std::string>();
             if (a.contains("muted")) f.muted = a["muted"].get<bool>();
             if (a.contains("hidden")) f.hidden = a["hidden"].get<bool>();
             if (a.contains("locked")) f.locked = a["locked"].get<bool>();
             edit(c.session, "track settings", [&](Project& p) { edit::setTrack(p, id, f); });
             return trackToJson(*c.session.project().findTrack(id));
         }});
    add({"track.move", "Move a track to an index (0 = bottom)", {req("track", "id"), req("index", "int")}, [edit](A a, C c) -> json {
             Id id = resolveTrack(c.session.project(), a["track"]);
             edit(c.session, "move track", [&](Project& p) { edit::moveTrack(p, id, a["index"].get<int>()); });
             return nullptr;
         }});

    // ---- clips ----
    add({"clip.add", "Add a text, solid, generator, or synth clip",
         {req("kind", "string", "text, solid, generator, synth"), req("track", "id"), req("start", "number"),
          req("duration", "number"), opt("name", "string"), opt("props", "json", "object of property name to value")},
         [edit](A a, C c) -> json {
             auto kind = parseClipKind(a["kind"].get<std::string>());
             if (!kind || *kind == ClipKind::Media) throw EditError("kind must be text, solid, generator, or synth; use clip.media for media");
             Clip clip;
             clip.kind = *kind;
             clip.track = resolveTrack(c.session.project(), a["track"]);
             clip.start = a["start"];
             clip.duration = a["duration"];
             clip.name = get<std::string>(a, "name", "");
             json props = get<json>(a, "props", json::object());
             if (!props.is_object()) throw EditError("props must be an object");
             Id id = 0;
             edit(c.session, "add clip", [&](Project& p) {
                 id = edit::addClip(p, clip);
                 for (auto it = props.begin(); it != props.end(); ++it) edit::setProp(p, id, it.key(), propertyFromJson(it.value()));
             });
             return clipToJson(*c.session.project().findClip(id));
         }});
    add({"clip.media", "Place a media source on the timeline (linked A/V pair when it has both)",
         {req("media", "id"), opt("start", "number"), opt("track", "id", "video track"), opt("audioTrack", "id")},
         [edit](A a, C c) -> json {
             const Project& cur = c.session.project();
             Id media = resolveMedia(cur, a["media"]);
             Id vt = a.contains("track") ? resolveTrack(cur, a["track"]) : 0;
             Id at = a.contains("audioTrack") ? resolveTrack(cur, a["audioTrack"]) : 0;
             std::vector<Id> ids;
             edit(c.session, "add media clip", [&](Project& p) { ids = edit::addMediaClips(p, media, get<double>(a, "start", 0.0), vt, at); });
             return ids;
         }});
    add({"clip.move", "Move a clip in time and optionally to another track",
         {req("clip", "id"), req("start", "number"), opt("track", "id"), opt("linked", "bool", "move linked clips (default true)")},
         [edit](A a, C c) -> json {
             const Project& cur = c.session.project();
             Id id = resolveClip(cur, a["clip"]);
             std::optional<Id> track;
             if (a.contains("track")) track = resolveTrack(cur, a["track"]);
             edit(c.session, "move clip", [&](Project& p) { edit::moveClip(p, id, a["start"], track, get<bool>(a, "linked", true)); });
             return clipToJson(*c.session.project().findClip(id));
         }});
    add({"clip.trim", "Set a clip's start and duration (source offset follows the start)",
         {req("clip", "id"), req("start", "number"), req("duration", "number"), opt("linked", "bool")},
         [edit](A a, C c) -> json {
             Id id = resolveClip(c.session.project(), a["clip"]);
             edit(c.session, "trim clip", [&](Project& p) { edit::trimClip(p, id, a["start"], a["duration"], get<bool>(a, "linked", true)); });
             return clipToJson(*c.session.project().findClip(id));
         }});
    add({"clip.split", "Split a clip at a timeline time; returns the new right-hand ids",
         {req("clip", "id"), req("time", "number"), opt("linked", "bool")}, [edit](A a, C c) -> json {
             Id id = resolveClip(c.session.project(), a["clip"]);
             std::vector<Id> ids;
             edit(c.session, "split clip", [&](Project& p) { ids = edit::splitClip(p, id, a["time"], get<bool>(a, "linked", true)); });
             return ids;
         }});
    add({"clip.remove", "Remove a clip", {req("clip", "id"), opt("linked", "bool")}, [edit](A a, C c) -> json {
             Id id = resolveClip(c.session.project(), a["clip"]);
             edit(c.session, "remove clip", [&](Project& p) { edit::removeClip(p, id, get<bool>(a, "linked", true)); });
             return nullptr;
         }});
    add({"clip.rename", "Rename a clip", {req("clip", "id"), req("name", "string")}, [edit](A a, C c) -> json {
             Id id = resolveClip(c.session.project(), a["clip"]);
             edit(c.session, "rename clip", [&](Project& p) { edit::renameClip(p, id, a["name"]); });
             return nullptr;
         }});
    add({"clip.speed", "Set a media clip's playback speed", {req("clip", "id"), req("speed", "number")}, [edit](A a, C c) -> json {
             Id id = resolveClip(c.session.project(), a["clip"]);
             edit(c.session, "clip speed", [&](Project& p) { edit::setClipSpeed(p, id, a["speed"]); });
             return clipToJson(*c.session.project().findClip(id));
         }});
    add({"clip.unlink", "Unlink a clip from its A/V partner", {req("clip", "id")}, [edit](A a, C c) -> json {
             Id id = resolveClip(c.session.project(), a["clip"]);
             edit(c.session, "unlink clip", [&](Project& p) { edit::unlinkClip(p, id); });
             return nullptr;
         }});
    add({"clip.get", "Show a clip with its evaluated property values", {req("clip", "id"), opt("time", "number")}, [](A a, C c) -> json {
             Session& s = c.session;
             Evaluator& ev = s.evaluator();
             const Project& ep = ev.project();
             Id id = resolveClip(ep, a["clip"]);
             const Clip& clip = *ep.findClip(id);
             double t = get<double>(a, "time", s.time());
             ev.setTime(t);
             json j = clipToJson(clip);
             json values = json::object();
             const Track* track = ep.findTrack(clip.track);
             for (const auto& spec : clipPropSpecs(clip.kind, track ? track->kind : TrackKind::Video))
                 values[spec.name] = valueToJson(ev.prop(clip, spec.name));
             j["time"] = t;
             j["values"] = values;
             return j;
         }});

    // ---- properties ----
    add({"prop.set", "Set a property to a constant", {req("target", "id", "clip, effect, or transition"), req("name", "string"), req("value", "value")},
         [edit](A a, C c) -> json {
             Id id = resolveTarget(c.session.project(), a["target"]);
             edit(c.session, "set " + a["name"].get<std::string>(),
                  [&](Project& p) { edit::setPropConstant(p, id, a["name"], valueFromJson(a["value"])); });
             return propertyToJson(propOf(c.session.project(), id, a["name"]));
         }});
    add({"prop.expr", "Set a property to a Lua expression (t, lt, progress, sig.*, prop(clip, name))",
         {req("target", "id"), req("name", "string"), req("expr", "string")}, [edit](A a, C c) -> json {
             Id id = resolveTarget(c.session.project(), a["target"]);
             edit(c.session, "expression " + a["name"].get<std::string>(), [&](Project& p) { edit::setPropExpr(p, id, a["name"], a["expr"]); });
             // Evaluate the new expression once while its clip is active, so errors show now.
             evaluatedProp(c.session, id, a["name"], activeTime(c.session, id));
             return {{"errors", errorsJson(c.session, c.session.time())}};
         }});
    add({"prop.key", "Add or replace a keyframe at clip-local time",
         {req("target", "id"), req("name", "string"), req("time", "number"), req("value", "number"),
          opt("ease", "string", "hold, linear, bezier, or a preset like ease-in-out")},
         [edit](A a, C c) -> json {
             Id id = resolveTarget(c.session.project(), a["target"]);
             std::string name = a["name"];
             double t = a["time"];
             std::string ease = get<std::string>(a, "ease", "");
             const EasePreset* preset = ease.empty() || parseInterp(ease) ? nullptr : findEase(ease);
             if (!ease.empty() && !preset && !parseInterp(ease)) throw EditError(fmt::format("unknown ease '{}'", ease));
             edit(c.session, "keyframe " + name, [&](Project& p) {
                 std::optional<Interp> interp = preset ? std::optional(Interp::Bezier) : (ease.empty() ? std::nullopt : parseInterp(ease));
                 edit::setKeyframe(p, id, name, t, a["value"], interp);
                 if (preset) {
                     const auto& keys = edit::propsOf(p, id).at(name).curve.keys;
                     size_t index = std::ranges::find_if(keys, [&](const Keyframe& k) { return std::abs(k.t - t) < 1e-6; }) - keys.begin();
                     edit::setKeyframeEase(p, id, name, index, Interp::Bezier, preset->x1, preset->y1, preset->x2, preset->y2);
                 }
             });
             return propertyToJson(propOf(c.session.project(), id, name));
         }});
    add({"prop.unkey", "Remove the keyframe at clip-local time", {req("target", "id"), req("name", "string"), req("time", "number")},
         [edit](A a, C c) -> json {
             Id id = resolveTarget(c.session.project(), a["target"]);
             edit(c.session, "remove keyframe", [&](Project& p) { edit::removeKeyframe(p, id, a["name"], a["time"]); });
             return nullptr;
         }});
    add({"prop.get", "Show a property definition and its value at a time", {req("target", "id"), req("name", "string"), opt("time", "number")},
         [](A a, C c) -> json {
             Session& s = c.session;
             const Project& ep = s.evaluator().project();
             Id id = resolveTarget(ep, a["target"]);
             std::string name = a["name"];
             const Property& prop = propOf(ep, id, name);
             double t = get<double>(a, "time", s.time());
             return {{"mode", propModeName(prop.mode)}, {"property", propertyToJson(prop)}, {"time", t},
                     {"value", valueToJson(evaluatedProp(s, id, name, t))}};
         }});

    // ---- effects and transitions ----
    add({"effect.add", "Add a video or audio effect to a clip, or an audio effect to 'master'",
         {req("target", "id", "clip or master"), req("type", "string"), opt("index", "int")}, [edit](A a, C c) -> json {
             std::string type = a["type"];
             bool master = a["target"].is_string() && a["target"] == "master";
             Id clip = master ? 0 : resolveClip(c.session.project(), a["target"]);
             bool audio = findAudioEffect(type) != nullptr;
             if (!audio && !findVideoEffect(type)) throw EditError(fmt::format("unknown effect '{}'", type));
             Id id = 0;
             edit(c.session, "add " + type, [&](Project& p) { id = edit::addEffect(p, clip, audio, type, get<int>(a, "index", -1)); });
             return effectToJson(*findEffectIn(c.session.project(), id));
         }});
    add({"effect.remove", "Remove an effect", {req("effect", "id")}, [edit](A a, C c) -> json {
             Id id = a["effect"].is_number() ? a["effect"].get<Id>() : throw EditError("effect needs an id");
             edit(c.session, "remove effect", [&](Project& p) { edit::removeEffect(p, id); });
             return nullptr;
         }});
    add({"effect.move", "Move an effect up (-1) or down (+1) its chain", {req("effect", "id"), req("delta", "int")}, [edit](A a, C c) -> json {
             Id id = a["effect"].is_number() ? a["effect"].get<Id>() : throw EditError("effect needs an id");
             edit(c.session, "move effect", [&](Project& p) { edit::moveEffect(p, id, a["delta"]); });
             return nullptr;
         }});
    add({"effect.enable", "Enable or bypass an effect", {req("effect", "id"), opt("enabled", "bool")}, [edit](A a, C c) -> json {
             Id id = a["effect"].is_number() ? a["effect"].get<Id>() : throw EditError("effect needs an id");
             bool on = get<bool>(a, "enabled", true);
             edit(c.session, on ? "enable effect" : "bypass effect", [&](Project& p) { edit::setEffectEnabled(p, id, on); });
             return nullptr;
         }});
    add({"transition.add", "Add a transition across the cut from clip a to clip b",
         {req("a", "id"), req("b", "id"), opt("type", "string", "crossfade, wipe, dip, slide"), opt("duration", "number")},
         [edit](A a, C c) -> json {
             const Project& cur = c.session.project();
             Id ca = resolveClip(cur, a["a"]), cb = resolveClip(cur, a["b"]);
             Id id = 0;
             edit(c.session, "add transition", [&](Project& p) {
                 id = edit::addTransition(p, ca, cb, get<std::string>(a, "type", "crossfade"), get<double>(a, "duration", 1.0));
             });
             return transitionToJson(*c.session.project().findTransition(id));
         }});
    add({"transition.remove", "Remove a transition", {req("transition", "id")}, [edit](A a, C c) -> json {
             Id id = a["transition"].is_number() ? a["transition"].get<Id>() : throw EditError("transition needs an id");
             edit(c.session, "remove transition", [&](Project& p) { edit::removeTransition(p, id); });
             return nullptr;
         }});
    add({"transition.set", "Change a transition's type or duration", {req("transition", "id"), opt("type", "string"), opt("duration", "number")},
         [edit](A a, C c) -> json {
             Id id = a["transition"].is_number() ? a["transition"].get<Id>() : throw EditError("transition needs an id");
             std::optional<std::string> type;
             std::optional<double> dur;
             if (a.contains("type")) type = a["type"].get<std::string>();
             if (a.contains("duration")) dur = a["duration"].get<double>();
             edit(c.session, "transition settings", [&](Project& p) { edit::setTransition(p, id, type, dur); });
             return transitionToJson(*c.session.project().findTransition(id));
         }});
    add({"transition.curve", "Set a transition's mix curve: an ease name or a JSON key list",
         {req("transition", "id"), req("curve", "value")}, [edit](A a, C c) -> json {
             Id id = a["transition"].is_number() ? a["transition"].get<Id>() : throw EditError("transition needs an id");
             Curve curve = curveArg(a["curve"]);
             edit(c.session, "transition curve", [&](Project& p) { edit::setTransitionCurve(p, id, curve); });
             return transitionToJson(*c.session.project().findTransition(id));
         }});

    // ---- script and evaluation ----
    add({"signal.set", "Define a project signal as a constant or an expression",
         {req("name", "string"), opt("value", "value"), opt("expr", "string")}, [edit](A a, C c) -> json {
             if (a.contains("value") == a.contains("expr")) throw EditError("give exactly one of value or expr");
             Property prop = a.contains("expr") ? Property::expression(a["expr"]) : Property::of(valueFromJson(a["value"]));
             edit(c.session, "signal " + a["name"].get<std::string>(), [&](Project& p) { edit::setSignal(p, a["name"], prop); });
             return {{"errors", errorsJson(c.session, c.session.time())}};
         }});
    add({"signal.remove", "Remove a project signal", {req("name", "string")}, [edit](A a, C c) -> json {
             edit(c.session, "remove signal", [&](Project& p) { edit::removeSignal(p, a["name"]); });
             return nullptr;
         }});
    add({"script.get", "Show the project script and its error", {}, [](A, C c) -> json {
             const auto& out = c.session.evaluator().script();
             json j = {{"source", c.session.project().script}, {"signals", out.signals}, {"generators", out.generators},
                       {"synths", out.synths}, {"clips", out.clips.size()}};
             if (out.error) j["error"] = {{"line", out.error->line}, {"message", out.error->message}};
             return j;
         }});
    add({"script.set", "Replace the project script from text or a file", {opt("source", "string"), opt("file", "path")},
         [edit](A a, C c) -> json {
             if (a.contains("source") == a.contains("file")) throw EditError("give exactly one of source or file");
             std::string src;
             if (a.contains("file")) {
                 std::ifstream in(a["file"].get<std::string>(), std::ios::binary);
                 if (!in) throw EditError("cannot read " + a["file"].get<std::string>());
                 src.assign(std::istreambuf_iterator<char>(in), {});
             } else {
                 src = a["source"];
             }
             edit(c.session, "edit script", [&](Project& p) { edit::setScript(p, src); });
             const auto& out = c.session.evaluator().script();
             json j = {{"signals", out.signals}, {"generators", out.generators}, {"synths", out.synths}, {"clips", out.clips.size()}};
             if (out.error) j["error"] = {{"line", out.error->line}, {"message", out.error->message}};
             return j;
         }});
    add({"eval", "Evaluate a Lua expression or statement at a time", {req("code", "string"), opt("time", "number")}, [](A a, C c) -> json {
             Evaluator& ev = c.session.evaluator();
             ev.setTime(get<double>(a, "time", c.session.time()));
             std::string err;
             Value v = ev.eval(a["code"], &err);
             if (!err.empty()) throw EditError(err);
             return valueToJson(v);
         }});
    add({"graph", "Show the reactive graph: nodes, values, and dependencies", {opt("time", "number")}, [](A a, C c) -> json {
             Evaluator& ev = c.session.evaluator();
             ev.setTime(get<double>(a, "time", c.session.time()));
             ev.refreshOutputs();
             json nodes = json::array();
             const Graph& g = ev.graph();
             for (size_t i = 0; i < g.size(); ++i) {
                 const auto& n = g.node(Graph::Index(i));
                 json deps = json::array();
                 for (auto d : n.deps) deps.push_back(g.node(d).key);
                 json j = {{"key", n.key}, {"kind", kindName(n.kind)}, {"value", valueToJson(n.value)}, {"deps", deps}, {"evals", n.evalCount}};
                 if (!n.label.empty()) j["label"] = n.label;
                 if (!n.error.empty()) j["error"] = n.error;
                 nodes.push_back(j);
             }
             return nodes;
         }});
    add({"errors", "List script, expression, and render errors", {}, [](A, C c) -> json { return errorsJson(c.session, c.session.time()); }});

    // ---- transport and output ----
    add({"seek", "Move the playhead", {req("time", "number")}, [](A a, C c) -> json {
             c.session.seek(a["time"]);
             return c.session.time();
         }});
    add({"play", "Start playback", {}, [](A, C c) -> json {
             if (!c.session.player().playing()) c.session.togglePlay();
             return c.session.time();
         }});
    add({"pause", "Pause playback", {}, [](A, C c) -> json {
             c.session.player().pause();
             return c.session.time();
         }});
    add({"toggle", "Toggle playback", {}, [](A, C c) -> json {
             c.session.togglePlay();
             return {{"playing", c.session.player().playing()}, {"time", c.session.time()}};
         }});
    add({"time", "Show the playhead and transport state", {}, [](A, C c) -> json {
             return {{"time", c.session.time()}, {"playing", c.session.player().playing()},
                     {"duration", c.session.evaluator().project().duration()}};
         }});
    add({"frame", "Render the frame at a time to a PNG",
         {req("path", "path"), opt("time", "number"), opt("width", "int"), opt("height", "int")}, [](A a, C c) -> json {
             Session& s = c.session;
             const auto& st = s.project().settings;
             int w = get<int>(a, "width", 0), h = get<int>(a, "height", 0);
             if (w > 0 && h <= 0) h = std::max(2, int(std::lround(double(w) * st.height / st.width)));
             if (h > 0 && w <= 0) w = std::max(2, int(std::lround(double(h) * st.width / st.height)));
             if (w <= 0) w = st.width, h = st.height;
             if (w > 16384 || h > 16384) throw EditError("frame size too large");
             double t = get<double>(a, "time", s.time());
             auto img = s.renderFrame(t, w, h);
             std::string path = a["path"];
             if (!savePng(*img, path.c_str())) throw EditError("cannot write " + path);
             return {{"path", path}, {"time", t}, {"width", img->w}, {"height", img->h}, {"errors", errorsJson(s, t)}};
         }});
    add({"export", "Export MP4 (H.264 + AAC) or WAV in the background",
         {req("path", "path"), opt("width", "int"), opt("height", "int"), opt("start", "number"), opt("end", "number"),
          opt("crf", "int"), opt("wait", "bool", "reply when the export finishes")},
         [](A a, C c) -> json {
             Session& s = c.session;
             ExportOptions o;
             o.path = a["path"];
             o.width = get<int>(a, "width", 0);
             o.height = get<int>(a, "height", 0);
             o.start = get<double>(a, "start", 0.0);
             o.end = get<double>(a, "end", -1.0);
             o.crf = get<int>(a, "crf", 18);
             std::string err;
             if (!s.exportJob().start(s.project(), o, &err)) throw EditError(err);
             s.log(LogLine::Level::Info, "export started: " + o.path);
             if (!get<bool>(a, "wait", false)) return exportStatus(s);
             Reply reply = c.defer();
             s.pollers.push_back([&s, reply] {
                 if (s.exportJob().running()) return false;
                 json st = exportStatus(s);
                 if (st.contains("error")) reply({{"error", st["error"]}});
                 else reply(st);
                 return true;
             });
             return nullptr;
         }});
    add({"export.status", "Show export progress", {}, [](A, C c) -> json { return exportStatus(c.session); }});
    add({"export.cancel", "Cancel the running export", {}, [](A, C c) -> json {
             c.session.exportJob().cancel();
             return nullptr;
         }});

    // ---- history and session state ----
    add({"undo", "Undo the last edit", {}, [](A, C c) -> json { return c.session.undo(); }});
    add({"redo", "Redo the last undone edit", {}, [](A, C c) -> json { return c.session.redo(); }});
    add({"history", "List undo and redo steps", {}, [](A, C c) -> json {
             return {{"undo", c.session.history().undoLabels()}, {"redo", c.session.history().redoLabels()}};
         }});
    add({"select", "Show or set the selection",
         {opt("clips", "value", "id, name, or list"), opt("track", "id"), opt("media", "id"), opt("effect", "id"),
          opt("transition", "id"), opt("prop", "string")},
         [](A a, C c) -> json {
             Session& s = c.session;
             const Project& p = s.project();
             if (!a.empty()) {
                 Selection sel;
                 if (a.contains("clips")) {
                     json list = a["clips"].is_array() ? a["clips"] : json::array({a["clips"]});
                     for (const auto& v : list)
                         if (!(v.is_string() && v.get<std::string>().empty())) sel.clips.push_back(resolveClip(p, v));
                 }
                 if (a.contains("track")) sel.track = resolveTrack(p, a["track"]);
                 if (a.contains("media")) sel.media = resolveMedia(p, a["media"]);
                 if (a.contains("effect")) sel.effect = resolveTarget(p, a["effect"]);
                 if (a.contains("transition")) sel.transition = resolveTarget(p, a["transition"]);
                 sel.prop = get<std::string>(a, "prop", "");
                 s.selection = sel;
             }
             return selectionJson(s.selection);
         }});
    add({"log", "Show log lines after a sequence number", {opt("since", "int")}, [](A a, C c) -> json {
             const auto& lines = c.session.logLines();
             uint64_t first = c.session.logCount() - lines.size();
             uint64_t since = get<uint64_t>(a, "since", 0);
             json out = json::array();
             for (size_t i = 0; i < lines.size(); ++i) {
                 uint64_t seq = first + i + 1;
                 if (seq <= since) continue;
                 const char* lv = lines[i].level == LogLine::Level::Error ? "error" : lines[i].level == LogLine::Level::Warn ? "warn" : "info";
                 out.push_back({{"seq", seq}, {"level", lv}, {"text", lines[i].text}});
             }
             return out;
         }});
    add({"levels", "Show the master meter levels (linear peak and RMS)", {}, [](A, C c) -> json {
             const auto& l = c.session.player().levels();
             return {{"peak", l.peak}, {"rms", l.rms}};
         }});
}

}  // namespace mf
