#include "ui/views/common.hpp"
#include "ui/views/views.hpp"

#include <fmt/format.h>

namespace mf::ui::views {

namespace {

bool contains(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); });
    return it != hay.end();
}

std::string chordList(const std::vector<KeyChord>& chords) {
    std::string out;
    for (const auto& c : chords) out += (out.empty() ? "" : ", ") + formatChord(c);
    return out;
}

// Interface fonts and scale, every theme color, and every key binding.
// Changes apply at once and are saved to the settings file.
class SettingsView : public View {
public:
    void header(ViewEnv& env, const Rect& area) override {
        Rect r = area;
        textField(env.ui, cutRight(r, std::min(r.w, env.ui.dp(180))), "##filter", filter_, FontRole::Ui, "Filter");
    }

    void draw(ViewEnv& env, const Rect& body) override {
        Context& ui = env.ui;
        ListLayout list(ui, body, "##settings", contentH_);
        float top = list.next(0).y;
        labelW_ = std::min(ui.dp(170), body.w * 0.4f);
        rowH_ = ui.rowHeight() + 2;
        drawInterface(env, list);
        drawColors(env, list);
        drawBindings(env, list);
        contentH_ = list.next(0).y - top;
    }

private:
    // A section title with an optional action button on the right. Returns true on click.
    bool section(Context& ui, ListLayout& list, std::string_view title, std::string_view action, bool enabled) {
        Rect r = list.next(ui.rowHeight());
        ui.painter.fill(list.next(1), ui.col(Slot::Border));
        ui.painter.fill(r, ui.col(Slot::Panel));
        bool clicked = false;
        if (!action.empty()) {
            Rect b = r;
            clicked = button(ui, cutRight(b, std::min(b.w, ui.dp(110))), action, false, enabled);
            r = b;
        }
        ui.painter.textIn(r.inset(ui.dp(8), 0), title, ui.col(Slot::Text));
        return clicked;
    }

    void drawInterface(ViewEnv& env, ListLayout& list) {
        Context& ui = env.ui;
        Shell& shell = env.shell;
        Settings& st = shell.settings();
        const Settings defaults;
        bool changed = st.uiSize != defaults.uiSize || st.monoSize != defaults.monoSize || st.uiScale != defaults.uiScale ||
                       st.uiFont != defaults.uiFont || st.monoFont != defaults.monoFont;
        if (section(ui, list, "Interface", "Reset interface", changed)) {
            st.uiSize = defaults.uiSize;
            st.monoSize = defaults.monoSize;
            st.uiScale = defaults.uiScale;
            st.uiFont = defaults.uiFont;
            st.monoFont = defaults.monoFont;
            shell.settingsChanged();
        }
        auto number = [&](const char* label, float& v, const NumberStyle& style) {
            if (!contains(label, filter_)) return;
            Rect row = list.next(rowH_);
            if (!list.visible(row)) return;
            double d = v;
            DragResult res = numberField(ui, labeled(ui, row, label, labelW_), fmt::format("##{}", label), d, style);
            if ((res.changed || res.committed) && float(d) != v) {
                v = float(d);
                shell.settingsChanged();
            }
        };
        number("Interface font size", st.uiSize, NumberStyle{1, 8, 32, 0, "px"});
        number("Code font size", st.monoSize, NumberStyle{1, 8, 32, 0, "px"});
        number("Scale", st.uiScale, NumberStyle{0.05, 0.5, 3, 2, "x"});
        auto file = [&](const char* label, std::string& path, const char* placeholder) {
            if (!contains(label, filter_)) return;
            Rect row = list.next(rowH_);
            if (!list.visible(row)) return;
            std::string text = path;
            if (textField(ui, labeled(ui, row, label, labelW_), fmt::format("##{}", label), text, FontRole::Mono, placeholder).committed &&
                text != path) {
                path = text;
                shell.settingsChanged();
            }
        };
        file("Interface font file", st.uiFont, "System sans");
        file("Code font file", st.monoFont, "System monospace");
        if (contains("Settings file", filter_)) {
            Rect row = list.next(rowH_);
            std::string path = shell.settingsPath().empty() ? "Not saved" : shell.settingsPath();
            ui.painter.textIn(labeled(ui, row, "Settings file", labelW_).inset(ui.dp(4), 0), path, ui.col(Slot::TextDim), Align::Left,
                              FontRole::Mono);
        }
    }

    void drawColors(ViewEnv& env, ListLayout& list) {
        Context& ui = env.ui;
        Shell& shell = env.shell;
        Theme& theme = shell.settings().theme;
        if (section(ui, list, "Colors", "Reset colors", theme != Theme{})) {
            theme = Theme{};
            shell.settingsChanged();
        }
        for (int i = 0; i < kSlotCount; ++i) {
            Slot slot = Slot(i);
            if (!contains(slotName(slot), filter_)) continue;
            Rect row = list.next(rowH_);
            if (!list.visible(row)) continue;
            ui.pushId(uint64_t(i));
            Color& c = theme[slot];
            Rect r = row;
            Rect label = cutLeft(r, labelW_);
            ui.painter.textIn(label.inset(ui.dp(8), 0), slotName(slot), ui.col(Slot::TextDim), Align::Left, FontRole::Mono);
            Rect swatch = cutLeft(r, r.h).inset(ui.dp(3), ui.dp(3));
            ui.painter.fill(swatch, c);
            ui.painter.outline(swatch, ui.col(Slot::Border));
            std::string hex = formatColor(c);
            if (textField(ui, cutLeft(r, std::min(r.w, ui.dp(84))), "##hex", hex, FontRole::Mono).committed) {
                if (auto parsed = parseColor(hex)) {
                    c = *parsed;
                    shell.settingsChanged();
                } else shell.notify(fmt::format("Cannot parse color \"{}\"", hex), true);
            }
            float resetW = std::min(r.w, ui.dp(26));
            float channelW = std::clamp((r.w - resetW) / 3, 0.0f, ui.dp(64));
            float* channels[] = {&c.r, &c.g, &c.b};
            const char* ids[] = {"##r", "##g", "##b"};
            for (int k = 0; k < 3; ++k) {
                double v = std::round(*channels[k] * 255);
                DragResult res = numberField(ui, cutLeft(r, channelW), ids[k], v, NumberStyle{1, 0, 255, 0, ""});
                if ((res.changed || res.committed) && float(v / 255) != *channels[k]) {
                    *channels[k] = float(v / 255);
                    shell.settingsChanged();
                }
            }
            bool reset = button(ui, cutLeft(r, resetW), "x##reset", false, c != defaultColor(slot));
            ui.tooltip("Reset to the theme color");
            if (reset) {
                c = defaultColor(slot);
                shell.settingsChanged();
            }
            ui.popId();
        }
    }

