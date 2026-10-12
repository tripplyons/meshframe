#include "ui/views/common.hpp"
#include "ui/views/views.hpp"

#include <fmt/format.h>

#include <set>

namespace mf::ui::views {

namespace {

constexpr float kDefaultZoom = 80;  // logical px per second
constexpr float kMinZoom = 2, kMaxZoom = 4000;
constexpr double kNewClipSeconds = 3;

std::string rulerLabel(double t, double step) {
    int total = int(std::floor(t + 1e-9));
    if (step < 1) return fmt::format("{}:{:02}.{}", total / 60, total % 60, int(std::round((t - total) * 10)) % 10);
    return fmt::format("{}:{:02}", total / 60, total % 60);
}

// The arranged clips over time: ruler, track headers, lanes, and transitions.
class TimelineView : public View {
public:
    void header(ViewEnv& env, const Rect& area) override {
        Context& ui = env.ui;
        Rect r = area;
        Shell& shell = env.shell;
        float b = ui.dp(44);
        bool snap = button(ui, cutRight(r, b), "Snap", snap_);
        ui.tooltip("Snap to clip edges and the playhead. Hold Option while dragging to skip snapping.");
        if (snap) snap_ = !snap_;
        bool fitAll = button(ui, cutRight(r, b), "Fit");
        ui.tooltip(shell.withShortcut("Fit the project in view", "timeline.zoom-fit"));
        if (fitAll) fit(env);
        bool in = button(ui, cutRight(r, ui.dp(26)), "+");
        ui.tooltip(shell.withShortcut("Zoom in", "timeline.zoom-in"));
        if (in) zoomAround(zoom_ * 1.5f, lanes_.x + lanes_.w / 2);
        bool out = button(ui, cutRight(r, ui.dp(26)), "-");
        ui.tooltip(shell.withShortcut("Zoom out", "timeline.zoom-out"));
        if (out) zoomAround(zoom_ / 1.5f, lanes_.x + lanes_.w / 2);
    }

    bool command(ViewEnv& env, std::string_view action) override {
        if (action == "zoom-in") zoomAround(zoom_ * 1.5f, playheadX(env));
        else if (action == "zoom-out") zoomAround(zoom_ / 1.5f, playheadX(env));
        else if (action == "zoom-fit") fit(env);
        else return false;
        return true;
    }

    void draw(ViewEnv& env, const Rect& body) override {
        Context& ui = env.ui;
        Session& s = env.session;
        scale_ = ui.scale();
        const Project& p = s.evaluator().project();

        Rect area = body;
        Rect rulerRow = cutTop(area, ui.rowHeight());
        ui.painter.fill(cutTop(area, 1), ui.col(Slot::Border));
        Rect headers = cutLeft(area, std::min(area.w, ui.dp(120)));
        ui.painter.fill(cutLeft(area, 1), ui.col(Slot::Border));
        lanes_ = area;
        Rect rulerCorner = cutLeft(rulerRow, headers.w + 1);
        ruler_ = rulerRow;
        laneH_ = ui.dp(44);

        ui.painter.fill(rulerCorner, ui.col(Slot::Panel));
        ui.painter.textIn(rulerCorner.inset(ui.dp(8), 0), formatTimecode(s.time(), p.settings.fps), ui.col(Slot::TextDim),
                          Align::Left, FontRole::Mono);
        ui.painter.fill(headers, ui.col(Slot::Panel));
        ui.painter.fill(lanes_, ui.col(Slot::Background));

        // Scrolling and zoom over the lanes and ruler.
        float contentH = float(p.tracks.size()) * laneH_;
        Rect wheelArea{lanes_.x, ruler_.y, lanes_.w, lanes_.b() - ruler_.y};
        Vec w = ui.takeWheel(wheelArea);
        if (w.x != 0 || w.y != 0) {
            if (ui.input().mods & ModGui) {
                zoomAround(zoom_ * std::pow(1.15f, w.y), ui.mouse().x);
            } else {
                float dx = w.x, dy = w.y;
                if (ui.input().mods & ModShift) dx = dy, dy = 0;
                scrollT_ = std::max(0.0, scrollT_ - dx * ui.dp(40) / pps());
                scrollY_ -= dy * ui.dp(40);
            }
        }
        if (Vec hw = ui.takeWheel(headers); hw.y != 0) scrollY_ -= hw.y * ui.dp(40);
        scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, contentH - lanes_.h));
        scrollY_ = std::round(scrollY_);
        followPlayhead(s);

