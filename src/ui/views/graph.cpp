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

// The evaluator's reactive graph: nodes in columns by dependency depth, with
// values, evaluation counts, and errors. Click a node to select what it belongs to.
class GraphView : public View {
public:
    void header(ViewEnv& env, const Rect& area) override {
        Context& ui = env.ui;
        Rect r = area;
        if (button(ui, cutRight(r, std::min(r.w, ui.dp(44))), "Fit")) fitPending_ = true;
        textField(ui, cutRight(r, std::min(r.w, ui.dp(160))), "##filter", filter_, FontRole::Ui, "Filter");
    }

    void draw(ViewEnv& env, const Rect& body) override {
        Context& ui = env.ui;
        Session& s = env.session;
        Evaluator& ev = s.evaluator();
        ev.setTime(s.time());
        ev.refreshOutputs();
        const Graph& g = ev.graph();
        if (g.size() != laidOut_ || s.revision() != layoutRevision_) {
            layout(g);
            layoutRevision_ = s.revision();
        }
        view_ = body;
        ui.painter.fill(body, ui.col(Slot::Background));

        float nodeW = ui.dp(170) * zoom_, nodeH = ui.rowHeight() * 2 * zoom_;
        float colW = nodeW + ui.dp(60) * zoom_, rowH = nodeH + ui.dp(10) * zoom_;
        if (fitPending_ && body.w > 0) {
            fitPending_ = false;
            float w = columns_ * (ui.dp(170) + ui.dp(60)), h = maxRows_ * (ui.rowHeight() * 2 + ui.dp(10));
            zoom_ = std::clamp(std::min(body.w / std::max(1.0f, w), body.h / std::max(1.0f, h)), 0.25f, 1.0f);
            pan_ = {ui.dp(12), ui.dp(12)};
            return draw(env, body);
        }

        // Pan by dragging the background or with the wheel; Cmd+wheel zooms.
        Hit bg = ui.interact(ui.id("canvas"), body);
        if (bg.held && !bg.pressed) pan_.x += bg.delta.x, pan_.y += bg.delta.y;
        if (bg.held) ui.setCursor(Cursor::Move);
        Vec w = ui.takeWheel(body);
        if (w.x != 0 || w.y != 0) {
            if (ui.input().mods & ModGui) {
                float z = std::clamp(zoom_ * std::pow(1.12f, w.y), 0.25f, 2.0f);
                Vec m = ui.mouse();
                pan_.x = m.x - body.x - (m.x - body.x - pan_.x) * z / zoom_;
                pan_.y = m.y - body.y - (m.y - body.y - pan_.y) * z / zoom_;
                zoom_ = z;
            } else {
                pan_.x += w.x * ui.dp(40);
                pan_.y += w.y * ui.dp(40);
            }
        }

        auto rectOf = [&](Graph::Index i) {
            return Rect{std::round(body.x + pan_.x + pos_[i].col * colW), std::round(body.y + pan_.y + pos_[i].row * rowH), nodeW, nodeH};
        };

        // Hover from the previous frame decides highlighting.
        Graph::Index hot = hovered_;
        hovered_ = Graph::kNone;
        auto related = [&](Graph::Index a) {
            if (hot == Graph::kNone) return false;
            if (a == hot) return true;
            const auto& n = g.node(hot);
            return std::find(n.deps.begin(), n.deps.end(), a) != n.deps.end() ||
                   std::find(n.dependents.begin(), n.dependents.end(), a) != n.dependents.end();
        };

        // Edges first, under the nodes.
        for (Graph::Index i = 0; i < g.size(); ++i) {
            Rect to = rectOf(i);
            for (Graph::Index d : g.node(i).deps) {
                Rect from = rectOf(d);
                if (std::max(from.r(), to.x) < body.x || std::min(from.r(), to.x) > body.r()) continue;
                bool hi = hot != Graph::kNone && (i == hot || d == hot);
                bool match = matches(g, i) && matches(g, d);
                Color c = ui.col(hi ? Slot::GraphActive : Slot::GraphEdge);
                if (!match && !hi) c.a *= 0.25f;
                Vec a{from.r(), from.y + from.h / 2}, b{to.x, to.y + to.h / 2};
                Vec pts[8];
                for (int k = 0; k < 8; ++k) {
                    float t = k / 7.0f, s3 = t * t * (3 - 2 * t);
                    pts[k] = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * s3};
                }
                ui.painter.polyline(pts, c, hi ? ui.dp(1.5f) : 1);
            }
        }

