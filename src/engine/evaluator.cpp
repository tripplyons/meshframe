#include "engine/evaluator.hpp"

#include "core/work_counter.hpp"
#include "model/edit.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>

namespace mf {

namespace {

Value coerceLoose(const PropSpec* spec, Value v) {
    if (!spec) return v;
    switch (spec->type) {
        case ValueType::Number: return asNumber(v, asNumber(spec->def));
        case ValueType::Color: return asColor(v, asColor(spec->def));
        case ValueType::String: return typeOf(v) == ValueType::None ? spec->def : Value(asString(v));
        case ValueType::None: return v;
    }
    return v;
}

}  // namespace

Evaluator::Evaluator() { lua_.setHost(this); }

void Evaluator::setProject(const Project& p) {
    // The script can read markers, so moving one reruns it.
    bool markersChanged = p.markers != lastMarkers_;
    lastMarkers_ = p.markers;
    lua_.setConstants(p.settings.fps, p.settings.width, p.settings.height, p.settings.sampleRate);
    lua_.setMarkers(p.markers);
    if (!scriptRan_ || p.script != lastScript_ || markersChanged) {
        // Rebuilding drops graph nodes that point at compiled expressions.
        graph_.clear();
        script_ = lua_.runScript(p.script);
        lastScript_ = p.script;
        scriptRan_ = true;
    }
    effective_ = p;
    instantiateScriptClips();
    trackClips_.clear();
    clipTransitions_.clear();
    std::unordered_map<Id, const Clip*> byId;
    for (const auto& c : effective_.clips) {
        trackClips_[c.track].push_back(&c);
        if (!effective_.transitions.empty()) byId.try_emplace(c.id, &c);
    }
    for (const auto& tr : effective_.transitions) {
        auto b = byId.find(tr.clipB);
        double cut = b == byId.end() ? 0 : b->second->start;
        ClipTransition indexed{&tr, cut - tr.duration / 2, cut + tr.duration / 2};
        clipTransitions_[tr.clipA].push_back(indexed);
        if (tr.clipB != tr.clipA) clipTransitions_[tr.clipB].push_back(indexed);
    }
    rebuildGraph();
}

void Evaluator::instantiateScriptClips() {
    scriptClipErrors_.clear();
    Id next = kScriptIdBase;
    for (size_t i = 0; i < script_.clips.size(); ++i) {
        const ScriptClipSpec& s = script_.clips[i];
        auto failSpec = [&](const std::string& msg) {
            scriptClipErrors_.push_back({0, fmt::format("mf.clip #{}: {}", i + 1, msg)});
        };
        bool audio = s.kind == ClipKind::Synth;
        const Track* track = nullptr;
        if (!s.track.empty()) {
            track = effective_.findTrackByName(s.track);
            if (!track) {
                failSpec(fmt::format("no track named '{}'", s.track));
                continue;
            }
        } else {
            const MediaSource* m = s.kind == ClipKind::Media ? effective_.findMediaByName(s.media) : nullptr;
            if (m && !m->hasVideo) audio = true;
            int want = std::max(0, s.trackIndex);
            for (const auto& t : effective_.tracks) {
                if ((t.kind == TrackKind::Audio) != audio) continue;
                if (want-- == 0) {
                    track = &t;
                    break;
                }
            }
            if (!track) {
                failSpec("no matching track; name one with track = \"V1\"");
                continue;
            }
        }
        Clip c;
        c.kind = s.kind;
        c.track = track->id;
        c.name = s.name.empty() ? fmt::format("script {}", i + 1) : s.name;
        c.start = s.start;
        c.duration = s.duration;
        c.in = s.in;
        c.speed = s.speed;
        if (s.kind == ClipKind::Media) {
            const MediaSource* m = effective_.findMediaByName(s.media);
            if (!m) {
                failSpec(fmt::format("no media named '{}'", s.media));
                continue;
            }
            c.media = m->id;
        }
        auto specs = clipPropSpecs(c.kind, track->kind);
        bool bad = false;
        for (const auto& [name, prop] : s.props) {
            const PropSpec* spec = nullptr;
            for (const auto& sp : specs)
                if (name == sp.name) spec = &sp;
            if (!spec) {
                failSpec(fmt::format("{} clips have no property '{}'", clipKindName(c.kind), name));
                bad = true;
                break;
            }
            Property copy = prop;
            if (copy.mode == PropMode::Constant) {
                try {
                    copy.constant = coerce(*spec, copy.constant);
                } catch (const std::exception& e) {
                    failSpec(e.what());
                    bad = true;
                    break;
                }
            }
            c.props[name] = std::move(copy);
        }
        if (bad) continue;
        try {
            Id assigned = edit::addClip(effective_, c);
            effective_.findClip(assigned)->id = next++;
        } catch (const std::exception& e) {
            failSpec(e.what());
        }
    }
}

Graph::Index Evaluator::addPropertyNode(const std::string& key, uint64_t owner, const std::string& name,
                                        const std::string& label, const Property& prop, const PropSpec* spec, Context ctx, bool localCurveTime,
                                        Graph::Kind kind) {
    // Constants are not nodes; they cannot change over time.
    if (prop.mode == PropMode::Constant) return Graph::kNone;
    Graph::Index idx;
    if (prop.mode == PropMode::Curve) {
        Curve curve = prop.curve;
        idx = graph_.addComputed(key, kind, [this, curve, ctx, localCurveTime](Graph& g) {
            double t = asNumber(g.get(timeNode_));
            return Value(curve.sample(localCurveTime ? t - ctx.start : t));
        }, label, owner);
    } else {
        std::string expr = prop.expr;
        idx = graph_.addComputed(key, kind, [this, expr, ctx, spec, key](Graph& g) {
            std::string err;
            int ref = lua_.compile(expr, &err);
            if (ref < 0) {
                g.reportError(err);
                return spec ? spec->def : Value{};
            }
            ctx_.push_back(ctx);
            Value v = lua_.call(ref, &err);
            ctx_.pop_back();
            if (!err.empty()) {
                g.reportError(err);
                return spec ? spec->def : Value{};
            }
            return coerceLoose(spec, std::move(v));
        }, label, owner);
    }
    propNodes_[owner].push_back({name, idx});
    return idx;
}

void Evaluator::rebuildGraph() {
    graph_.clear();
    propNodes_.clear();
    transitionNodes_.clear();
    timeNode_ = graph_.addSource("time", time_, "time");

    // Signals: project signals first, then script signals.
    for (const auto& s : effective_.signals) {
        std::string key = "signal:" + s.name;
        if (s.value.mode == PropMode::Constant) {
            graph_.addSource(key, s.value.constant, s.name);
        } else {
            addPropertyNode(key, s.id, s.name, s.name, s.value, nullptr, Context{}, false, Graph::Kind::Signal);
        }
    }
    for (const auto& name : script_.signals) {
        std::string key = "signal:" + name;
        if (graph_.find(key) != Graph::kNone) continue;
        graph_.addComputed(key, Graph::Kind::Signal, [this, name](Graph& g) {
            std::string err;
            Value v = lua_.callSignal(name, &err);
            if (!err.empty()) g.reportError(err);
            return v;
        }, name);
    }

    std::vector<Graph::Index> videoClips, audioClips;
    for (const auto& c : effective_.clips) {
        const Track* track = effective_.findTrack(c.track);
        if (!track) continue;
        Context ctx{c.start, c.duration};
        std::vector<Graph::Index> owned;
        auto specs = clipPropSpecs(c.kind, track->kind);
        for (const auto& [name, prop] : c.props) {
            const PropSpec* spec = nullptr;
            for (const auto& sp : specs)
                if (name == sp.name) spec = &sp;
            auto idx = addPropertyNode(fmt::format("prop:{}.{}", c.id, name), c.id, name, fmt::format("{}.{}", c.name, name),
                                       prop, spec, ctx, true);
            if (idx != Graph::kNone) owned.push_back(idx);
        }
        auto addEffects = [&](const std::vector<Effect>& chain, bool audio) {
            for (const auto& e : chain) {
                const EffectSpec* es = audio ? findAudioEffect(e.type) : findVideoEffect(e.type);
                for (const auto& [name, prop] : e.props) {
                    const PropSpec* spec = es ? findSpec(es->props, name) : nullptr;
                    auto idx = addPropertyNode(fmt::format("prop:{}.{}", e.id, name), e.id, name,
                                               fmt::format("{}.{}.{}", c.name, e.type, name), prop, spec, ctx, true);
                    if (idx != Graph::kNone) owned.push_back(idx);
                }
            }
        };
        addEffects(c.videoEffects, false);
        addEffects(c.audioEffects, true);
        const Clip* cp = &c;
        auto clipNode = graph_.addComputed(fmt::format("clip:{}", c.id), Graph::Kind::Clip,
                                           [this, cp, owned](Graph& g) {
            double t = asNumber(g.get(timeNode_));
            bool active = clipVisibleAt(*cp, t);
            if (active)
                for (auto i : owned) g.get(i);
            return Value(active ? 1.0 : 0.0);
        }, c.name, c.id);
        (track->kind == TrackKind::Video ? videoClips : audioClips).push_back(clipNode);
    }

    for (const auto& tr : effective_.transitions) {
        const EffectSpec* es = findTransition(tr.type);
        auto [ws, we] = window(effective_, tr);
        for (const auto& [name, prop] : tr.props) {
            const PropSpec* spec = es ? findSpec(es->props, name) : nullptr;
            addPropertyNode(fmt::format("prop:{}.{}", tr.id, name), tr.id, name, fmt::format("transition.{}", name), prop, spec,
                            Context{ws, we - ws}, true);
        }
        Curve curve = tr.curve;
        double start = ws, len = std::max(1e-9, we - ws);
        transitionNodes_[tr.id] = graph_.addComputed(fmt::format("transition:{}", tr.id), Graph::Kind::Transition,
                                                     [this, curve, start, len](Graph& g) {
            double x = std::clamp((asNumber(g.get(timeNode_)) - start) / len, 0.0, 1.0);
            return Value(std::clamp(curve.sample(x), -1.0, 2.0));
        }, fmt::format("{} {}", tr.type, tr.id), tr.id);
    }

    auto sum = [](std::vector<Graph::Index> nodes) {
        return [nodes](Graph& g) {
            double n = 0;
            for (auto i : nodes) n += asNumber(g.get(i));
            return Value(n);
        };
    };
    outVideo_ = graph_.addComputed("out:video", Graph::Kind::Output, sum(videoClips), "video out");
    outAudio_ = graph_.addComputed("out:audio", Graph::Kind::Output, sum(audioClips), "audio out");
}

void Evaluator::setTime(double t) {
    time_ = t;
    if (timeNode_ != Graph::kNone) graph_.set(timeNode_, t);
}

std::pair<double, double> Evaluator::window(const Project& p, const Transition& tr) {
    const Clip* b = p.findClip(tr.clipB);
    double cut = b ? b->start : 0;
    return {cut - tr.duration / 2, cut + tr.duration / 2};
}

bool Evaluator::clipVisibleAt(const Clip& c, double t) const {
    if (t >= c.start && t < c.end()) return true;
    for (const auto& tr : clipTransitions(c.id))
        if (t >= tr.start && t < tr.end) return true;
    return false;
}

const std::vector<const Clip*>& Evaluator::trackClips(Id track) const {
    static const std::vector<const Clip*> empty;
    auto it = trackClips_.find(track);
    return it == trackClips_.end() ? empty : it->second;
}

const std::vector<Evaluator::ClipTransition>& Evaluator::clipTransitions(Id clip) const {
    static const std::vector<ClipTransition> empty;
    auto it = clipTransitions_.find(clip);
    return it == clipTransitions_.end() ? empty : it->second;
}

Value Evaluator::lookup(uint64_t owner, std::string_view name, const PropertyMap& props) {
    work::addEval(1);
    auto it = props.find(std::string(name));
    if (it == props.end()) return std::monostate{};
    if (it->second.mode == PropMode::Constant) return it->second.constant;
    auto pn = propNodes_.find(owner);
    if (pn != propNodes_.end())
        for (const auto& n : pn->second)
            if (n.name == name) return graph_.get(n.node);
    return std::monostate{};
}

Value Evaluator::prop(const Clip& c, std::string_view name) { return lookup(c.id, name, c.props); }

double Evaluator::num(const Clip& c, std::string_view name, double fallback) { return asNumber(prop(c, name), fallback); }

Color Evaluator::color(const Clip& c, std::string_view name, Color fallback) { return asColor(prop(c, name), fallback); }

std::string Evaluator::str(const Clip& c, std::string_view name) {
    Value v = prop(c, name);
    return typeOf(v) == ValueType::None ? std::string() : asString(v);
}

Value Evaluator::effectProp(const Effect& e, std::string_view name) { return lookup(e.id, name, e.props); }

Value Evaluator::transitionProp(const Transition& tr, std::string_view name) { return lookup(tr.id, name, tr.props); }

namespace {
ParamFn withDefaults(std::function<Value(std::string_view)> lookup, const EffectSpec* spec) {
    return [lookup = std::move(lookup), spec](std::string_view name) {
        Value v = lookup(name);
        if (typeOf(v) == ValueType::None && spec)
            for (const auto& p : spec->props)
                if (name == p.name) return p.def;
        return v;
    };
}
}  // namespace

ParamFn Evaluator::effectParams(const Effect& e, bool audio) {
    return withDefaults([this, &e](std::string_view n) { return effectProp(e, n); },
                        audio ? findAudioEffect(e.type) : findVideoEffect(e.type));
}

ParamFn Evaluator::transitionParams(const Transition& tr) {
    return withDefaults([this, &tr](std::string_view n) { return transitionProp(tr, n); }, findTransition(tr.type));
}

double Evaluator::transitionMix(const Transition& tr) {
    auto it = transitionNodes_.find(tr.id);
    if (it == transitionNodes_.end()) return 0;
    return asNumber(graph_.get(it->second));
}

Value Evaluator::signal(std::string_view name) { return hostSignal(name); }

bool Evaluator::drawGenerator(const Clip& c, DrawList& out, std::string* err) {
    std::string name = str(c, "generator");
    if (name.empty()) return true;
    ctx_.push_back({c.start, c.duration});
    double lt = time_ - c.start;
    bool ok = lua_.draw(name, out, time_, lt, lt / std::max(1e-9, c.duration), effective_.settings.width,
                        effective_.settings.height, err);
    ctx_.pop_back();
    return ok;
}

bool Evaluator::renderSynth(const Clip& c, double t0, int frames, float* stereo, std::string* err) {
    std::string name = str(c, "synth");
    if (name.empty()) {
        std::fill_n(stereo, size_t(frames) * 2, 0.0f);
        return true;
    }
    ctx_.push_back({c.start, c.duration});
    bool ok = lua_.synth(name, t0, effective_.settings.sampleRate, frames, stereo, err);
    ctx_.pop_back();
    return ok;
}

void Evaluator::refreshOutputs() {
    if (outVideo_ != Graph::kNone) graph_.get(outVideo_);
    if (outAudio_ != Graph::kNone) graph_.get(outAudio_);
}

Value Evaluator::eval(const std::string& code, std::string* err) { return lua_.eval(code, err); }

std::vector<NodeError> Evaluator::errors() const {
    std::vector<NodeError> out;
    if (script_.error) out.push_back({"script", script_.error->message, script_.error->line});
    for (const auto& e : scriptClipErrors_) out.push_back({"script", e.message, e.line});
    for (const auto& n : graph_.nodes())
        if (!n.error.empty()) out.push_back({n.key, n.error, 0});
    for (const auto& [k, m] : runtimeErrors_) out.push_back({k, m, 0});
    return out;
}

void Evaluator::reportRuntimeError(const std::string& key, std::string message) { runtimeErrors_[key] = std::move(message); }

void Evaluator::clearRuntimeError(const std::string& key) { runtimeErrors_.erase(key); }

double Evaluator::hostTime() {
    if (timeNode_ == Graph::kNone) return time_;
    return asNumber(graph_.get(timeNode_));
}

double Evaluator::hostLocalTime() {
    double t = hostTime();
    return ctx_.empty() ? t : t - ctx_.back().start;
}

double Evaluator::hostProgress() {
    double t = hostTime();
    if (ctx_.empty()) {
        double d = effective_.duration();
        return d > 0 ? t / d : 0;
    }
    return (t - ctx_.back().start) / std::max(1e-9, ctx_.back().duration);
}

Value Evaluator::hostSignal(std::string_view name) {
    auto idx = graph_.find("signal:" + std::string(name));
    if (idx == Graph::kNone) {
        graph_.reportError(fmt::format("unknown signal '{}'", name));
        return std::monostate{};
    }
    return graph_.get(idx);
}

Value Evaluator::hostProp(std::string_view clip, std::string_view prop) {
    for (const auto& c : effective_.clips)
        if (c.name == clip) return lookup(c.id, prop, c.props);
    graph_.reportError(fmt::format("no clip named '{}'", clip));
    return std::monostate{};
}

void Evaluator::hostLog(std::string line) {
    if (onLog) onLog(std::move(line));
}

}  // namespace mf
