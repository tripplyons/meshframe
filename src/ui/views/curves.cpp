#include "ui/views/common.hpp"
#include "ui/views/views.hpp"

#include <fmt/format.h>

namespace mf::ui::views {

namespace {

// What the editor shows: a property curve in clip-local seconds, or a
// transition's 0..1 mix curve.
struct CurveModel {
    std::string message;  // set when there is nothing to edit
    bool convertible = false;  // the property exists but is not keyframed
    Id target = 0;
    std::string prop;
    std::vector<std::string> props;  // animatable number properties of the target
    Curve curve;
    double t0 = 0, t1 = 1;
    bool unitRange = false;  // transition curves stay in 0..1
    double playhead = 0;
    std::function<void(Project&, const Curve&)> apply;
};

// Keyframe editor: drag keys, double-click to add, right-click for
// interpolation and easing, and drag bezier handles of the selected key.
class CurvesView : public View {
public:
    void header(ViewEnv& env, const Rect& area) override {
        Context& ui = env.ui;
        CurveModel m = model(env);
        Rect r = area;
        if (button(ui, cutRight(r, std::min(r.w, ui.dp(44))), "Fit")) fitPending_ = true;
        if (m.props.empty()) return;
        int index = 0;
        for (size_t i = 0; i < m.props.size(); ++i)
            if (m.props[i] == m.prop) index = int(i);
        if (dropdown(ui, cutRight(r, std::min(r.w, ui.dp(140))), "##prop", index, m.props)) {
            env.session.selection.prop = m.props[index];
            selectedKey_ = -1;
            fitPending_ = true;
        }
    }

    void draw(ViewEnv& env, const Rect& body) override {
        Context& ui = env.ui;
        Session& s = env.session;
        CurveModel m = model(env);
        if (m.target != lastTarget_ || m.prop != lastProp_) {
            lastTarget_ = m.target;
            lastProp_ = m.prop;
            selectedKey_ = -1;
            fitPending_ = true;
        }
        if (!m.message.empty()) {
            Rect r = body.inset(ui.pad());
            float h = paragraph(ui, r, m.message, Slot::TextFaint);
            if (m.convertible) {
                Rect b{r.x, r.y + h + ui.pad(), std::min(r.w, ui.dp(180)), ui.rowHeight()};
                if (button(ui, b, "Add a keyframe here")) {
                    Id target = m.target;
                    std::string name = m.prop;
                    Evaluator& ev = s.evaluator();
                    ev.setTime(s.time());
                    double v = asNumber(evaluatedValue(ev, target, name));
                    double t = m.playhead;
                    env.shell.edit("Keyframe " + name, [&](Project& p) { edit::setKeyframe(p, target, name, t, v); });
                }
            }
            return;
        }

        plot_ = body.inset(ui.dp(12), ui.dp(10));
        plot_.x += ui.dp(36);
        plot_.w -= ui.dp(36);
        if (plot_.empty()) return;
        if (!dragging_ && (fitPending_ || m.unitRange)) fit(m);
        tr0_ = m.t0;
        tr1_ = m.t1;

        drawGrid(env, m);
        drawCurve(env, m);
        drawPlayhead(env, m);
        drawKeys(env, m);
        drawMenu(env, m);
    }

private:
    CurveModel model(ViewEnv& env) const {
        Session& s = env.session;
        const Project& p = s.project();
        CurveModel m;
        Id target = 0;
        if (Id c = s.selection.primaryClip()) target = c;
        if (s.selection.effect) target = s.selection.effect;
        if (!target && s.selection.transition) {
            const Transition* tr = p.findTransition(s.selection.transition);
            if (!tr) return m.message = "Select a clip, effect, or transition.", m;
            bool propSelected = false;
            for (const auto& spec : edit::specsOf(p, tr->id)) propSelected |= spec.name == s.selection.prop;
            if (!propSelected) {
                m.target = tr->id;
                m.prop = "curve";
                m.curve = tr->curve;
                m.unitRange = true;
                auto [w0, w1] = Evaluator::window(p, *tr);
                m.playhead = w1 > w0 ? std::clamp((s.time() - w0) / (w1 - w0), 0.0, 1.0) : 0;
                Id id = tr->id;
                m.apply = [id](Project& pr, const Curve& c) { edit::setTransitionCurve(pr, id, c); };
                m.props.push_back("curve");
                for (const auto& spec : edit::specsOf(p, id))
                    if (spec.type == ValueType::Number && spec.animatable) m.props.push_back(spec.name);
                return m;
            }
            target = tr->id;
        }
        if (!target) return m.message = "Select a clip, effect, or transition,\nthen pick a property to edit its keyframes.", m;
        if (target >= kScriptIdBase) return m.message = "Script clips are defined by the project script.", m;
        m.target = target;
        std::vector<PropSpec> specs;
        try {
            specs = edit::specsOf(p, target);
        } catch (const EditError&) {
            return m.message = "The selection has no properties.", m;
        }
        if (p.findTransition(target)) m.props.push_back("curve");
        for (const auto& spec : specs)
            if (spec.type == ValueType::Number && spec.animatable) m.props.push_back(spec.name);
        m.prop = s.selection.prop;
        bool known = std::find(m.props.begin(), m.props.end(), m.prop) != m.props.end() && m.prop != "curve";
        if (!known) {
            m.prop.clear();
            return m.message = "Pick a property in the header to edit its keyframes.", m;
        }
        double start = targetStart(p, target), duration = 1;
        if (const Clip* c = p.findClip(target)) duration = c->duration;
        else if (const Transition* tr = p.findTransition(target)) {
            auto [w0, w1] = Evaluator::window(p, *tr);
            duration = w1 - w0;
        } else {
            Clip* owner = nullptr;
            const_cast<Project&>(p).findEffect(target, &owner);
            duration = owner ? owner->duration : std::max(1.0, p.duration());
        }
        m.t0 = 0;
        m.t1 = std::max(duration, 1e-3);
        m.playhead = s.time() - start;
        const Property* prop = findProperty(p, target, m.prop);
        if (!prop || prop->mode != PropMode::Curve) {
            m.convertible = true;
            m.message = fmt::format("\"{}\" is {}. Add a keyframe to animate it.", m.prop,
                                    prop && prop->mode == PropMode::Expression ? "an expression" : "a constant");
            return m;
        }
        m.curve = prop->curve;
        std::string name = m.prop;
        Property base = *prop;
        m.apply = [target, name, base](Project& pr, const Curve& c) {
            Property next = base;
            next.mode = PropMode::Curve;
            next.curve = c;
            edit::setProp(pr, target, name, next);
        };
        for (const auto& k : m.curve.keys) m.t1 = std::max(m.t1, k.t);
        for (const auto& k : m.curve.keys) m.t0 = std::min(m.t0, k.t);
        return m;
    }