        for (Graph::Index i = 0; i < g.size(); ++i) {
            Rect r = rectOf(i);
            if (r.r() < body.x || r.x > body.r() || r.b() < body.y || r.y > body.b()) continue;
            const Graph::Node& n = g.node(i);
            bool match = matches(g, i);
            bool hi = related(i);
            Color fill = ui.col(n.error.empty() ? Slot::GraphNode : Slot::ErrorBackground);
            if (!match) fill.a *= 0.4f;
            ui.painter.fill(r, fill);
            ui.painter.outline(r, ui.col(!n.error.empty() ? Slot::Error : hi ? Slot::GraphActive : Slot::Border));
            if (zoom_ >= 0.7f) {
                Rect top{r.x, r.y, r.w, r.h / 2}, bottom{r.x, r.y + r.h / 2, r.w, r.h / 2};
                Color tc = ui.col(match ? Slot::Text : Slot::TextFaint);
                std::string label = n.label.empty() ? n.key : n.label;
                ui.painter.textIn(top.inset(ui.dp(6), 0), label, tc);
                std::string value = n.error.empty() ? describe(n.value) : n.error;
                std::string count = fmt::format("{}x", n.evalCount);
                float cw = ui.painter.measure(count, FontRole::Mono);
                Rect b = bottom.inset(ui.dp(6), 0);
                ui.painter.textIn(cutRight(b, cw), count, ui.col(Slot::TextFaint), Align::Right, FontRole::Mono);
                ui.painter.textIn(b, value, ui.col(n.error.empty() ? Slot::TextDim : Slot::Error), Align::Left, FontRole::Mono);
            }
            ui.pushId(uint64_t(i));
            Hit h = ui.interact(ui.id("node"), r);
            if (h.hovered) hovered_ = i;
            if (h.clicked) select(env, n);
            ui.popId();
        }
        if (hovered_ != Graph::kNone) {
            const auto& n = g.node(hovered_);
            std::string tip = fmt::format("{}  ({}, {} deps, {} dependents)", n.key, kindName(n.kind), n.deps.size(), n.dependents.size());
            Rect bar{body.x, body.b() - ui.rowHeight(), body.w, ui.rowHeight()};
            ui.painter.fill(bar, ui.col(Slot::Panel));
            ui.painter.textIn(bar.inset(ui.dp(8), 0), tip, ui.col(Slot::Text), Align::Left, FontRole::Mono);
        }
        if (hot != hovered_) ui.requestFrame();
    }

private:
    struct Pos {
        int col = 0, row = 0;
    };

    bool matches(const Graph& g, Graph::Index i) const {
        const auto& n = g.node(i);
        return contains(n.key, filter_) || contains(n.label, filter_);
    }

    void layout(const Graph& g) {
        laidOut_ = g.size();
        std::vector<int> depth(g.size(), -1);
        // Longest path from a source; the iteration bound guards against cycles.
        for (size_t pass = 0; pass < g.size() + 1; ++pass) {
            bool changed = false;
            for (Graph::Index i = 0; i < g.size(); ++i) {
                int d = 0;
                for (Graph::Index dep : g.node(i).deps) d = std::max(d, std::max(depth[dep], 0) + 1);
                if (d != depth[i]) depth[i] = d, changed = true;
            }
            if (!changed) break;
        }
        std::vector<std::vector<Graph::Index>> cols;
        for (Graph::Index i = 0; i < g.size(); ++i) {
            int d = std::min(depth[i], 64);
            if (int(cols.size()) <= d) cols.resize(d + 1);
            cols[d].push_back(i);
        }
        pos_.assign(g.size(), {});
        maxRows_ = 0;
        for (size_t c = 0; c < cols.size(); ++c) {
            auto& col = cols[c];
            std::stable_sort(col.begin(), col.end(), [&](Graph::Index a, Graph::Index b) {
                if (g.node(a).kind != g.node(b).kind) return g.node(a).kind < g.node(b).kind;
                return g.node(a).key < g.node(b).key;
            });
            for (size_t r = 0; r < col.size(); ++r) pos_[col[r]] = {int(c), int(r)};
            maxRows_ = std::max(maxRows_, int(col.size()));
        }
        columns_ = int(cols.size());
    }

    void select(ViewEnv& env, const Graph::Node& n) {
        Session& s = env.session;
        const Project& p = s.evaluator().project();
        Id id = n.subject;
        if (n.key.starts_with("prop:")) {
            auto dot = n.key.find('.');
            if (dot != std::string::npos) s.selection.prop = n.key.substr(dot + 1);
        }
        if (!id) return;
        if (p.findClip(id)) {
            s.selection.clips = {id};
            s.selection.effect = 0;
            s.selection.transition = 0;
        } else if (p.findTransition(id)) {
            s.selection.clips.clear();
            s.selection.effect = 0;
            s.selection.transition = id;
        } else {
            Clip* owner = nullptr;
            if (const_cast<Project&>(p).findEffect(id, &owner)) {
                s.selection.clips = owner ? std::vector<Id>{owner->id} : std::vector<Id>{};
                s.selection.effect = id;
                s.selection.transition = 0;
            }
        }
    }

    std::string filter_;
    std::vector<Pos> pos_;
    size_t laidOut_ = 0;
    uint64_t layoutRevision_ = 0;
    int columns_ = 0, maxRows_ = 0;
    Vec pan_{12, 12};
    float zoom_ = 1;
    bool fitPending_ = true;
    Rect view_;
    Graph::Index hovered_ = Graph::kNone;
};

}  // namespace

std::unique_ptr<View> graph() { return std::make_unique<GraphView>(); }

}  // namespace mf::ui::views
