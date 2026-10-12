#include "ui/views/common.hpp"
#include "ui/views/views.hpp"

#include <fmt/format.h>

#include <array>

namespace mf::ui::views {

namespace {

constexpr int kScopeWidth = 320;  // analysis frame width in pixels
constexpr int kLevels = 128;      // waveform rows

float luma(const uint8_t* px) { return (0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2]) / 255.0f; }

// Luma waveform and histogram of the frame at the playhead.
class ScopesView : public View {
public:
    void header(ViewEnv& env, const Rect& area) override {
        static const std::vector<std::string> modes = {"Waveform and histogram", "Waveform", "Histogram"};
        Rect r = area;
        dropdown(env.ui, cutRight(r, std::min(r.w, env.ui.dp(170))), "##mode", mode_, modes);
        env.ui.tooltip("Scopes to show for the frame at the playhead");
    }

    void draw(ViewEnv& env, const Rect& body) override {
        Context& ui = env.ui;
        analyze(env);
        if (!analyzed_) {
            ui.painter.textIn(body, "No frame", ui.col(Slot::TextFaint), Align::Center);
            return;
        }
        Rect area = body.inset(ui.dp(8));
        Rect wave = area, hist;
        if (mode_ == 0) {
            hist = cutBottom(wave, std::round(area.h * 0.4f));
            cutBottom(wave, ui.dp(8));
        } else if (mode_ == 2) {
            hist = area;
            wave = {};
        }
        if (!wave.empty()) drawWaveform(ui, wave);
        if (!hist.empty()) drawHistogram(ui, hist);
    }

private:
    void analyze(ViewEnv& env) {
        Session& s = env.session;
        double t = s.time();
        if (analyzed_ && t == time_ && s.revision() == revision_) return;
        const ProjectSettings& ps = s.project().settings;
        int w = std::min(kScopeWidth, ps.width);
        int h = std::max(1, int(std::round(double(w) * ps.height / std::max(1, ps.width))));
        auto img = s.renderFrame(t, w, h);
        time_ = t;
        revision_ = s.revision();
        analyzed_ = img && !img->empty();
        if (!analyzed_) return;

        histogram_.fill(0);
        std::vector<uint32_t> counts(size_t(img->w) * kLevels, 0);
        for (int y = 0; y < img->h; ++y)
            for (int x = 0; x < img->w; ++x) {
                float l = luma(img->at(x, y));
                histogram_[std::min(255, int(l * 255 + 0.5f))]++;
                counts[size_t(std::min(kLevels - 1, int(l * (kLevels - 1) + 0.5f))) * img->w + x]++;
            }
        // Each column holds img->h samples; scale so a few hits are visible.
        Color c = env.ui.col(Slot::Waveform);
        Image wave(img->w, kLevels);
        for (int row = 0; row < kLevels; ++row)
            for (int x = 0; x < img->w; ++x) {
                float v = std::min(1.0f, std::sqrt(float(counts[size_t(row) * img->w + x]) / float(img->h)) * 2.5f);
                uint8_t* px = wave.at(x, kLevels - 1 - row);
                px[0] = uint8_t(c.r * v * 255), px[1] = uint8_t(c.g * v * 255), px[2] = uint8_t(c.b * v * 255), px[3] = uint8_t(v * 255);
            }
        waveTexture_.update(env.ui.painter.renderer(), wave);
    }

    void drawWaveform(Context& ui, Rect r) {
        Rect labels = cutLeft(r, ui.dp(30));
        ui.painter.fill(r, ui.col(Slot::Field));
        for (int pct : {0, 25, 50, 75, 100}) {
            float y = std::round(r.b() - pct / 100.0f * r.h);
            ui.painter.hline(r.x, r.r(), std::min(y, r.b() - 1), ui.col(Slot::Grid));
            ui.painter.textIn({labels.x, y - ui.rowHeight() / 2, labels.w - ui.dp(4), ui.rowHeight()}, std::to_string(pct),
                              ui.col(Slot::TextFaint), Align::Right);
        }
        if (waveTexture_.get()) ui.painter.image(waveTexture_, r);
        ui.painter.outline(r, ui.col(Slot::Border));
    }

    void drawHistogram(Context& ui, Rect r) {
        cutLeft(r, ui.dp(30));
        ui.painter.fill(r, ui.col(Slot::Field));
        uint32_t peak = 1;
        for (uint32_t v : histogram_) peak = std::max(peak, v);
        for (int x = 0; x < int(r.w); ++x) {
            int a = int(float(x) / r.w * 256), b = std::max(a + 1, int(float(x + 1) / r.w * 256));
            uint32_t v = 0;
            for (int i = a; i < std::min(b, 256); ++i) v = std::max(v, histogram_[i]);
            float h = std::sqrt(float(v) / float(peak)) * (r.h - 2);
            if (h >= 0.5f) ui.painter.vline(r.x + x, r.b() - 1 - h, r.b() - 1, ui.col(Slot::Waveform));
        }
        ui.painter.outline(r, ui.col(Slot::Border));
    }

    int mode_ = 0;
    bool analyzed_ = false;
    double time_ = -1;
    uint64_t revision_ = 0;
    std::array<uint32_t, 256> histogram_{};
    Texture waveTexture_;
};

}  // namespace

std::unique_ptr<View> scopes() { return std::make_unique<ScopesView>(); }

}  // namespace mf::ui::views