    void fit(const CurveModel& m) {
        fitPending_ = false;
        if (m.unitRange) {
            v0_ = 0, v1_ = 1;
            return;
        }
        double lo = 1e300, hi = -1e300;
        for (int i = 0; i <= 64; ++i) {
            double v = m.curve.sample(m.t0 + (m.t1 - m.t0) * i / 64);
            lo = std::min(lo, v), hi = std::max(hi, v);
        }
        for (const auto& k : m.curve.keys) lo = std::min(lo, k.v), hi = std::max(hi, k.v);
        if (lo > hi) lo = 0, hi = 1;
        if (hi - lo < 1e-6) lo -= 0.5, hi += 0.5;
        double pad = (hi - lo) * 0.12;
        v0_ = lo - pad, v1_ = hi + pad;
    }

    float xOf(double t) const { return plot_.x + float((t - tr0_) / (tr1_ - tr0_) * plot_.w); }
    float yOf(double v) const { return plot_.b() - float((v - v0_) / (v1_ - v0_) * plot_.h); }
    double tAt(float x) const { return tr0_ + (x - plot_.x) / plot_.w * (tr1_ - tr0_); }
    double vAt(float y) const { return v0_ + (plot_.b() - y) / plot_.h * (v1_ - v0_); }

    void drawGrid(ViewEnv& env, const CurveModel& m) {
        Context& ui = env.ui;
        ui.painter.fill(plot_, ui.col(Slot::Field));
        for (int i = 0; i <= 4; ++i) {
            double v = v0_ + (v1_ - v0_) * i / 4;
            float y = std::round(yOf(v));
            ui.painter.hline(plot_.x, plot_.r(), y, ui.col(Slot::Grid));
            ui.painter.textIn({plot_.x - ui.dp(44), y - ui.rowHeight() / 2, ui.dp(40), ui.rowHeight()}, formatNumber(v, 2),
                              ui.col(Slot::TextFaint), Align::Right);
        }
        for (int i = 0; i <= 4; ++i) {
            float x = std::round(xOf(tr0_ + (tr1_ - tr0_) * i / 4));
            ui.painter.vline(x, plot_.y, plot_.b(), ui.col(Slot::Grid));
        }
        ui.painter.outline(plot_, ui.col(Slot::Border));
        std::string axis = m.unitRange ? "progress 0 to 1" : fmt::format("{} s", formatNumber(tr1_ - tr0_, 2));
        ui.painter.textIn({plot_.x + ui.dp(4), plot_.b() - ui.rowHeight(), plot_.w - ui.dp(8), ui.rowHeight()}, axis,
                          ui.col(Slot::TextFaint), Align::Right);
    }

