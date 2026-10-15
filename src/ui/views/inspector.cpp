#include "ui/views/common.hpp"
#include "ui/views/views.hpp"

#include <fmt/format.h>

namespace mf::ui::views {

namespace {

// Fields of the selection: a clip, transition, effect, track, or media source,
// or the project settings when nothing is selected.
class InspectorView : public View {
public:
    void draw(ViewEnv& env, const Rect& body) override {
        Context& ui = env.ui;
        Session& s = env.session;
        labelW_ = std::min(ui.dp(110), body.w * 0.4f);
        rowH_ = ui.rowHeight() + 2;
        ListLayout list(ui, body, "##inspector", contentH_);
        float top = list.next(0).y;
        const Project& p = s.evaluator().project();
        if (Id id = s.selection.primaryClip(); id && p.findClip(id)) clip(env, list, *p.findClip(id));
        else if (s.selection.transition && p.findTransition(s.selection.transition)) transition(env, list, *p.findTransition(s.selection.transition));
        else if (s.selection.effect && const_cast<Project&>(s.project()).findEffect(s.selection.effect)) effect(env, list, s.selection.effect);
        else if (s.selection.track && p.findTrack(s.selection.track)) track(env, list, *p.findTrack(s.selection.track));
        else if (s.selection.media && p.findMedia(s.selection.media)) media(env, list, *p.findMedia(s.selection.media));
        else project(env, list);
        contentH_ = list.next(0).y - top + ui.pad();
    }

private:
    Rect section(Context& ui, ListLayout& list, std::string_view title) {
        Rect r = list.next(ui.rowHeight());
        Rect area = r;
        Rect rest = sectionHeader(ui, area, title);
        return rest;
    }

    Rect field(Context& ui, ListLayout& list, std::string_view label) { return labeled(ui, list.next(rowH_), label, labelW_); }

    void text(Context& ui, ListLayout& list, std::string_view label, std::string_view value) {
        Rect r = field(ui, list, label);
        ui.painter.textIn(r.inset(ui.dp(6), 0), value, ui.col(Slot::Text));
    }

    void props(ViewEnv& env, ListLayout& list, Id target, std::span<const PropSpec> specs, bool readOnly) {
        for (const PropSpec& spec : specs) {
            Rect r = list.next(rowH_);
            if (list.visible(r)) propertyRow(env, r, target, spec, labelW_, readOnly);
        }
    }

    void clip(ViewEnv& env, ListLayout& list, const Clip& clipRef) {
        Context& ui = env.ui;
        Session& s = env.session;
        Shell& shell = env.shell;
        const Clip c = clipRef;  // a copy: edits below replace the project
        const Project& p = s.evaluator().project();
        const Track* tr = p.findTrack(c.track);
        TrackKind tk = tr ? tr->kind : TrackKind::Video;
        std::string trackName = tr ? tr->name : "?";
        bool ro = c.scriptOwned();
        Id id = c.id;
        double fps = p.settings.fps;
        ui.pushId(id);

        Rect head = section(ui, list, fmt::format("{} clip", clipKindName(c.kind)));
        if (size_t n = s.selection.clips.size(); n > 1) {
            ui.painter.textIn(head, fmt::format("{} clips selected", n), ui.col(Slot::TextDim), Align::Right);
            Rect r = list.next(rowH_);
            ui.painter.textIn(r.inset(ui.dp(8), 0), fmt::format("Edits here apply to \"{}\" only.", c.name), ui.col(Slot::TextDim));
        }
        if (ro) {
            Rect r = list.next(rowH_);
            ui.painter.textIn(r.inset(ui.dp(8), 0), "Defined by the project script. Edit it in the script view.", ui.col(Slot::TextDim));
        }
        if (ro) text(ui, list, "Name", c.name);
        else {
            std::string name = c.name;
            if (textField(ui, field(ui, list, "Name"), "##name", name).committed)
                shell.edit("Rename clip", [&](Project& pr) { edit::renameClip(pr, id, name); });
        }
        text(ui, list, "Track", trackName);
        if (ro) {
            text(ui, list, "Start", formatTimecode(c.start, fps));
            text(ui, list, "Duration", formatTimecode(c.duration, fps));
        } else {
            NumberStyle time{.step = 1 / fps, .min = 0, .decimals = 3, .unit = "s"};
            editNumber(env, field(ui, list, "Start"), "##start", c.start, time, "Move clip",
                       [&](Project& pr, double v) { edit::moveClip(pr, id, v); });
            NumberStyle dur{.step = 1 / fps, .min = 1 / fps, .decimals = 3, .unit = "s"};
            double start = c.start;
            editNumber(env, field(ui, list, "Duration"), "##duration", c.duration, dur, "Trim clip",
                       [&](Project& pr, double v) { edit::trimClip(pr, id, start, v); });
            if (c.kind == ClipKind::Media) {
                NumberStyle speed{.step = 0.01, .min = 0.01, .max = 100, .decimals = 2, .unit = "x"};
                editNumber(env, field(ui, list, "Speed"), "##speed", c.speed, speed, "Set speed",
                           [&](Project& pr, double v) { edit::setClipSpeed(pr, id, v); });
            }
        }
        if (c.kind == ClipKind::Media) {
            const MediaSource* m = p.findMedia(c.media);
            text(ui, list, "Source", m ? m->name : "missing");
            text(ui, list, "Source in", formatTimecode(c.in, fps));
        }
        if (c.link) text(ui, list, "Linked", "Moves with its linked clip");

        section(ui, list, "Properties");
        if (c.kind == ClipKind::Path) {
            text(ui, list, "Draw / morph", "Animate trimEnd or morph in Curves.");
            text(ui, list, "Path", "SVG path data; coordinates from clip center.");
        }
        props(env, list, id, clipPropSpecs(c.kind, tk), ro);

        section(ui, list, "Effects");
        size_t count = tk == TrackKind::Video ? c.videoEffects.size() : c.audioEffects.size();
        Rect r = list.next(rowH_).inset(ui.dp(4), ui.dp(2));
        ui.painter.textIn(cutLeft(r, r.w / 2).inset(ui.dp(4), 0), fmt::format("{} {} effects", count, tk == TrackKind::Video ? "video" : "audio"),
                          ui.col(Slot::TextDim));
        if (button(ui, r, "Edit effects")) shell.showView(tk == TrackKind::Video ? "video-effects" : "audio-effects");
        ui.popId();
    }

