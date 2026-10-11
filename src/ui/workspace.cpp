#include "ui/workspace.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace mf::ui {

namespace {

using K = LayoutNode::Kind;

LayoutNode pane() { return {}; }
LayoutNode row(std::vector<LayoutNode> c, std::vector<float> w = {}) { return {K::Row, std::move(c), std::move(w)}; }
LayoutNode column(std::vector<LayoutNode> c, std::vector<float> w = {}) { return {K::Column, std::move(c), std::move(w)}; }

std::vector<LayoutPreset> makePresets() {
    return {
        {"single", "Single", pane(), {"preview"}},
        {"columns-2", "Two columns", row({pane(), pane()}), {"preview", "inspector"}},
        {"rows-2", "Two rows", column({pane(), pane()}), {"preview", "timeline"}},
        {"columns-3", "Three columns", row({pane(), pane(), pane()}), {"media", "preview", "inspector"}},
        {"grid-2x2", "Grid", column({row({pane(), pane()}), row({pane(), pane()})}),
         {"video-effects", "audio-effects", "curves", "preview"}},
        {"editor", "Editor", column({row({pane(), pane(), pane()}, {1, 2, 1}), pane()}, {3, 2}),
         {"media", "preview", "inspector", "timeline"}},
        {"main-stack", "Main and stack", row({pane(), column({pane(), pane()})}, {2, 1}), {"preview", "scopes", "meters"}},
        {"code", "Code", column({row({pane(), pane()}), row({pane(), pane()})}, {3, 2}), {"script", "graph", "preview", "console"}},
    };
}

void collectDividers(const LayoutNode& n, std::vector<float>& out) {
    if (n.kind == K::Pane) return;
    std::vector<float> w = n.weights;
    w.resize(n.children.size(), 1.0f);
    float total = std::accumulate(w.begin(), w.end(), 0.0f);
    float acc = 0;
    for (size_t i = 0; i + 1 < w.size(); ++i) {
        acc += w[i];
        out.push_back(acc / total);
    }
    for (const auto& ch : n.children) collectDividers(ch, out);
}

void layout(const LayoutNode& n, const Rect& r, const std::vector<float>& div, int& di, int& pi, float minPane, LayoutRects& out) {
    if (n.kind == K::Pane) {
        out.panes[pi++] = r;
        return;
    }
    bool side = n.kind == K::Row;
    size_t k = n.children.size();
    int first = di;
    di += int(k) - 1;
    float origin = side ? r.x : r.y;
    float extent = side ? r.w : r.h;
    // Border positions; each child starts one pixel after the previous border.
    std::vector<float> borders;
    for (size_t i = 0; i + 1 < k; ++i) {
        float f = div[first + i];
        float pos = std::round(origin + f * extent);
        float lo = (borders.empty() ? origin : borders.back() + 1) + minPane;
        float hi = origin + extent - float(k - 1 - i) * (minPane + 1) ;
        pos = std::clamp(pos, std::min(lo, hi), std::max(lo, hi));
        borders.push_back(pos);
    }
    float start = origin;
    for (size_t i = 0; i < k; ++i) {
        float end = i + 1 < k ? borders[i] : origin + extent;
        Rect c = side ? Rect{start, r.y, std::max(0.0f, end - start), r.h} : Rect{r.x, start, r.w, std::max(0.0f, end - start)};
        if (i + 1 < k) {
            Rect line = side ? Rect{end, r.y, 1, r.h} : Rect{r.x, end, r.w, 1};
            float prev = i == 0 ? 0.0f : div[first + i - 1];
            float next = i + 2 < k ? div[first + i + 1] : 1.0f;
            float m = extent > 0 ? (minPane + 1) / extent : 0;
            out.dividers.push_back({line, r, side, first + int(i), prev + m, next - m});
        }
        layout(n.children[i], c, div, di, pi, minPane, out);
        start = end + 1;
    }
}

}  // namespace

const std::vector<LayoutPreset>& layoutPresets() {
    static const std::vector<LayoutPreset> presets = makePresets();
    return presets;
}

const LayoutPreset* findLayout(std::string_view name) {
    for (const auto& p : layoutPresets())
        if (p.name == name) return &p;
    return nullptr;
}

