#include "ui/views/common.hpp"
#include "ui/views/views.hpp"

#include <fmt/format.h>

#include <filesystem>

namespace mf::ui::views {

namespace {

// The session log, a Lua prompt, and CLI commands after ":".
class ConsoleView : public View {
public:
    void header(ViewEnv& env, const Rect& area) override {
        Rect r = area;
        bool clear = button(env.ui, cutRight(r, std::min(r.w, env.ui.dp(56))), "Clear");
        env.ui.tooltip("Hide the log lines shown so far");
        if (clear) clearedAt_ = env.session.logCount();
    }

    void draw(ViewEnv& env, const Rect& body) override {
        Context& ui = env.ui;
        Session& s = env.session;
        Rect area = body;
        Rect input = cutBottom(area, ui.rowHeight());
        ui.painter.fill(cutBottom(area, 1), ui.col(Slot::Border));

        const auto& lines = s.logLines();
        uint64_t count = s.logCount();
        // logLines keeps the newest entries; skip the ones before the last Clear.
        size_t first = 0;
        if (clearedAt_ > 0) {
            uint64_t dropped = count - lines.size();
            first = clearedAt_ > dropped ? size_t(std::min<uint64_t>(clearedAt_ - dropped, lines.size())) : 0;
        }
        float lh = ui.painter.lineHeight(FontRole::Mono);
        float contentH = float(lines.size() - first) * lh + ui.dp(8);
        if (count != seen_) {
            seen_ = count;
            scrollTo(ui, "##log", contentH, 0, area.h);
        }
        {
            ListLayout list(ui, area, "##log", contentH);
            list.next(ui.dp(4));
            for (size_t i = first; i < lines.size(); ++i) {
                Rect r = list.next(lh);
                if (!list.visible(r)) continue;
                const LogLine& l = lines[i];
                Slot c = l.level == LogLine::Level::Error ? Slot::Error : l.level == LogLine::Level::Warn ? Slot::Warning : Slot::Text;
                ui.painter.textIn(r.inset(ui.dp(8), 0), l.text, ui.col(c), Align::Left, FontRole::Mono);
            }
        }

        ui.painter.fill(input, ui.col(Slot::Field));
        Rect prompt = cutLeft(input, ui.dp(20));
        ui.painter.textIn(prompt, ">", ui.col(Slot::TextDim), Align::Center, FontRole::Mono);
        FieldResult f = textField(ui, input, "##input", input_, FontRole::Mono, "Lua expression, or :command args");
        if (f.committed && !input_.empty()) {
            run(env, input_);
            input_.clear();
            // Keep typing in the prompt after Enter.
            ui.focus(ui.id("##input"));
        }
    }

private:
    void run(ViewEnv& env, const std::string& line) {
        Session& s = env.session;
        s.log(LogLine::Level::Info, "> " + line);
        if (line.starts_with(":")) {
            Commands* commands = env.shell.commands();
            if (!commands) return s.log(LogLine::Level::Error, "commands are not available");
            std::vector<std::string> words = Commands::splitWords(std::string_view(line).substr(1));
            if (words.empty()) return;
            json request;
            try {
                request = Commands::requestFromWords(words, std::filesystem::current_path().string());
            } catch (const std::exception& e) {
                return s.log(LogLine::Level::Error, e.what());
            }
            Session* sp = &s;
            commands->run(request, [sp](json reply) {
                bool ok = reply.value("ok", false);
                std::string text = ok ? reply["result"].dump(2) : reply.value("error", std::string("failed"));
                size_t start = 0;
                while (start <= text.size()) {
                    size_t end = text.find('\n', start);
                    if (end == std::string::npos) end = text.size();
                    sp->log(ok ? LogLine::Level::Info : LogLine::Level::Error, text.substr(start, end - start));
                    start = end + 1;
                }
            });
            return;
        }
        std::string err;
        Value v = s.evaluator().eval(line, &err);
        if (!err.empty()) s.log(LogLine::Level::Error, err);
        else s.log(LogLine::Level::Info, describe(v));
    }

    std::string input_;
    uint64_t seen_ = 0;
    uint64_t clearedAt_ = 0;
};

}  // namespace

std::unique_ptr<View> console() { return std::make_unique<ConsoleView>(); }

}  // namespace mf::ui::views
