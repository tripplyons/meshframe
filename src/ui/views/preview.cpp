#include "ui/views/common.hpp"
#include "ui/views/views.hpp"

#include <fmt/format.h>

namespace mf::ui::views {

namespace {

// The rendered frame at the playhead, fitted to the pane, with transport controls.
class PreviewView : public View {
public:
    void header(ViewEnv& env, const Rect& area) override {
        static const std::vector<std::string> labels = {"Full", "Half", "Quarter"};
        Rect r = area;
        dropdown(env.ui, cutRight(r, std::min(r.w, env.ui.dp(90))), "##resolution", resolution_, labels);
    }

    void draw(ViewEnv& env, const Rect& body) override {
        Context& ui = env.ui;
        Session& s = env.session;
        Rect area = body;
        Rect bar = cutBottom(area, ui.rowHeight());
        ui.painter.fill(cutBottom(area, 1), ui.col(Slot::Border));
        ui.painter.fill(area, ui.col(Slot::PreviewBackground));

        const ProjectSettings& ps = s.project().settings;
        float aspect = float(ps.width) / float(std::max(1, ps.height));
        float w = area.w, h = area.w / aspect;
        if (h > area.h) h = area.h, w = area.h * aspect;
        Rect frame{std::round(area.x + (area.w - w) / 2), std::round(area.y + (area.h - h) / 2), std::round(w), std::round(h)};

        int div = 1 << resolution_;
        int rw = std::max(1, std::min(int(frame.w), ps.width / div));
        int rh = std::max(1, std::min(int(frame.h), ps.height / div));
        double t = s.time();
        if (!frame.empty() && (t != renderedTime_ || s.revision() != renderedRevision_ || rw != renderedW_ || rh != renderedH_)) {
            if (auto img = s.renderFrame(t, rw, rh)) texture_.update(ui.painter.renderer(), *img);
            renderedTime_ = t;
            renderedRevision_ = s.revision();
            renderedW_ = rw;
            renderedH_ = rh;
        }
        if (texture_.get() && !frame.empty()) ui.painter.image(texture_, frame);

        // Clicking the frame toggles playback.
        Hit fh = ui.interact(ui.id("frame"), area);
        if (fh.clicked) s.togglePlay();

        drawTransport(env, bar);
    }

    bool command(ViewEnv& env, std::string_view action) override {
        if (action != "toggle") return false;
        env.session.togglePlay();
        return true;
    }

private:
    void drawTransport(ViewEnv& env, Rect bar) {
        Context& ui = env.ui;
        Session& s = env.session;
        ui.painter.fill(bar, ui.col(Slot::Panel));
        float b = ui.dp(44);
        double fps = s.project().settings.fps;
        if (button(ui, cutLeft(bar, b), "Start")) s.seek(0);
        if (button(ui, cutLeft(bar, b), "Prev")) s.seek(std::max(0.0, std::round(s.time() * fps - 1) / fps));
        if (button(ui, cutLeft(bar, b), s.player().playing() ? "Pause" : "Play")) s.togglePlay();
        if (button(ui, cutLeft(bar, b), "Next")) s.seek(std::round(s.time() * fps + 1) / fps);
        if (button(ui, cutLeft(bar, b), "End")) s.seek(s.project().duration());
        if (button(ui, cutLeft(bar, b), "Loop", s.player().loop())) s.player().setLoop(!s.player().loop());
        Rect text = bar.inset(ui.pad(), 0);
        std::string tc = fmt::format("{} / {}", formatTimecode(s.time(), fps), formatTimecode(s.project().duration(), fps));
        ui.painter.textIn(text, tc, ui.col(Slot::Text), Align::Right, FontRole::Mono);
    }

    int resolution_ = 0;
    Texture texture_;
    double renderedTime_ = -1;
    uint64_t renderedRevision_ = 0;
    int renderedW_ = 0, renderedH_ = 0;
};

}  // namespace

std::unique_ptr<View> preview() { return std::make_unique<PreviewView>(); }

}  // namespace mf::ui::views