        drawRuler(env);
        drawTrackHeaders(env, headers);
        ui.painter.pushClip(lanes_);
        drawLanes(env);
        ui.painter.popClip();
        drawPlayhead(env);
        drawMenus(env);
    }

private:
    enum class DragMode { None, Move, TrimStart, TrimEnd };

    float pps() const { return zoom_ * scale_; }
    float xAt(double t) const { return lanes_.x + float((t - scrollT_) * pps()); }
    double timeAt(float x) const { return scrollT_ + (x - lanes_.x) / pps(); }
    float playheadX(ViewEnv& env) const { return xAt(env.session.time()); }

    void zoomAround(float zoom, float x) {
        double t = timeAt(x);
        zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
        scrollT_ = std::max(0.0, t - (x - lanes_.x) / pps());
    }

    void fit(ViewEnv& env) {
        double d = std::max(1.0, env.session.evaluator().project().duration());
        if (lanes_.w > 0) zoom_ = std::clamp(float(lanes_.w * 0.95 / d / scale_), kMinZoom, kMaxZoom);
        scrollT_ = 0;
    }

    void followPlayhead(Session& s) {
        if (!s.player().playing() || lanes_.w <= 0) return;
        float x = xAt(s.time());
        if (x > lanes_.r() - lanes_.w * 0.05f || x < lanes_.x) scrollT_ = std::max(0.0, s.time() - lanes_.w * 0.05 / pps());
    }

    // Tracks draw top to bottom from the highest index.
    int laneCount(const Project& p) const { return int(p.tracks.size()); }
    int trackIndexForLane(const Project& p, int lane) const { return laneCount(p) - 1 - lane; }
    float laneY(int lane) const { return lanes_.y - scrollY_ + lane * laneH_; }
    int laneAt(const Project& p, float y) const {
        int lane = int(std::floor((y - lanes_.y + scrollY_) / laneH_));
        return lane >= 0 && lane < laneCount(p) ? lane : -1;
    }
    int laneOf(const Project& p, Id track) const {
        int i = p.trackIndex(track);
        return i < 0 ? -1 : laneCount(p) - 1 - i;
    }