    void drawCurve(ViewEnv& env, const CurveModel& m) {
        Context& ui = env.ui;
        std::vector<Vec> pts;
        int n = std::max(2, int(plot_.w / 2));
        for (int i = 0; i <= n; ++i) {
            double t = tr0_ + (tr1_ - tr0_) * i / n;
            pts.push_back({xOf(t), yOf(m.curve.sample(t))});
        }
        ui.painter.pushClip(plot_);
        ui.painter.polyline(pts, ui.col(Slot::Curve), ui.dp(1.5f));
        ui.painter.popClip();
    }

    void drawPlayhead(ViewEnv& env, const CurveModel& m) {
        Context& ui = env.ui;
        if (m.playhead < tr0_ || m.playhead > tr1_) return;
        ui.painter.vline(std::round(xOf(m.playhead)), plot_.y, plot_.b(), ui.col(Slot::Playhead));
    }

    void drawKeys(ViewEnv& env, const CurveModel& m) {
        Context& ui = env.ui;
        const auto& keys = m.curve.keys;
        if (selectedKey_ >= int(keys.size())) selectedKey_ = -1;

        // Background: double-click adds a key; a press clears the key selection.
        Hit bg = ui.interact(ui.id("plot"), plot_, true);
        if (bg.pressed) selectedKey_ = -1;
        if (bg.doubleClicked && m.apply) {
            Curve c = m.curve;
            double t = std::clamp(tAt(ui.mouse().x), m.unitRange ? 0.0 : -1e9, m.unitRange ? 1.0 : 1e9);
            double v = m.unitRange ? std::clamp(vAt(ui.mouse().y), 0.0, 1.0) : vAt(ui.mouse().y);
            selectedKey_ = int(c.setKey(t, v, keys.empty() ? Interp::Linear : keys.front().interp));
            env.shell.edit("Add keyframe", [&](Project& p) { m.apply(p, c); });
        }

        float r = ui.dp(5);
        // Bezier handles of the selected key's segment.
        if (selectedKey_ >= 0 && selectedKey_ + 1 < int(keys.size()) && keys[selectedKey_].interp == Interp::Bezier) {
            const Keyframe& a = keys[selectedKey_];
            const Keyframe& b = keys[selectedKey_ + 1];
            Vec pa{xOf(a.t), yOf(a.v)}, pb{xOf(b.t), yOf(b.v)};
            Vec h1{xOf(a.t + a.x1 * (b.t - a.t)), yOf(a.v + a.y1 * (b.v - a.v))};
            Vec h2{xOf(a.t + a.x2 * (b.t - a.t)), yOf(a.v + a.y2 * (b.v - a.v))};
            ui.painter.line(pa, h1, ui.col(Slot::TextFaint));
            ui.painter.line(pb, h2, ui.col(Slot::TextFaint));
            for (int which = 0; which < 2; ++which) {
                Vec h = which == 0 ? h1 : h2;
                Rect hr{h.x - r, h.y - r, 2 * r, 2 * r};
                Hit hh = ui.interact(ui.id(which == 0 ? "handle1" : "handle2"), hr);
                ui.painter.fill(hr.inset(ui.dp(1.5f)), hh.hovered || hh.held ? ui.col(Slot::Text) : ui.col(Slot::Curve));
                if (hh.pressed) beginDrag(env, m, "Edit easing");
                if (hh.held && !hh.pressed && m.apply && b.t != a.t) {
                    Curve c = dragCurve_;
                    Keyframe& k = c.keys[selectedKey_];
                    float fx = float(std::clamp((tAt(ui.mouse().x) - a.t) / (b.t - a.t), 0.0, 1.0));
                    float fy = b.v != a.v ? float((vAt(ui.mouse().y) - a.v) / (b.v - a.v)) : (which == 0 ? k.y1 : k.y2);
                    (which == 0 ? k.x1 : k.x2) = fx;
                    (which == 0 ? k.y1 : k.y2) = fy;
                    update(env, m, c);
                }
                if (hh.released) endDrag(env);
            }
        }

        for (int i = 0; i < int(keys.size()); ++i) {
            const Keyframe& k = keys[i];
            Vec c{xOf(k.t), yOf(k.v)};
            Rect kr{c.x - r - 2, c.y - r - 2, 2 * r + 4, 2 * r + 4};
            ui.pushId(uint64_t(i));
            Hit h = ui.interact(ui.id("key"), kr, true);
            bool sel = i == selectedKey_;
            ui.painter.diamond(c, r + (sel ? 1 : 0), sel || h.hovered ? ui.col(Slot::Text) : ui.col(Slot::Keyframe));
            if (!sel) ui.painter.diamond(c, r - ui.dp(2), ui.col(Slot::Field));
            if (h.pressed) {
                selectedKey_ = i;
                beginDrag(env, m, "Move keyframe");
            }
            if (h.held && !h.pressed && m.apply && dragKey_ >= 0) {
                Curve cv = dragCurve_;
                Keyframe moved = cv.keys[dragKey_];
                double t = tAt(ui.mouse().x), v = vAt(ui.mouse().y);
                if (m.unitRange) t = std::clamp(t, 0.0, 1.0), v = std::clamp(v, 0.0, 1.0);
                // Keep keys in order: a key cannot pass its neighbors.
                double eps = 1e-4;
                if (dragKey_ > 0) t = std::max(t, cv.keys[dragKey_ - 1].t + eps);
                if (dragKey_ + 1 < int(cv.keys.size())) t = std::min(t, cv.keys[dragKey_ + 1].t - eps);
                if (ui.input().mods & ModShift) t = moved.t;
                moved.t = t;
                moved.v = v;
                cv.keys[dragKey_] = moved;
                update(env, m, cv);
            }
            if (h.released) endDrag(env);
            if (h.rightClicked) {
                selectedKey_ = i;
                ui.openPopup(ui.id("##key-menu"), ui.mouse());
            }
            if (h.hovered && !h.held) {
                std::string tip = fmt::format("{} s  {}", formatNumber(k.t, 3), formatNumber(k.v, 3));
                ui.painter.textIn({c.x + r + 4, c.y - ui.rowHeight(), ui.dp(160), ui.rowHeight()}, tip, ui.col(Slot::Text));
            }
            ui.popId();
        }
    }