int dividerCount(const LayoutNode& n) {
    std::vector<float> d;
    collectDividers(n, d);
    return int(d.size());
}

std::vector<float> defaultDividers(const LayoutNode& n) {
    std::vector<float> d;
    collectDividers(n, d);
    return d;
}

Workspace::Workspace() { reset(); }

void Workspace::reset() {
    struct Default {
        const char* layout;
        std::vector<std::string> views;
    };
    const Default defaults[kSpaces] = {
        {"editor", {}},
        {"grid-2x2", {}},
        {"code", {}},
        {"main-stack", {}},
        {"columns-2", {"history", "export"}},
        {"single", {"settings"}},
        {"single", {}},
        {"single", {}},
        {"single", {}},
        {"single", {}},
    };
    for (int i = 0; i < kSpaces; ++i) {
        spaces_[i] = {};
        setLayout(i, defaults[i].layout);
        for (size_t p = 0; p < defaults[i].views.size(); ++p) spaces_[i].panes[p].view = defaults[i].views[p];
    }
    current_ = 0;
}

void Workspace::switchTo(int index) {
    if (index >= 0 && index < kSpaces) current_ = index;
}

void Workspace::swapWith(int index) {
    if (index < 0 || index >= kSpaces || index == current_) return;
    std::swap(spaces_[current_], spaces_[index]);
    current_ = index;
}

void Workspace::setLayout(int index, const std::string& name) {
    const LayoutPreset* preset = findLayout(name);
    if (!preset || index < 0 || index >= kSpaces) return;
    Space& s = spaces_[index];
    bool same = s.layout == name;
    s.layout = name;
    for (size_t i = s.panes.size(); i < size_t(preset->panes()); ++i) s.panes.push_back({preset->views[i], nullptr});
    if (!same || int(s.dividers.size()) != dividerCount(preset->root)) s.dividers = defaultDividers(preset->root);
}

void Workspace::setPaneView(int space, int pane, const std::string& view) {
    if (space < 0 || space >= kSpaces) return;
    Space& s = spaces_[space];
    if (pane < 0 || pane >= int(s.panes.size()) || s.panes[pane].view == view) return;
    s.panes[pane] = {view, nullptr};
}

LayoutRects Workspace::compute(const Space& s, const Rect& area, float minPane) {
    LayoutRects out;
    const LayoutPreset* preset = findLayout(s.layout);
    if (!preset) preset = &layoutPresets().front();
    std::vector<float> div = s.dividers;
    if (int(div.size()) != dividerCount(preset->root)) div = defaultDividers(preset->root);
    out.panes.resize(preset->panes());
    int di = 0, pi = 0;
    layout(preset->root, area, div, di, pi, minPane, out);
    return out;
}

nlohmann::json Workspace::toJson() const {
    auto spaces = nlohmann::json::array();
    for (const auto& s : spaces_) {
        auto panes = nlohmann::json::array();
        for (const auto& p : s.panes) panes.push_back(p.view);
        spaces.push_back({{"layout", s.layout}, {"panes", panes}, {"dividers", s.dividers}});
    }
    return {{"current", current_ + 1}, {"spaces", spaces}};
}

void Workspace::loadJson(const nlohmann::json& j) {
    reset();
    if (!j.is_object()) return;
    if (j.contains("spaces") && j["spaces"].is_array()) {
        for (size_t i = 0; i < std::min<size_t>(kSpaces, j["spaces"].size()); ++i) {
            const auto& sj = j["spaces"][i];
            if (!sj.is_object()) continue;
            std::string layout = sj.value("layout", "");
            if (!findLayout(layout)) continue;
            Space s;
            s.layout = layout;
            if (sj.contains("panes") && sj["panes"].is_array())
                for (const auto& v : sj["panes"])
                    if (v.is_string()) s.panes.push_back({v.get<std::string>(), nullptr});
            if (sj.contains("dividers") && sj["dividers"].is_array())
                for (const auto& d : sj["dividers"])
                    if (d.is_number()) s.dividers.push_back(std::clamp(d.get<float>(), 0.02f, 0.98f));
            spaces_[i] = std::move(s);
            setLayout(int(i), layout);
        }
    }
    if (j.contains("current") && j["current"].is_number_integer()) switchTo(j["current"].get<int>() - 1);
}

}  // namespace mf::ui
