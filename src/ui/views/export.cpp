#include "exporter/exporter.hpp"
#include "ui/views/common.hpp"
#include "ui/views/views.hpp"

#include <filesystem>
#include <fmt/format.h>

namespace mf::ui::views {

namespace {

// Export settings, the progress of a running export, and the last result.
class ExportView : public View {
public:
    ExportView() : chosen_(std::make_shared<std::string>()) {}

    void draw(ViewEnv& env, const Rect& body) override {
        Context& ui = env.ui;
        Session& s = env.session;
        ExportJob& job = s.exportJob();
        const ProjectSettings& ps = s.project().settings;
        if (!chosen_->empty()) path_ = std::exchange(*chosen_, {});
        if (path_.empty()) path_ = defaultPath(s);

        float labelW = std::min(ui.dp(90), body.w * 0.35f);
        float rowH = ui.rowHeight() + 2;
        ListLayout list(ui, body, "##export", rowH * 9 + ui.rowHeight() * 4);
        bool busy = job.running();

        Rect r = labeled(ui, list.next(rowH), "File", labelW);
        if (env.shell.hooks.fileDialog) {
            if (button(ui, cutRight(r, std::min(r.w, ui.dp(70))), "Browse", false, !busy)) {
                std::weak_ptr<std::string> out = chosen_;
                env.shell.hooks.fileDialog(true, "Export", {"mp4", "wav"}, [out](std::string path) {
                    if (auto o = out.lock(); o && !path.empty()) *o = std::move(path);
                });
            }
        }
        textField(ui, r, "##path", path_, FontRole::Mono, "out.mp4");

        auto intRow = [&](const char* label, int& v, int lo, int hi, const char* unit) {
            Rect f = labeled(ui, list.next(rowH), label, labelW);
            double d = v;
            DragResult res = numberField(ui, f, fmt::format("##{}", label), d, NumberStyle{1, double(lo), double(hi), 0, unit});
            if (res.changed || res.committed) v = int(std::round(d));
        };
        if (width_ <= 0) width_ = ps.width;
        if (height_ <= 0) height_ = ps.height;
        intRow("Width", width_, 16, 8192, "px");
        intRow("Height", height_, 16, 8192, "px");
        if (button(ui, labeled(ui, list.next(rowH), "", labelW), "Use project size")) width_ = ps.width, height_ = ps.height;

        double duration = s.project().duration();
        auto timeRow = [&](const char* label, double& v) {
            Rect f = labeled(ui, list.next(rowH), label, labelW);
            numberField(ui, f, fmt::format("##{}", label), v, NumberStyle{0.1, 0, std::max(duration, 0.0), 2, "s"});
        };
        if (end_ < 0) end_ = duration;
        timeRow("Start", start_);
        timeRow("End", end_);
        intRow("Quality", crf_, 0, 51, "crf");

        list.next(ui.dp(6));
        Rect actions = list.next(rowH).inset(ui.pad(), 0);
        if (busy) {
            if (button(ui, cutRight(actions, std::min(actions.w, ui.dp(80))), "Cancel")) job.cancel();
            Rect bar = actions.inset(0, ui.dp(4));
            bar.w -= ui.dp(8);
            ui.painter.fill(bar, ui.col(Slot::Field));
            Rect done = bar;
            done.w = std::round(bar.w * float(job.progress()));
            ui.painter.fill(done, ui.col(Slot::Accent));
            ui.painter.outline(bar, ui.col(Slot::Border));
            ui.painter.textIn(bar, fmt::format("{:.0f}%", job.progress() * 100), ui.col(Slot::Text), Align::Center);
        } else if (button(ui, cutLeft(actions, std::min(actions.w, ui.dp(100))), "Export", false, end_ > start_ && !path_.empty())) {
            start(env);
        }

        list.next(ui.dp(6));
        std::string status;
        Slot slot = Slot::TextDim;
        if (busy) status = "Exporting to " + job.options().path;
        else if (job.finished() && !job.error().empty()) status = job.error(), slot = Slot::Error;
        else if (job.finished()) status = "Wrote " + job.options().path;
        if (!status.empty()) paragraph(ui, list.next(ui.rowHeight() * 3).inset(ui.pad(), 0), status, slot);
        if (busy) ui.requestFrame();
    }

private:
    static std::string defaultPath(Session& s) {
        std::filesystem::path base = s.path().empty() ? std::filesystem::current_path() / s.project().name : std::filesystem::path(s.path());
        return base.replace_extension(".mp4").string();
    }

    void start(ViewEnv& env) {
        ExportOptions opts;
        opts.path = path_;
        opts.width = width_;
        opts.height = height_;
        opts.start = start_;
        opts.end = end_;
        opts.crf = crf_;
        std::string err;
        if (!env.session.exportJob().start(env.session.project(), opts, &err)) env.shell.notify(err, true);
    }

    std::shared_ptr<std::string> chosen_;  // set by the async file dialog
    std::string path_;
    int width_ = 0, height_ = 0, crf_ = 18;
    double start_ = 0, end_ = -1;
};

}  // namespace

std::unique_ptr<View> exportView() { return std::make_unique<ExportView>(); }

}  // namespace mf::ui::views