    void drawRuler(ViewEnv& env) {
        Context& ui = env.ui;
        Session& s = env.session;
        ui.painter.fill(ruler_, ui.col(Slot::Ruler));
        static const double steps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200, 3600};
        double step = steps[std::size(steps) - 1];
        for (double st : steps)
            if (st * pps() >= ui.dp(70)) {
                step = st;
                break;
            }
        majorStep_ = step;
        double minor = step / 5;
        ui.painter.pushClip(ruler_);
        double t0 = std::floor(scrollT_ / minor) * minor;
        for (double t = t0; xAt(t) < ruler_.r(); t += minor) {
            float x = std::round(xAt(t));
            bool major = std::abs(std::remainder(t, step)) < minor * 0.01;
            float h = major ? ruler_.h * 0.45f : ruler_.h * 0.2f;
            ui.painter.vline(x, ruler_.b() - h, ruler_.b(), ui.col(major ? Slot::TextDim : Slot::TextFaint));
            if (major)
                ui.painter.text(x + ui.dp(4), ruler_.y, rulerLabel(t, step), ui.col(Slot::TextDim));
        }
        ui.painter.popClip();
        Hit h = ui.interact(ui.id("ruler"), ruler_);
        if (h.held) {
            s.seek(std::max(0.0, timeAt(ui.mouse().x)));
            ui.setCursor(Cursor::ResizeH);
        }
    }

    void drawTrackHeaders(ViewEnv& env, const Rect& headers) {
        Context& ui = env.ui;
        Session& s = env.session;
        const Project& p = s.project();
        ui.painter.pushClip(headers);
        for (int lane = 0; lane < laneCount(p); ++lane) {
            const Track t = p.tracks[trackIndexForLane(p, lane)];  // a copy: the flag buttons edit the project
            Rect r{headers.x, laneY(lane), headers.w, laneH_};
            if (r.b() < headers.y || r.y > headers.b()) continue;
            ui.pushId(t.id);
            Rect row = r;
            ui.painter.hline(row.x, row.r(), row.b() - 1, ui.col(Slot::Border));
            row.h -= 1;
            Hit h = ui.interact(ui.id("header"), row, true);
            if (s.selection.track == t.id) ui.painter.fill(row, ui.col(Slot::Selection));
            else if (h.hovered) ui.painter.fill(row, ui.col(Slot::Hover));
            if (h.pressed) s.selection.track = t.id;
            if (h.rightClicked) {
                menuTrack_ = t.id;
                ui.openPopup(ui.id("##track-menu"), ui.mouse());
            }
            Rect top = cutTop(row, ui.rowHeight());
            ui.painter.textIn(top.inset(ui.dp(8), 0), t.name, ui.col(Slot::Text));
            Rect flags = row.inset(ui.dp(4), ui.dp(2));
            float fb = ui.dp(22);
            Id id = t.id;
            auto flag = [&](const char* label, bool on, const char* tip, auto set) {
                bool clicked = button(ui, cutLeft(flags, fb), label, on);
                ui.tooltip(tip);
                if (clicked)
                    env.shell.edit(fmt::format("{} {}", on ? "Clear" : "Set", label), [&](Project& pr) { edit::setTrack(pr, id, set(!on)); });
            };
            if (t.kind == TrackKind::Audio)
                flag("M", t.muted, t.muted ? "Unmute track" : "Mute track", [](bool v) { return edit::TrackFlags{.muted = v}; });
            else flag("H", t.hidden, t.hidden ? "Show track" : "Hide track", [](bool v) { return edit::TrackFlags{.hidden = v}; });
            flag("L", t.locked, t.locked ? "Unlock track" : "Lock track", [](bool v) { return edit::TrackFlags{.locked = v}; });
            ui.popId();
        }
        ui.painter.popClip();
    }

    struct Placed {
        Clip clip;  // a copy: edits during the loop replace the evaluated project
        Rect rect;
    };

    void drawLanes(ViewEnv& env) {
        Context& ui = env.ui;
        Session& s = env.session;
        const Project& p = s.evaluator().project();

        // Lane backgrounds and the end of the project.
        for (int lane = 0; lane < laneCount(p); ++lane) {
            float y = laneY(lane);
            ui.painter.hline(lanes_.x, lanes_.r(), y + laneH_ - 1, ui.col(Slot::Grid));
        }
        float endX = xAt(p.duration());
        if (endX < lanes_.r()) ui.painter.fill({endX, lanes_.y, lanes_.r() - endX, lanes_.h}, Color::gray(0, 0.25f));
        double t0 = std::floor(scrollT_ / majorStep_) * majorStep_;
        for (double t = t0; xAt(t) < lanes_.r(); t += majorStep_) ui.painter.vline(std::round(xAt(t)), lanes_.y, lanes_.b(), ui.col(Slot::Grid));

        // Empty lane area: deselect, context menu, and media drops.
        Hit bg = ui.interact(ui.id("lanes"), lanes_, true);
        if (bg.pressed) {
            s.selection.clips.clear();
            s.selection.transition = 0;
            s.selection.effect = 0;
            int lane = laneAt(p, ui.mouse().y);
            if (lane >= 0) s.selection.track = p.tracks[trackIndexForLane(p, lane)].id;
        }
        if (bg.rightClicked) {
            int lane = laneAt(p, ui.mouse().y);
            if (lane >= 0) {
                menuTrack_ = p.tracks[trackIndexForLane(p, lane)].id;
                menuTime_ = std::max(0.0, timeAt(ui.mouse().x));
                ui.openPopup(ui.id("##lane-menu"), ui.mouse());
            }
        }

        std::vector<Placed> placed;
        for (const Clip& c : p.clips) {
            int lane = laneOf(p, c.track);
            if (lane < 0) continue;
            Rect r{xAt(c.start), laneY(lane) + 1, float(c.duration * pps()), laneH_ - 3};
            r.x = std::round(r.x);
            r.w = std::max(1.0f, std::round(r.w));
            if (r.r() < lanes_.x || r.x > lanes_.r() || r.b() < lanes_.y || r.y > lanes_.b()) continue;
            placed.push_back({c, r});
        }
        for (const Placed& pl : placed) drawClip(env, p, pl.clip, pl.rect);
        drawTransitions(env, p);
        drawDragFeedback(env);
        drawDrop(env, p);
    }

    // While a clip drag runs: a guide where it snapped, and a readout of
    // where the clip is, with the reason when it cannot go further.
    void drawDragFeedback(ViewEnv& env) {
        Context& ui = env.ui;
        Session& s = env.session;
        if (!drag_.started || drag_.mode == DragMode::None) return;
        if (drag_.snap) ui.painter.vline(std::round(xAt(*drag_.snap)), lanes_.y, lanes_.b(), ui.col(Slot::Accent));
        const Clip* c = s.project().findClip(drag_.clip);
        if (!c) return;
        double fps = s.project().settings.fps;
        std::string text;
        if (drag_.mode == DragMode::Move) {
            double delta = c->start - drag_.origStart;
            text = fmt::format("Start {}   {}{} s", formatTimecode(c->start, fps), delta < 0 ? "-" : "+", formatNumber(std::abs(delta), 2));
        } else {
            double edge = drag_.mode == DragMode::TrimStart ? c->start : c->end();
            text = fmt::format("{} {}   Duration {}", drag_.mode == DragMode::TrimStart ? "Start" : "End", formatTimecode(edge, fps),
                               formatTimecode(c->duration, fps));
        }
        std::string limit = drag_.limit;
        float w = std::max(ui.painter.measure(text), ui.painter.measure(limit)) + ui.dp(16);
        float h = ui.rowHeight() * (limit.empty() ? 1 : 2);
        Rect r{ui.mouse().x + ui.dp(12), ui.mouse().y - h - ui.dp(8), w, h};
        r.x = std::max(lanes_.x, std::min(r.x, lanes_.r() - r.w));
        r.y = std::max(ui.screen().y, r.y);
        ui.overlay(r, [&ui, r, text, limit] {
            ui.painter.fill(r, ui.col(Slot::Panel));
            ui.painter.outline(r, ui.col(Slot::Border));
            Rect t = r.inset(ui.dp(8), 0);
            ui.painter.textIn(cutTop(t, ui.rowHeight()), text, ui.col(Slot::Text), Align::Left, FontRole::Mono);
            if (!limit.empty()) ui.painter.textIn(cutTop(t, ui.rowHeight()), limit, ui.col(Slot::Error));
        });
    }

    void drawClip(ViewEnv& env, const Project& p, const Clip& c, const Rect& r) {
        Context& ui = env.ui;
        Session& s = env.session;
        const Track* track = p.findTrack(c.track);
        TrackKind tk = track ? track->kind : TrackKind::Video;
        bool dim = track && (track->muted || track->hidden);
        bool selected = s.selection.hasClip(c.id);
        Color fill = ui.col(clipSlot(c, tk));
        if (dim) fill.a *= 0.45f;
        ui.painter.fill(r, fill);

        if (c.kind == ClipKind::Media && tk == TrackKind::Audio)
            if (const MediaSource* m = p.findMedia(c.media)) drawWaveform(env, c, *m, r);
        Rect label = r.inset(ui.dp(6), 0);
        label.h = ui.rowHeight();
        std::string name = c.scriptOwned() ? c.name + " (script)" : c.name;
        if (label.w > ui.dp(12)) ui.painter.textIn(label, name, ui.col(Slot::ClipLabel));
        if (selected) ui.painter.outline(r, ui.col(Slot::ClipSelected));

        ui.pushId(c.id);
        Hit h = ui.interact(ui.id("clip"), r, true);
        float edge = std::min(ui.dp(6), r.w / 3);
        bool nearStart = ui.mouse().x < r.x + edge, nearEnd = ui.mouse().x >= r.r() - edge;
        if ((h.hovered || h.held) && !c.scriptOwned()) {
            if (drag_.mode == DragMode::TrimStart || drag_.mode == DragMode::TrimEnd || (!h.held && (nearStart || nearEnd)))
                ui.setCursor(Cursor::ResizeH);
        }
        if (h.pressed) pressClip(env, p, c, nearStart, nearEnd);
        if (h.held && drag_.clip == c.id) dragClip(env, p);
        if (h.released && drag_.clip == c.id) releaseClip(env);
        if (h.rightClicked) {
            if (!s.selection.hasClip(c.id)) s.selection.clips = {c.id};
            menuClip_ = c.id;
            ui.openPopup(ui.id("##clip-menu"), ui.mouse());
        }
        ui.popId();
    }

    void drawWaveform(ViewEnv& env, const Clip& c, const MediaSource& m, const Rect& r) {
        Context& ui = env.ui;
        Session& s = env.session;
        auto buf = s.cache().audio(m, s.project().settings.sampleRate, false);
        if (!buf || buf->peaks.empty()) {
            ui.requestFrame();
            return;
        }
        size_t blocks = buf->peaks.size() / 2;
        double blocksPerSec = double(buf->sampleRate) / AudioBuffer::kPeakBlock;
        float mid = r.y + r.h / 2, half = r.h * 0.42f;
        float x0 = std::max(r.x, lanes_.x), x1 = std::min(r.r(), lanes_.r());
        Color col = ui.col(Slot::Waveform);
        for (float x = x0; x < x1; x += 1) {
            double ta = c.in + (timeAt(x) - c.start) * c.speed, tb = c.in + (timeAt(x + 1) - c.start) * c.speed;
            size_t b0 = size_t(std::max(0.0, ta * blocksPerSec)), b1 = size_t(std::max(0.0, tb * blocksPerSec)) + 1;
            if (b0 >= blocks) break;
            float lo = 0, hi = 0;
            for (size_t b = b0; b < std::min(b1, blocks); ++b) {
                lo = std::min(lo, buf->peaks[b * 2]);
                hi = std::max(hi, buf->peaks[b * 2 + 1]);
            }
            ui.painter.vline(x, mid - hi * half, mid - lo * half + 1, col);
        }
    }

    void drawTransitions(ViewEnv& env, const Project& p) {
        Context& ui = env.ui;
        Session& s = env.session;
        for (const Transition& tr : p.transitions) {
            const Clip* b = p.findClip(tr.clipB);
            if (!b) continue;
            int lane = laneOf(p, b->track);
            if (lane < 0) continue;
            auto [w0, w1] = Evaluator::window(p, tr);
            float y = laneY(lane);
            Rect r{std::round(xAt(w0)), y + laneH_ / 2, std::max(2.0f, std::round(float((w1 - w0) * pps()))), laneH_ / 2 - 2};
            bool selected = s.selection.transition == tr.id;
            Color c = ui.col(Slot::Transition);
            c.a = selected ? 0.9f : 0.55f;
            ui.painter.fill(r, c);
            ui.painter.line({r.x, r.b()}, {r.r(), r.y}, ui.col(Slot::Background));
            if (selected) ui.painter.outline(r, ui.col(Slot::ClipSelected));
            ui.pushId(tr.id);
            Hit h = ui.interact(ui.id("transition"), r, true);
            if (h.pressed || h.rightClicked) {
                s.selection.clips.clear();
                s.selection.effect = 0;
                s.selection.transition = tr.id;
            }
            if (h.rightClicked) ui.openPopup(ui.id("##transition-menu"), ui.mouse());
            ui.popId();
        }
    }

    void drawDrop(ViewEnv& env, const Project& p) {
        Context& ui = env.ui;
        const Payload* payload = ui.dragging();
        if (!payload || payload->kind != "media") return;
        // The line shows where the clip will start, after snapping.
        double t = std::max(0.0, snapTime(env, p, timeAt(ui.mouse().x), {}));
        if (ui.mouseIn(lanes_)) ui.painter.vline(std::round(xAt(t)), lanes_.y, lanes_.b(), ui.col(Slot::Accent));
        auto dropped = ui.drop(lanes_);
        if (!dropped) return;
        Id media = dropped->id;
        Id vtrack = 0, atrack = 0;
        int lane = laneAt(p, ui.mouse().y);
        if (lane >= 0) {
            const Track& tr = p.tracks[trackIndexForLane(p, lane)];
            (tr.kind == TrackKind::Video ? vtrack : atrack) = tr.id;
        }
        env.shell.edit("Add media clip", [&](Project& pr) {
            auto ids = edit::addMediaClips(pr, media, t, vtrack, atrack);
            env.session.selection.clips = ids;
        });
    }

    void drawPlayhead(ViewEnv& env) {
        Context& ui = env.ui;
        float x = std::round(playheadX(env));
        if (x < lanes_.x || x >= lanes_.r()) return;
        ui.painter.vline(x, ruler_.y, lanes_.b(), ui.col(Slot::Playhead));
        float r = ui.dp(5);
        ui.painter.triangle({x - r, ruler_.y}, {x + r + 1, ruler_.y}, {x, ruler_.y + r}, ui.col(Slot::Playhead));
    }

    // ---- clip dragging ----

    void pressClip(ViewEnv& env, const Project& p, const Clip& c, bool nearStart, bool nearEnd) {
        Session& s = env.session;
        Context& ui = env.ui;
        uint8_t mods = ui.input().mods;
        s.selection.transition = 0;
        s.selection.effect = 0;
        s.selection.track = c.track;
        if (mods & (ModShift | ModGui)) {
            if (s.selection.hasClip(c.id)) std::erase(s.selection.clips, c.id);
            else s.selection.clips.push_back(c.id);
            drag_ = {};
            return;
        }
        bool wasSelected = s.selection.hasClip(c.id);
        if (!wasSelected) s.selection.clips = {c.id};
        else {
            // Keep the pressed clip primary for the inspector.
            std::erase(s.selection.clips, c.id);
            s.selection.clips.insert(s.selection.clips.begin(), c.id);
        }
        drag_ = {};
        drag_.clip = c.id;
        drag_.collapse = wasSelected;
        drag_.pressTime = timeAt(ui.mouse().x);
        drag_.pressLane = laneAt(p, ui.mouse().y);
        drag_.origStart = c.start;
        drag_.origDuration = c.duration;
        if (c.scriptOwned()) drag_.mode = DragMode::None;
        else if (nearStart) drag_.mode = DragMode::TrimStart;
        else if (nearEnd) drag_.mode = DragMode::TrimEnd;
        else drag_.mode = DragMode::Move;
        for (Id id : s.selection.clips)
            if (const Clip* sc = p.findClip(id); sc && !sc->scriptOwned()) drag_.origin.push_back({id, sc->start, sc->track});
    }

    void dragClip(ViewEnv& env, const Project& p) {
        Context& ui = env.ui;
        Session& s = env.session;
        if (drag_.mode == DragMode::None) return;
        if (!drag_.started) {
            Vec o = ui.input().mouse;
            if (std::abs(o.x - xAt(drag_.pressTime)) < ui.dp(3) && laneAt(p, o.y) == drag_.pressLane) return;
            drag_.started = true;
            s.beginGesture(drag_.mode == DragMode::Move ? "Move clips" : "Trim clip");
        }
        drag_.snap.reset();
        drag_.limit.clear();
        double frame = 1.0 / p.settings.fps;
        std::set<Id> moving;
        for (const auto& o : drag_.origin) {
            moving.insert(o.id);
            if (const Clip* c = s.project().findClip(o.id); c && c->link)
                for (const Clip& other : s.project().clips)
                    if (other.link == c->link) moving.insert(other.id);
        }
        double t = timeAt(ui.mouse().x);
        double dt = t - drag_.pressTime;
        Id clip = drag_.clip;
        try {
            if (drag_.mode == DragMode::Move) {
                // Snap the pressed clip's edges, then move every selected clip by the same amount.
                double start = drag_.origStart + dt;
                double snappedStart = snapTime(env, p, start, moving);
                double snappedEnd = snapTime(env, p, start + drag_.origDuration, moving);
                if (snappedStart != start) {
                    dt += snappedStart - start;
                    drag_.snap = snappedStart;
                } else if (snappedEnd != start + drag_.origDuration) {
                    dt += snappedEnd - (start + drag_.origDuration);
                    drag_.snap = snappedEnd;
                }
                double minStart = 1e300;
                for (const auto& o : drag_.origin) minStart = std::min(minStart, o.start);
                if (dt < -minStart) {
                    dt = -minStart;
                    drag_.snap.reset();
                    drag_.limit = "Reached the start of the timeline";
                }
                int laneDelta = drag_.pressLane >= 0 ? laneAt(p, ui.mouse().y) - drag_.pressLane : 0;
                if (laneAt(p, ui.mouse().y) < 0) laneDelta = 0;
                auto origin = drag_.origin;
                s.updateGesture([&](Project& pr) {
                    for (const auto& o : origin) {
                        std::optional<Id> track;
                        int lane = laneOf(pr, o.track) + laneDelta;
                        if (laneDelta != 0 && lane >= 0 && lane < laneCount(pr)) {
                            const Track& target = pr.tracks[trackIndexForLane(pr, lane)];
                            if (target.kind == pr.findTrack(o.track)->kind) track = target.id;
                        }
                        edit::moveClip(pr, o.id, o.start + dt, track);
                    }
                });
            } else if (drag_.mode == DragMode::TrimStart) {
                double end = drag_.origStart + drag_.origDuration;
                double raw = drag_.origStart + dt;
                double snapped = snapTime(env, p, raw, moving);
                double start = std::clamp(snapped, 0.0, end - frame);
                if (start != snapped) drag_.limit = start == 0 ? "Reached the start of the timeline" : "A clip is at least one frame long";
                else if (snapped != raw) drag_.snap = snapped;
                s.updateGesture([&](Project& pr) { edit::trimClip(pr, clip, start, end - start); });
            } else {
                double raw = drag_.origStart + drag_.origDuration + dt;
                double snapped = snapTime(env, p, raw, moving);
                double end = std::max(drag_.origStart + frame, snapped);
                if (end != snapped) drag_.limit = "A clip is at least one frame long";
                else if (snapped != raw) drag_.snap = snapped;
                s.updateGesture([&](Project& pr) { edit::trimClip(pr, clip, drag_.origStart, end - drag_.origStart); });
            }
        } catch (const EditError& e) {
            // Keep the last valid position, such as when a trim passes the
            // source start, and say why the clip stopped.
            drag_.snap.reset();
            drag_.limit = e.what();
            if (!drag_.limit.empty()) drag_.limit[0] = char(std::toupper(uint8_t(drag_.limit[0])));
        }
    }

    void releaseClip(ViewEnv& env) {
        Session& s = env.session;
        if (drag_.started) s.endGesture();
        else if (drag_.collapse) s.selection.clips = {drag_.clip};
        if (!drag_.started && drag_.mode == DragMode::None && drag_.clip >= kScriptIdBase)
            env.shell.notify("Script clips come from the project script; edit them in the script view");
        drag_ = {};
    }

    // Snaps t to the playhead, zero, and clip edges within a few pixels.
    double snapTime(ViewEnv& env, const Project& p, double t, const std::set<Id>& exclude) {
        if (!snap_ || (env.ui.input().mods & ModAlt)) return t;
        double best = t, bestDist = env.ui.dp(8) / pps();
        auto consider = [&](double c) {
            if (std::abs(c - t) < bestDist) bestDist = std::abs(c - t), best = c;
        };
        consider(0);
        consider(env.session.time());
        for (const Clip& c : p.clips) {
            if (exclude.count(c.id)) continue;
            consider(c.start);
            consider(c.end());
        }
        return best;
    }

    // ---- menus ----

    void drawMenus(ViewEnv& env) {
        Context& ui = env.ui;
        Session& s = env.session;
        Shell& shell = env.shell;

        if (beginMenu(ui, ui.id("##clip-menu"), ui.dp(220))) {
            const Project& p = s.evaluator().project();
            const Clip* c = p.findClip(menuClip_);
            bool editable = c && !c->scriptOwned();
            Id id = menuClip_;
            double t = s.time();
            bool under = c && c->start < t && c->end() > t;
            if (menuItem(ui, "Split at playhead", "", editable && under))
                shell.edit("Split", [&](Project& pr) { edit::splitClip(pr, id, t); });
            const Clip* next = nullptr;
            if (c)
                for (const Clip& o : p.clips)
                    if (o.track == c->track && o.id != c->id && o.start > c->start && o.start <= c->end() + 1e-3 &&
                        (!next || o.start < next->start))
                        next = &o;
            if (menuItem(ui, "Crossfade into next clip", "", editable && next && !next->scriptOwned())) {
                Id b = next->id;
                double dur = std::min({1.0, c->duration / 2, next->duration / 2});
                shell.edit("Add crossfade", [&](Project& pr) {
                    s.selection.transition = edit::addTransition(pr, id, b, "crossfade", dur);
                });
            }
            if (menuItem(ui, "Unlink", "", editable && c->link != 0))
                shell.edit("Unlink", [&](Project& pr) { edit::unlinkClip(pr, id); });
            menuSeparator(ui);
            if (menuItem(ui, "Show in inspector")) shell.showView("inspector");
            if (menuItem(ui, "Delete", "", editable)) {
                shell.edit("Delete clip", [&](Project& pr) { edit::removeClip(pr, id); });
                s.pruneSelection();
            }
            if (c && c->scriptOwned()) {
                menuSeparator(ui);
                menuItem(ui, "Defined by the project script", "", false);
            }
            endMenu(ui);
        }

        if (beginMenu(ui, ui.id("##transition-menu"), ui.dp(200))) {
            Id id = s.selection.transition;
            if (menuItem(ui, "Edit curve")) shell.showView("curves");
            if (menuItem(ui, "Delete")) {
                shell.edit("Delete transition", [&](Project& pr) { edit::removeTransition(pr, id); });
                s.pruneSelection();
            }
            endMenu(ui);
        }

        if (beginMenu(ui, ui.id("##lane-menu"), ui.dp(220))) {
            const Track* track = s.project().findTrack(menuTrack_);
            Id trackId = menuTrack_;
            double t = menuTime_;
            auto add = [&](ClipKind kind, std::string name, const char* prop = nullptr) {
                Clip c;
                c.track = trackId;
                c.kind = kind;
                c.name = name;
                c.start = t;
                c.duration = kNewClipSeconds;
                if (prop) c.props[prop] = Property::of(name);
                shell.edit("Add clip", [&](Project& pr) { s.selection.clips = {edit::addClip(pr, c)}; });
            };
            const ScriptOutput& out = s.evaluator().script();
            if (track && track->kind == TrackKind::Video) {
                if (menuItem(ui, "Add text")) add(ClipKind::Text, "Text");
                if (menuItem(ui, "Add solid")) add(ClipKind::Solid, "Solid");
                menuSeparator(ui);
                for (const auto& g : out.generators)
                    if (menuItem(ui, fmt::format("Add generator \"{}\"", g))) add(ClipKind::Generator, g, "generator");
                if (out.generators.empty()) menuItem(ui, "No generators in the script", "", false);
            } else if (track) {
                for (const auto& sy : out.synths)
                    if (menuItem(ui, fmt::format("Add synth \"{}\"", sy))) add(ClipKind::Synth, sy, "synth");
                if (out.synths.empty()) menuItem(ui, "No synths in the script", "", false);
            }
            menuSeparator(ui);
            if (menuItem(ui, "Import media...")) shell.importMedia();
            endMenu(ui);
        }

        if (beginMenu(ui, ui.id("##track-menu"), ui.dp(200))) {
            const Project& p = s.project();
            Id id = menuTrack_;
            int index = p.trackIndex(id);
            if (menuItem(ui, "Add video track"))
                shell.edit("Add track", [&](Project& pr) { edit::addTrack(pr, TrackKind::Video); });
            if (menuItem(ui, "Add audio track"))
                shell.edit("Add track", [&](Project& pr) { edit::addTrack(pr, TrackKind::Audio); });
            menuSeparator(ui);
            if (menuItem(ui, "Move up", "", index >= 0 && index + 1 < int(p.tracks.size())))
                shell.edit("Move track", [&](Project& pr) { edit::moveTrack(pr, id, index + 1); });
            if (menuItem(ui, "Move down", "", index > 0))
                shell.edit("Move track", [&](Project& pr) { edit::moveTrack(pr, id, index - 1); });
            if (menuItem(ui, "Remove track", "", index >= 0)) {
                shell.edit("Remove track", [&](Project& pr) { edit::removeTrack(pr, id); });
                s.pruneSelection();
            }
            endMenu(ui);
        }
    }

    struct Origin {
        Id id;
        double start;
        Id track;
    };
    struct Drag {
        DragMode mode = DragMode::None;
        Id clip = 0;
        bool started = false;
        bool collapse = false;  // a plain click on a selected clip selects only it
        double pressTime = 0;
        int pressLane = -1;
        double origStart = 0, origDuration = 0;
        std::vector<Origin> origin;
        std::optional<double> snap;  // where the last update snapped
        std::string limit;           // why the last update stopped short
    };

    float zoom_ = kDefaultZoom;
    float scale_ = 1;
    double scrollT_ = 0;
    float scrollY_ = 0;
    bool snap_ = true;
    Rect lanes_, ruler_;
    float laneH_ = 44;
    double majorStep_ = 1;
    Drag drag_;
    Id menuClip_ = 0, menuTrack_ = 0;
    double menuTime_ = 0;
};

}  // namespace

std::unique_ptr<View> timeline() { return std::make_unique<TimelineView>(); }

}  // namespace mf::ui::views
