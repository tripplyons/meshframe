#include "ui/views/common.hpp"
#include "ui/views/views.hpp"

#include <fmt/format.h>

namespace mf::ui::views {

namespace {

// The video or audio effect chain of the selected clip. The audio view can
// also show the master chain.
class EffectsView : public View {
public:
    explicit EffectsView(bool audio) : audio_(audio) {}

    void header(ViewEnv& env, const Rect& area) override {
        Context& ui = env.ui;
        Rect r = area;
        WidgetId menu = ui.id("##add-effect");
        Rect add = cutRight(r, std::min(r.w, ui.dp(60)));
        Target t = target(env);
        if (button(ui, add, "Add", false, t.editable)) ui.openPopup(menu, {add.x, add.b()});
        if (audio_) {
            static const std::vector<std::string> modes = {"Selected clip", "Master"};
            int m = master_ ? 1 : 0;
            if (dropdown(ui, cutRight(r, std::min(r.w, ui.dp(120))), "##target", m, modes)) master_ = m == 1;
        }
        if (beginMenu(ui, menu, ui.dp(200))) {
            const auto& specs = audio_ ? audioEffectSpecs() : videoEffectSpecs();
            for (const auto& spec : specs)
                if (menuItem(ui, spec.label)) {
                    std::string type = spec.type;
                    Id clip = t.clip;
                    env.shell.edit(fmt::format("Add {}", spec.label),
                                   [&](Project& p) { env.session.selection.effect = edit::addEffect(p, clip, audio_, type); });
                }
            endMenu(ui);
        }
    }

    void draw(ViewEnv& env, const Rect& body) override {
        Context& ui = env.ui;
        Session& s = env.session;
        Target t = target(env);
        if (!t.message.empty()) {
            paragraph(ui, body.inset(ui.pad()), t.message, Slot::TextFaint);
            return;
        }
        Rect info = body;
        Rect title = cutTop(info, ui.rowHeight());
        ui.painter.fill(title, ui.col(Slot::Panel));
        ui.painter.textIn(title.inset(ui.dp(8), 0), t.title, ui.col(Slot::TextDim));
        ui.painter.fill(cutTop(info, 1), ui.col(Slot::Border));
        if (t.chain.empty()) {
            paragraph(ui, info.inset(ui.pad()), t.editable ? "No effects. Use Add." : "No effects.", Slot::TextFaint);
            return;
        }

        float labelW = std::min(ui.dp(110), body.w * 0.4f);
        float rowH = ui.rowHeight() + 2;
        ListLayout list(ui, info, "##chain", contentH_);
        float top = list.next(0).y;
        for (size_t i = 0; i < t.chain.size(); ++i) {
            const Effect& e = t.chain[i];
            const EffectSpec* spec = audio_ ? findAudioEffect(e.type) : findVideoEffect(e.type);
            ui.pushId(e.id);
            Rect head = list.next(ui.rowHeight());
            bool selected = s.selection.effect == e.id;
            ui.painter.fill(head, ui.col(selected ? Slot::Selection : Slot::Panel));
            Hit h = ui.interact(ui.id("head"), head, true);
            if (h.pressed) s.selection.effect = e.id;
            Rect r = head;
            Id id = e.id;
            if (t.editable) {
                bool enabled = e.enabled;
                if (toggle(ui, cutLeft(r, ui.dp(24)), "##enabled", enabled))
                    env.shell.edit(enabled ? "Enable effect" : "Disable effect", [&](Project& p) { edit::setEffectEnabled(p, id, enabled); });
                float b = ui.dp(26);
                if (button(ui, cutRight(r, b), "x##remove")) {
                    env.shell.edit("Remove effect", [&](Project& p) { edit::removeEffect(p, id); });
                    s.pruneSelection();
                }
                if (button(ui, cutRight(r, b), "v##down", false, i + 1 < t.chain.size()))
                    env.shell.edit("Move effect", [&](Project& p) { edit::moveEffect(p, id, 1); });
                if (button(ui, cutRight(r, b), "^##up", false, i > 0))
                    env.shell.edit("Move effect", [&](Project& p) { edit::moveEffect(p, id, -1); });
            }
            std::string label = spec ? spec->label : e.type;
            ui.painter.textIn(r.inset(ui.dp(8), 0), e.enabled ? label : label + " (off)", ui.col(e.enabled ? Slot::Text : Slot::TextDim));
            if (spec)
                for (const PropSpec& ps : spec->props) {
                    Rect row = list.next(rowH);
                    if (list.visible(row)) propertyRow(env, row, e.id, ps, labelW, !t.editable);
                }
            list.next(1);
            ui.popId();
        }
        contentH_ = list.next(0).y - top;
    }

private:
    struct Target {
        Id clip = 0;  // 0 = master
        bool editable = false;
        std::string title, message;
        std::vector<Effect> chain;  // a copy: edits replace the project
    };

    Target target(ViewEnv& env) const {
        Session& s = env.session;
        Target t;
        if (audio_ && master_) {
            t.editable = true;
            t.title = "Master audio";
            t.chain = s.project().masterAudio;
            return t;
        }
        const Project& p = s.evaluator().project();
        const Clip* c = p.findClip(s.selection.primaryClip());
        if (!c) {
            t.message = audio_ ? "Select a clip on an audio track,\nor choose Master above." : "Select a clip on a video track.";
            return t;
        }
        const Track* tr = p.findTrack(c->track);
        if (tr && (tr->kind == TrackKind::Audio) != audio_) {
            t.message = audio_ ? "The selected clip is on a video track.\nIts effects are in the video effects view."
                               : "The selected clip is on an audio track.\nIts effects are in the audio effects view.";
            return t;
        }
        t.clip = c->id;
        t.editable = !c->scriptOwned();
        t.title = c->scriptOwned() ? c->name + " (script, read-only)" : c->name;
        t.chain = audio_ ? c->audioEffects : c->videoEffects;
        return t;
    }

    bool audio_;
    bool master_ = false;
    float contentH_ = 0;
};

}  // namespace

std::unique_ptr<View> videoEffects() { return std::make_unique<EffectsView>(false); }
std::unique_ptr<View> audioEffects() { return std::make_unique<EffectsView>(true); }

}  // namespace mf::ui::views