    void beginDrag(ViewEnv& env, const CurveModel& m, const std::string& label) {
        dragCurve_ = m.curve;
        dragKey_ = selectedKey_;
        dragging_ = true;
        env.session.beginGesture(label);
    }

    void update(ViewEnv& env, const CurveModel& m, const Curve& c) {
        try {
            env.session.updateGesture([&](Project& p) { m.apply(p, c); });
        } catch (const EditError&) {
        }
    }

    void endDrag(ViewEnv& env) {
        if (!dragging_) return;
        dragging_ = false;
        dragKey_ = -1;
        env.session.endGesture();
    }

    void drawMenu(ViewEnv& env, const CurveModel& m) {
        Context& ui = env.ui;
        if (!beginMenu(ui, ui.id("##key-menu"), ui.dp(200))) return;
        int i = selectedKey_;
        bool ok = i >= 0 && i < int(m.curve.keys.size()) && m.apply;
        auto change = [&](const char* label, auto fn) {
            Curve c = m.curve;
            fn(c.keys[i]);
            env.shell.edit(label, [&](Project& p) { m.apply(p, c); });
        };
        Interp cur = ok ? m.curve.keys[i].interp : Interp::Linear;
        if (menuItem(ui, "Hold", "", ok, cur == Interp::Hold)) change("Set interpolation", [](Keyframe& k) { k.interp = Interp::Hold; });
        if (menuItem(ui, "Linear", "", ok, cur == Interp::Linear)) change("Set interpolation", [](Keyframe& k) { k.interp = Interp::Linear; });
        if (menuItem(ui, "Bezier", "", ok, cur == Interp::Bezier)) change("Set interpolation", [](Keyframe& k) { k.interp = Interp::Bezier; });
        menuSeparator(ui);
        for (const auto& e : easePresets()) {
            const Keyframe* k = ok ? &m.curve.keys[i] : nullptr;
            bool on = k && k->interp == Interp::Bezier && k->x1 == e.x1 && k->y1 == e.y1 && k->x2 == e.x2 && k->y2 == e.y2;
            if (menuItem(ui, e.name, "", ok, on))
                change("Set easing", [&](Keyframe& kf) {
                    kf.interp = Interp::Bezier;
                    kf.x1 = e.x1, kf.y1 = e.y1, kf.x2 = e.x2, kf.y2 = e.y2;
                });
        }
        menuSeparator(ui);
        bool removable = ok && !(m.unitRange && m.curve.keys.size() <= 2);
        if (menuItem(ui, "Delete keyframe", "", removable)) {
            Curve c = m.curve;
            c.keys.erase(c.keys.begin() + i);
            selectedKey_ = -1;
            env.shell.edit("Remove keyframe", [&](Project& p) {
                if (c.keys.empty() && !m.unitRange) {
                    edit::setPropConstant(p, m.target, m.prop, m.curve.sample(m.curve.keys[i].t));
                } else m.apply(p, c);
            });
        }
        endMenu(ui);
    }

    Rect plot_;
    double tr0_ = 0, tr1_ = 1, v0_ = 0, v1_ = 1;
    bool fitPending_ = true;
    int selectedKey_ = -1;
    bool dragging_ = false;
    int dragKey_ = -1;
    Curve dragCurve_;
    Id lastTarget_ = 0;
    std::string lastProp_;
};

}  // namespace

std::unique_ptr<View> curves() { return std::make_unique<CurvesView>(); }

}  // namespace mf::ui::views
