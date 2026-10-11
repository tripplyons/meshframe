#include "ui/views/common.hpp"
#include "ui/views/views.hpp"

#include <fmt/format.h>

namespace mf::ui::views {

namespace {

constexpr float kFloorDb = -60;
constexpr double kHoldSeconds = 1.5;

float toDb(float linear) { return linear > 0 ? std::max(kFloorDb, 20 * std::log10(linear)) : kFloorDb; }

// Stereo output levels: RMS bars, peak lines, and a peak hold, in dBFS.
class MetersView : public View {
public:
    void draw(ViewEnv& env, const Rect& body) override {
        Context& ui = env.ui;
        Session& s = env.session;
        bool playing = s.player().playing();
        const AudioLevels& lv = s.player().levels();
        double now = ui.now();

        Rect area = body.inset(ui.dp(10));
        Rect scale = cutLeft(area, ui.dp(34));
        cutBottom(area, ui.rowHeight());  // channel labels
        float barW = std::min(ui.dp(28), (area.w - ui.dp(8)) / 2);
        if (area.empty() || barW <= 0) return;

        auto yOf = [&](float db) { return area.b() - (db - kFloorDb) / -kFloorDb * area.h; };
        for (float db : {0.0f, -6.0f, -12.0f, -18.0f, -24.0f, -36.0f, -48.0f}) {
            float y = std::round(yOf(db));
            ui.painter.textIn({scale.x, y - ui.rowHeight() / 2, scale.w - ui.dp(4), ui.rowHeight()}, fmt::format("{}", int(db)),
                              ui.col(Slot::TextFaint), Align::Right);
            ui.painter.hline(area.x, area.x + 2 * barW + ui.dp(8), y, ui.col(Slot::Grid));
        }

        std::string text;
        for (int ch = 0; ch < 2; ++ch) {
            float peak = playing ? toDb(lv.peak[ch]) : kFloorDb;
            float rms = playing ? toDb(lv.rms[ch]) : kFloorDb;
            if (peak >= hold_[ch] || now - holdAt_[ch] > kHoldSeconds) {
                hold_[ch] = peak;
                holdAt_[ch] = now;
            }
            Rect bar{area.x + ch * (barW + ui.dp(8)), area.y, barW, area.h};
            ui.painter.fill(bar, ui.col(Slot::Field));
            float yr = yOf(rms), yp = yOf(peak), yh = yOf(hold_[ch]);
            ui.painter.fill({bar.x, yr, bar.w, bar.b() - yr}, ui.col(Slot::Meter));
            if (peak > kFloorDb) ui.painter.hline(bar.x, bar.r(), std::round(yp), ui.col(Slot::MeterPeak));
            if (hold_[ch] > kFloorDb) ui.painter.fill({bar.x, std::round(yh) - 1, bar.w, 2}, ui.col(hold_[ch] >= -0.1f ? Slot::Error : Slot::MeterPeak));
            ui.painter.textIn({bar.x, area.b(), bar.w, ui.rowHeight()}, ch == 0 ? "L" : "R", ui.col(Slot::TextDim), Align::Center);
            text += fmt::format("{}{}", ch ? "  " : "", hold_[ch] > kFloorDb ? fmt::format("{:.1f}", hold_[ch]) : "-inf");
        }
        ui.painter.textIn({area.x + 2 * barW + ui.dp(16), area.y, std::max(0.0f, area.w - 2 * barW - ui.dp(16)), ui.rowHeight()},
                          text + " dB", ui.col(Slot::TextDim), Align::Left, FontRole::Mono);
        if (!playing && (hold_[0] > kFloorDb || hold_[1] > kFloorDb)) ui.requestFrame();
    }

private:
    float hold_[2] = {kFloorDb, kFloorDb};
    double holdAt_[2] = {0, 0};
};

}  // namespace

std::unique_ptr<View> meters() { return std::make_unique<MetersView>(); }

}  // namespace mf::ui::views