    void drawBindings(ViewEnv& env, ListLayout& list) {
        Context& ui = env.ui;
        Shell& shell = env.shell;
        Actions& actions = shell.actions();
        bool custom = false;
        for (const Action& a : actions.all()) custom |= actions.bindings(a.id) != a.defaults;
        if (section(ui, list, "Key bindings", "Reset bindings", custom)) {
            shell.cancelCapture();
            actions.resetAll();
            shell.settings().bindings = actions.overridesJson();
            shell.settingsChanged();
        }
        Rect hint = list.next(rowH_);
        const auto& capture = shell.capture();
        ui.painter.textIn(hint.inset(ui.dp(8), 0),
                          capture ? "Press the new key, or Escape to cancel." : "Click a key to change it, or + to add one.",
                          ui.col(capture ? Slot::Text : Slot::TextFaint));

        std::vector<std::string> groups;
        for (const Action& a : actions.all())
            if (std::find(groups.begin(), groups.end(), a.group) == groups.end()) groups.push_back(a.group);
        for (const std::string& group : groups) {
            bool headed = false;
            for (const Action& a : actions.all()) {
                if (a.group != group) continue;
                std::vector<KeyChord> chords = actions.bindings(a.id);  // a copy: edits replace the list
                if (!contains(a.label, filter_) && !contains(a.id, filter_) && !contains(group, filter_) &&
                    !contains(chordList(chords), filter_))
                    continue;
                if (!headed) {
                    headed = true;
                    Rect g = list.next(ui.rowHeight());
                    ui.painter.textIn(g.inset(ui.dp(8), 0), group, ui.col(Slot::TextDim));
                }
                Rect row = list.next(rowH_);
                if (!list.visible(row)) continue;
                ui.pushId(a.id);
                drawBinding(env, row, a, chords);
                ui.popId();
            }
        }
    }

    void drawBinding(ViewEnv& env, Rect r, const Action& a, const std::vector<KeyChord>& chords) {
        Context& ui = env.ui;
        Shell& shell = env.shell;
        Actions& actions = shell.actions();
        const auto& capture = shell.capture();
        auto capturing = [&](int index) { return capture && capture->action == a.id && capture->index == index; };

        Rect label = cutLeft(r, labelW_);
        ui.painter.textIn(label.inset(ui.dp(8), 0), a.label, ui.col(Slot::Text));
        bool reset = button(ui, cutRight(r, std::min(r.w, ui.dp(56))), "Reset", false, chords != a.defaults);
        ui.tooltip(chords != a.defaults ? "Restore the default keys" : "These are the default keys");
        if (reset) {
            shell.cancelCapture();
            actions.resetBindings(a.id);
            shell.settings().bindings = actions.overridesJson();
            shell.settingsChanged();
        }
        float gap = ui.dp(4);
        for (size_t i = 0; i < chords.size(); ++i) {
            std::string text = capturing(int(i)) ? "Press a key" : formatChord(chords[i]);
            float w = ui.painter.measure(text) + ui.dp(16);
            if (w + ui.dp(22) > r.w) break;
            ui.pushId(uint64_t(i));
            bool change = button(ui, cutLeft(r, w), text + "##chord", capturing(int(i)));
            ui.tooltip("Click, then press a new key. Escape cancels.");
            if (change) shell.captureBinding(a.id, int(i));
            bool remove = button(ui, cutLeft(r, ui.dp(22)), "x##remove");
            ui.tooltip("Remove this key");
            if (remove) {
                std::vector<KeyChord> rest = chords;
                rest.erase(rest.begin() + long(i));
                shell.cancelCapture();
                actions.setBindings(a.id, std::move(rest));
                shell.settings().bindings = actions.overridesJson();
                shell.settingsChanged();
            }
            cutLeft(r, gap);
            ui.popId();
        }
        std::string add = capturing(-1) ? "Press a key" : "+";
        float w = capturing(-1) ? ui.painter.measure(add) + ui.dp(16) : ui.dp(26);
        if (w > r.w) return;
        bool addKey = button(ui, cutLeft(r, w), add + "##add", capturing(-1));
        ui.tooltip("Add a key. Click, then press it.");
        if (addKey) shell.captureBinding(a.id, -1);
    }

    std::string filter_;
    float contentH_ = 0;
    float labelW_ = 0, rowH_ = 0;
};

}  // namespace

std::unique_ptr<View> settings() { return std::make_unique<SettingsView>(); }

}  // namespace mf::ui::views