    void transition(ViewEnv& env, ListLayout& list, const Transition& trRef) {
        Context& ui = env.ui;
        Shell& shell = env.shell;
        const Transition tr = trRef;
        Id id = tr.id;
        ui.pushId(id);
        section(ui, list, "Transition");
        std::vector<std::string> types;
        int index = 0;
        for (const auto& spec : transitionSpecs()) {
            if (tr.type == spec.type) index = int(types.size());
            types.push_back(spec.label);
        }
        if (dropdown(ui, field(ui, list, "Type"), "##type", index, types)) {
            std::string type = transitionSpecs()[index].type;
            shell.edit("Set transition type", [&](Project& pr) { edit::setTransition(pr, id, type, std::nullopt); });
        }
        double fps = env.session.project().settings.fps;
        NumberStyle dur{.step = 1 / fps, .min = 1 / fps, .decimals = 3, .unit = "s"};
        editNumber(env, field(ui, list, "Duration"), "##duration", tr.duration, dur, "Set transition duration",
                   [&](Project& pr, double v) { edit::setTransition(pr, id, std::nullopt, v); });
        Rect r = field(ui, list, "Curve");
        if (button(ui, r, "Edit curve")) shell.showView("curves");
        if (const EffectSpec* spec = findTransition(tr.type); spec && !spec->props.empty()) {
            section(ui, list, "Properties");
            props(env, list, id, spec->props, false);
        }
        ui.popId();
    }

    void effect(ViewEnv& env, ListLayout& list, Id id) {
        Context& ui = env.ui;
        Project& p = const_cast<Project&>(env.session.project());
        Clip* owner = nullptr;
        const Effect* e = p.findEffect(id, &owner);
        bool audio = !owner || std::any_of(owner->audioEffects.begin(), owner->audioEffects.end(), [&](const Effect& x) { return x.id == id; });
        const EffectSpec* spec = audio ? findAudioEffect(e->type) : findVideoEffect(e->type);
        ui.pushId(id);
        section(ui, list, fmt::format("{} effect", spec ? spec->label : e->type.c_str()));
        text(ui, list, "On", owner ? owner->name : "Master audio");
        if (spec) {
            std::vector<PropSpec> specs = spec->props;
            props(env, list, id, specs, false);
        }
        ui.popId();
    }

