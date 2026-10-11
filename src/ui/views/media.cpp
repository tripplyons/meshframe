#include "ui/views/common.hpp"
#include "ui/views/views.hpp"

#include <fmt/format.h>

namespace mf::ui::views {

namespace {

std::string mediaDetails(const MediaSource& m) {
    std::string d = fmt::format("{} {}", mediaKindName(m.kind), formatTimecode(m.duration, m.fps > 0 ? m.fps : 30));
    if (m.hasVideo) d += fmt::format("  {}x{}", m.width, m.height);
    if (m.hasVideo && m.fps > 0) d += fmt::format("  {} fps", formatNumber(m.fps, 2));
    if (m.hasAudio) d += fmt::format("  {} Hz", m.sampleRate);
    return d;
}

// The media pool. Rows drag onto the timeline; a double click adds at the playhead.
class MediaView : public View {
public:
    void header(ViewEnv& env, const Rect& area) override {
        Rect r = area;
        if (button(env.ui, cutRight(r, std::min(r.w, env.ui.dp(70))), "Import")) env.shell.importMedia();
    }

    void draw(ViewEnv& env, const Rect& body) override {
        Context& ui = env.ui;
        Session& s = env.session;
        const Project& p = s.project();
        if (p.media.empty()) {
            paragraph(ui, body.inset(ui.pad()), "No media.\nUse Import, or drop files on the window.", Slot::TextFaint);
            return;
        }
        float rowH = ui.rowHeight() * 2;
        WidgetId menu = ui.id("##media-menu");
        {
            // A copy: an edit inside the loop replaces the project.
            std::vector<MediaSource> items = p.media;
            ListLayout list(ui, body, "##media", rowH * float(items.size()));
            for (const MediaSource& m : items) {
                Rect r = list.next(rowH);
                if (!list.visible(r)) continue;
                ui.pushId(m.id);
                Hit h;
                Rect top{r.x, r.y, r.w, ui.rowHeight()};
                selectable(ui, r, "##row", s.selection.media == m.id, &h);
                ui.painter.textIn(top.inset(ui.dp(8), 0), m.name, ui.col(Slot::Text));
                std::string err = s.cache().error(m.path);
                ui.painter.textIn(Rect{r.x, r.y + ui.rowHeight(), r.w, ui.rowHeight()}.inset(ui.dp(8), 0),
                                  err.empty() ? mediaDetails(m) : err, ui.col(err.empty() ? Slot::TextDim : Slot::Error));
                if (h.pressed) s.selection.media = m.id;
                if (h.held && !ui.dragging() && std::hypot(ui.mouse().x - h.origin.x, ui.mouse().y - h.origin.y) > ui.dp(4))
                    ui.beginDrag({"media", m.id, m.name});
                if (h.doubleClicked) addAtPlayhead(env, m.id);
                if (h.rightClicked) {
                    s.selection.media = m.id;
                    ui.openPopup(menu, ui.mouse());
                }
                ui.popId();
        }
        }
        if (beginMenu(ui, menu, ui.dp(200))) {
            Id id = s.selection.media;
            if (menuItem(ui, "Add at playhead")) addAtPlayhead(env, id);
            if (menuItem(ui, "Import media...")) env.shell.importMedia();
            menuSeparator(ui);
            if (menuItem(ui, "Remove from project")) {
                env.shell.edit("Remove media", [&](Project& pr) { edit::removeMedia(pr, id); });
                s.pruneSelection();
            }
            endMenu(ui);
        }
    }

private:
    static void addAtPlayhead(ViewEnv& env, Id media) {
        double t = env.session.time();
        env.shell.edit("Add media clip", [&](Project& pr) { env.session.selection.clips = edit::addMediaClips(pr, media, t); });
    }
};

}  // namespace

std::unique_ptr<View> media() { return std::make_unique<MediaView>(); }

}  // namespace mf::ui::views
