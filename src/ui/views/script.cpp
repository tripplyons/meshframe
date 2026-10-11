#include "ui/code_editor.hpp"
#include "ui/views/common.hpp"
#include "ui/views/views.hpp"

#include <fmt/format.h>

namespace mf::ui::views {

namespace {

constexpr double kAutoApplyDelay = 0.8;  // seconds after the last keystroke

// The project's Lua script. Edits apply on demand (Cmd+Enter) or after a
// pause in typing when auto-apply is on. Errors appear as line markers.
class ScriptView : public View {
public:
    void header(ViewEnv& env, const Rect& area) override {
        Context& ui = env.ui;
        Rect r = area;
        if (button(ui, cutRight(r, std::min(r.w, ui.dp(56))), "Apply", false, modified())) apply(env);
        bool autoApply = autoApply_;
        if (toggle(ui, cutRight(r, std::min(r.w, ui.dp(60))), "Auto", autoApply)) autoApply_ = autoApply;
        if (external_ && button(ui, cutRight(r, std::min(r.w, ui.dp(64))), "Reload")) {
            editor_.setText(env.session.project().script, false);
            applied_ = env.session.project().script;
            external_ = false;
        }
        std::string status = external_ ? "Changed elsewhere" : modified() ? "Modified" : "";
        ui.painter.textIn(r.inset(ui.dp(8), 0), status, ui.col(Slot::TextDim), Align::Right);
    }

    void draw(ViewEnv& env, const Rect& body) override {
        Context& ui = env.ui;
        Session& s = env.session;
        const std::string& script = s.project().script;
        if (!loaded_) {
            editor_.setText(script);
            applied_ = script;
            loaded_ = true;
        } else if (script != applied_) {
            // Changed by undo, the CLI, or another script view.
            if (!modified()) {
                editor_.setText(script, false);
                applied_ = script;
            } else external_ = true;
        }

        std::vector<CodeMarker> markers;
        std::string errorText;
        const ScriptOutput& out = s.evaluator().script();
        if (out.error) {
            markers.push_back({out.error->line, out.error->message});
            errorText = out.error->line > 0 ? fmt::format("Line {}: {}", out.error->line, out.error->message) : out.error->message;
        }
        for (const NodeError& e : s.evaluator().errors())
            if (e.line > 0 && e.key != "script") markers.push_back({e.line, e.key + ": " + e.message});

        Rect area = body;
        if (!errorText.empty()) {
            Rect bar = cutBottom(area, ui.rowHeight());
            ui.painter.fill(cutBottom(area, 1), ui.col(Slot::Border));
            ui.painter.fill(bar, ui.col(Slot::ErrorBackground));
            ui.painter.textIn(bar.inset(ui.dp(8), 0), errorText, ui.col(Slot::Error), Align::Left, FontRole::Mono);
            Hit h = ui.interact(ui.id("error"), bar);
            if (h.clicked && out.error->line > 0) editor_.gotoLine(out.error->line);
        }
        if (editor_.draw(ui, area, "##script", markers)) external_ = external_ && modified();
        if (autoApply_ && modified() && !external_ && ui.now() - editor_.lastEditTime() > kAutoApplyDelay) apply(env);
        if (autoApply_ && modified()) ui.requestFrame();
    }

    bool command(ViewEnv& env, std::string_view action) override {
        if (action != "apply") return false;
        apply(env);
        return true;
    }

private:
    bool modified() const { return editor_.text() != applied_; }

    void apply(ViewEnv& env) {
        std::string text = editor_.text();
        if (text == env.session.project().script) {
            applied_ = text;
            external_ = false;
            return;
        }
        if (env.shell.edit("Edit script", [&](Project& p) { edit::setScript(p, text); })) {
            applied_ = text;
            external_ = false;
        }
    }

    CodeEditor editor_;
    bool loaded_ = false;
    bool autoApply_ = true;
    bool external_ = false;
    std::string applied_;
};

}  // namespace

std::unique_ptr<View> script() { return std::make_unique<ScriptView>(); }

}  // namespace mf::ui::views