    void track(ViewEnv& env, ListLayout& list, const Track& trRef) {
        Context& ui = env.ui;
        Shell& shell = env.shell;
        const Track t = trRef;
        Id id = t.id;
        ui.pushId(id);
        section(ui, list, fmt::format("{} track", t.kind == TrackKind::Video ? "Video" : "Audio"));
        std::string name = t.name;
        if (textField(ui, field(ui, list, "Name"), "##name", name).committed)
            shell.edit("Rename track", [&](Project& pr) { edit::setTrack(pr, id, {.name = name}); });
        auto flag = [&](const char* label, bool value, auto make) {
            bool v = value;
            Rect r = field(ui, list, "");
            if (toggle(ui, r, label, v)) shell.edit(fmt::format("Set {}", label), [&](Project& pr) { edit::setTrack(pr, id, make(v)); });
        };
        if (t.kind == TrackKind::Audio) flag("Muted", t.muted, [](bool v) { return edit::TrackFlags{.muted = v}; });
        else flag("Hidden", t.hidden, [](bool v) { return edit::TrackFlags{.hidden = v}; });
        flag("Locked", t.locked, [](bool v) { return edit::TrackFlags{.locked = v}; });
        int clips = 0;
        for (const Clip& c : env.session.project().clips) clips += c.track == id;
        text(ui, list, "Clips", std::to_string(clips));
        ui.popId();
    }

    void media(ViewEnv& env, ListLayout& list, const MediaSource& m) {
        Context& ui = env.ui;
        section(ui, list, "Media");
        text(ui, list, "Name", m.name);
        text(ui, list, "Path", m.path);
        text(ui, list, "Kind", mediaKindName(m.kind));
        text(ui, list, "Duration", formatTimecode(m.duration, m.fps > 0 ? m.fps : 30));
        if (m.hasVideo) {
            text(ui, list, "Size", fmt::format("{} x {}", m.width, m.height));
            text(ui, list, "Frame rate", formatNumber(m.fps, 3));
        }
        if (m.hasAudio) text(ui, list, "Audio", fmt::format("{} Hz, {} channels", m.sampleRate, m.channels));
        std::string err = env.session.cache().error(m.path);
        if (!err.empty()) {
            Rect r = field(ui, list, "Error");
            ui.painter.textIn(r.inset(ui.dp(6), 0), err, ui.col(Slot::Error));
        }
    }

    void project(ViewEnv& env, ListLayout& list) {
        Context& ui = env.ui;
        Session& s = env.session;
        Shell& shell = env.shell;
        const ProjectSettings ps = s.project().settings;
        section(ui, list, "Project");
        auto setting = [&](const char* label, const char* id, double value, NumberStyle style, auto set) {
            editNumber(env, field(ui, list, label), id, value, style, fmt::format("Set {}", label), [&](Project& pr, double v) {
                ProjectSettings next = pr.settings;
                set(next, v);
                edit::setSettings(pr, next);
            });
        };
        setting("Width", "##width", ps.width, {.step = 2, .min = 16, .max = 8192, .decimals = 0, .unit = "px"},
                [](ProjectSettings& n, double v) { n.width = int(v) & ~1; });
        setting("Height", "##height", ps.height, {.step = 2, .min = 16, .max = 8192, .decimals = 0, .unit = "px"},
                [](ProjectSettings& n, double v) { n.height = int(v) & ~1; });
        setting("Frame rate", "##fps", ps.fps, {.step = 0.1, .min = 1, .max = 240, .decimals = 3, .unit = "fps"},
                [](ProjectSettings& n, double v) { n.fps = v; });
        setting("Sample rate", "##rate", ps.sampleRate, {.step = 100, .min = 8000, .max = 192000, .decimals = 0, .unit = "Hz"},
                [](ProjectSettings& n, double v) { n.sampleRate = int(v); });
        std::string bg = formatColor(ps.background);
        Rect r = field(ui, list, "Background");
        Rect swatch = cutLeft(r, r.h);
        ui.painter.fill(swatch, ps.background);
        ui.painter.outline(swatch, ui.col(Slot::Border));
        cutLeft(r, ui.dp(4));
        if (textField(ui, r, "##background", bg, FontRole::Mono).committed) {
            if (auto c = parseColor(bg)) {
                ProjectSettings next = s.project().settings;
                next.background = *c;
                shell.edit("Set background", [&](Project& pr) { edit::setSettings(pr, next); });
            } else shell.notify(fmt::format("Cannot parse color \"{}\"", bg), true);
        }
        const Project& p = s.evaluator().project();
        text(ui, list, "Duration", formatTimecode(p.duration(), ps.fps));
        text(ui, list, "Contents", fmt::format("{} tracks, {} clips, {} media", p.tracks.size(), p.clips.size(), p.media.size()));
        Rect hint = list.next(rowH_ * 2);
        paragraph(ui, hint.inset(ui.dp(8), ui.dp(4)), "Select a clip, transition, track, or media item\nto edit it here.", Slot::TextFaint);
    }

    float labelW_ = 110, rowH_ = 24, contentH_ = 0;
};

}  // namespace

std::unique_ptr<View> inspector() { return std::make_unique<InspectorView>(); }

}  // namespace mf::ui::views
