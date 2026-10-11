#include "ui/widgets.hpp"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>
#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cmath>

namespace mf::ui {

std::string_view displayLabel(std::string_view label) {
    size_t p = label.find("##");
    return p == std::string_view::npos ? label : label.substr(0, p);
}

std::string formatNumber(double v, int decimals) {
    if (!std::isfinite(v)) return v != v ? "nan" : (v > 0 ? "inf" : "-inf");
    std::string s = fmt::format("{:.{}f}", v, std::max(0, decimals));
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string formatTimecode(double seconds, double fps) {
    if (fps <= 0) fps = 30;
    int perSecond = std::max(1, int(std::round(fps)));
    int64_t frames = int64_t(std::floor(std::max(0.0, seconds) * fps + 1e-6));
    int64_t s = frames / perSecond;
    return fmt::format("{:02}:{:02}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60, frames % perSecond);
}

void drawChevronDown(Context& ui, Vec c, float size, const Color& col) {
    float h = size * 0.5f;
    ui.painter.triangle({c.x - size, c.y - h}, {c.x + size, c.y - h}, {c.x, c.y + h}, col);
}

bool button(Context& ui, const Rect& r, std::string_view label, bool selected, bool enabled) {
    WidgetId id = ui.id(label);
    Hit h = enabled ? ui.interact(id, r) : Hit{};
    Color bg = selected ? ui.col(Slot::Selection) : ui.col(Slot::Panel);
    if (h.held) bg = ui.col(Slot::Pressed);
    else if (h.hovered) bg = selected ? ui.col(Slot::Selection) : ui.col(Slot::Hover);
    ui.painter.fill(r, bg);
    ui.painter.textIn(r.inset(ui.dp(4), 0), displayLabel(label), enabled ? ui.col(Slot::Text) : ui.col(Slot::TextFaint), Align::Center);
    return h.clicked;
}

bool toggle(Context& ui, const Rect& r, std::string_view label, bool& value) {
    WidgetId id = ui.id(label);
    Hit h = ui.interact(id, r);
    if (h.hovered) ui.painter.fill(r, ui.col(Slot::Hover));
    float box = ui.dp(12);
    Rect b{r.x + ui.dp(4), r.y + std::floor((r.h - box) / 2), box, box};
    ui.painter.fill(b, ui.col(Slot::Field));
    ui.painter.outline(b, ui.col(Slot::TextFaint));
    if (value) ui.painter.fill(b.inset(ui.dp(3)), ui.col(Slot::Accent));
    Rect t = r;
    cutLeft(t, box + ui.dp(10));
    ui.painter.textIn(t, displayLabel(label), ui.col(Slot::Text));
    if (h.clicked) {
        value = !value;
        return true;
    }
    return false;
}

// ---- menus ------------------------------------------------------------------------

namespace {

struct MenuState {
    WidgetId id = 0;
    Rect area;
    float y = 0;
    size_t mark = 0;
    int count = 0;
};
MenuState g_menu;

}  // namespace

bool beginMenu(Context& ui, WidgetId id, float width) {
    if (!ui.popupOpen(id)) return false;
    Vec pos = ui.popupPos();
    if (width <= 0) width = ui.dp(200);
    float lastH = ui.popupHeight(id);
    Rect s = ui.screen();
    pos.x = std::clamp(pos.x, s.x, std::max(s.x, s.r() - width));
    if (lastH > 0 && pos.y + lastH > s.b()) pos.y = std::max(s.y, s.b() - lastH);
    g_menu = {id, {pos.x, pos.y, width, 0}, pos.y + ui.dp(4), ui.overlayMark(), 0};
    ui.setInOverlay(true);
    ui.pushId(id);
    return true;
}

bool menuItem(Context& ui, std::string_view label, std::string_view shortcut, bool enabled, bool checked) {
    Rect r{g_menu.area.x, g_menu.y, g_menu.area.w, ui.rowHeight()};
    g_menu.y += r.h;
    ++g_menu.count;
    WidgetId id = ui.id(label);
    Hit h = enabled ? ui.interact(id, r) : Hit{};
    std::string text(displayLabel(label)), keys(shortcut);
    bool hovered = h.hovered;
    ui.overlay(r, [&ui, r, text, keys, hovered, enabled, checked] {
        if (hovered) ui.painter.fill(r, ui.col(Slot::Selection));
        Rect t = r.inset(ui.dp(10), 0);
        Rect mark = cutLeft(t, ui.dp(14));
        if (checked) ui.painter.fill(Rect{mark.x, mark.y + std::floor(mark.h / 2) - ui.dp(3), ui.dp(6), ui.dp(6)}, ui.col(Slot::Accent));
        Color c = enabled ? ui.col(Slot::Text) : ui.col(Slot::TextFaint);
        if (!keys.empty()) {
            float kw = ui.painter.measure(keys);
            ui.painter.textIn(cutRight(t, kw), keys, ui.col(Slot::TextDim), Align::Right);
        }
        ui.painter.textIn(t, text, c);
    });
    if (h.clicked) {
        ui.closePopup();
        return true;
    }
    return false;
}

void menuSeparator(Context& ui) {
    Rect r{g_menu.area.x, g_menu.y, g_menu.area.w, ui.dp(9)};
    g_menu.y += r.h;
    ui.overlay(r, [&ui, r] { ui.painter.hline(r.x, r.r(), r.y + std::floor(r.h / 2), ui.col(Slot::Border)); });
}

void endMenu(Context& ui) {
    ui.popId();
    ui.setInOverlay(false);
    Rect area = g_menu.area;
    area.h = g_menu.y + ui.dp(4) - area.y;
    ui.popupHeight(g_menu.id) = area.h;
    ui.overlayAt(g_menu.mark, area, [&ui, area] {
        ui.painter.fill(area, ui.col(Slot::Panel));
        ui.painter.outline(area, ui.col(Slot::Border));
    });
}

bool dropdown(Context& ui, const Rect& r, std::string_view label, int& index, std::span<const std::string> options) {
    WidgetId id = ui.id(label);
    Hit h = ui.interact(id, r);
    bool open = ui.popupOpen(id);
    Color bg = open || h.held ? ui.col(Slot::Pressed) : h.hovered ? ui.col(Slot::Hover) : ui.col(Slot::Field);
    ui.painter.fill(r, bg);
    Rect t = r.inset(ui.dp(6), 0);
    Rect arrow = cutRight(t, ui.dp(10));
    drawChevronDown(ui, {arrow.x + arrow.w / 2, arrow.y + arrow.h / 2}, ui.dp(3.5f), ui.col(Slot::TextDim));
    std::string_view current = index >= 0 && size_t(index) < options.size() ? std::string_view(options[index]) : std::string_view();
    ui.painter.textIn(t, current, ui.col(Slot::Text));
    if (h.clicked) {
        if (open) ui.closePopup();
        else ui.openPopup(id, {r.x, r.b()});
    }
    bool changed = false;
    float width = r.w;
    for (const auto& o : options) width = std::max(width, ui.painter.measure(o) + ui.dp(44));
    if (beginMenu(ui, id, width)) {
        for (size_t i = 0; i < options.size(); ++i) {
            ui.pushId(uint64_t(i));
            if (menuItem(ui, options[i], {}, true, int(i) == index) && int(i) != index) {
                index = int(i);
                changed = true;
            }
            ui.popId();
        }
        endMenu(ui);
    }
    return changed;
}

// ---- text editing ---------------------------------------------------------------

namespace {

std::string clipboard() {
    char* t = SDL_GetClipboardText();
    std::string s = t ? t : "";
    SDL_free(t);
    return s;
}

// Edits ui.text() for a focused single-line field and draws it.
FieldResult editLine(Context& ui, WidgetId id, const Rect& r, const Hit& h, FontRole role) {
    FieldResult res;
    TextState& ts = ui.text();
    std::string& b = ts.buf;
    auto selMin = [&] { return std::min(ts.caret, ts.anchor); };
    auto selMax = [&] { return std::max(ts.caret, ts.anchor); };
    auto eraseSel = [&] {
        if (ts.caret == ts.anchor) return false;
        b.erase(selMin(), selMax() - selMin());
        ts.caret = ts.anchor = selMin();
        return true;
    };
    auto insert = [&](std::string_view s) {
        eraseSel();
        b.insert(ts.caret, s);
        ts.caret += s.size();
        ts.anchor = ts.caret;
    };
    Rect inner = r.inset(ui.dp(6), 0);
    auto caretAt = [&](float mx) {
        auto stops = ui.painter.caretStops(b, role);
        float x = mx - inner.x + ts.scroll;
        size_t best = 0;
        for (size_t i = 0; i < stops.size(); ++i) {
            if (i > 0 && (uint8_t(b[i - 1]) & 0xC0) == 0x80 && i < b.size() && (uint8_t(b[i]) & 0xC0) == 0x80) continue;
            if (std::abs(stops[i] - x) < std::abs(stops[best] - x)) best = i;
        }
        return best;
    };

    if (h.pressed) ui.focus(id);
    if (h.pressed && !h.doubleClicked) ts.caret = ts.anchor = caretAt(ui.mouse().x);
    if (h.doubleClicked) {
        ts.anchor = 0;
        ts.caret = b.size();
    }
    if (h.held && !h.pressed) ts.caret = caretAt(ui.mouse().x);

    for (const KeyChord& k : ui.input().keys) {
        bool shift = k.mods & ModShift;
        bool word = k.mods & ModAlt;
        bool line = k.mods & (ModGui | ModCtrl);
        uint8_t base = k.mods & ~ModShift;
        auto move = [&](size_t to) {
            ts.caret = to;
            if (!shift) ts.anchor = to;
        };
        switch (k.key) {
            case SDL_SCANCODE_LEFT:
                if (!shift && ts.caret != ts.anchor && !word && !line) move(selMin());
                else move(line ? 0 : word ? prevWord(b, ts.caret) : prevBoundary(b, ts.caret));
                break;
            case SDL_SCANCODE_RIGHT:
                if (!shift && ts.caret != ts.anchor && !word && !line) move(selMax());
                else move(line ? b.size() : word ? nextWord(b, ts.caret) : nextBoundary(b, ts.caret));
                break;
            case SDL_SCANCODE_HOME:
            case SDL_SCANCODE_UP: move(0); break;
            case SDL_SCANCODE_END:
            case SDL_SCANCODE_DOWN: move(b.size()); break;
            case SDL_SCANCODE_BACKSPACE:
                if (!eraseSel()) {
                    size_t from = line ? 0 : word ? prevWord(b, ts.caret) : prevBoundary(b, ts.caret);
                    b.erase(from, ts.caret - from);
                    ts.caret = ts.anchor = from;
                }
                break;
            case SDL_SCANCODE_DELETE:
                if (!eraseSel()) {
                    size_t to = word ? nextWord(b, ts.caret) : nextBoundary(b, ts.caret);
                    b.erase(ts.caret, to - ts.caret);
                }
                break;
            case SDL_SCANCODE_A:
                if (base == ModGui || base == ModCtrl) {
                    ts.anchor = 0;
                    ts.caret = b.size();
                }
                break;
            case SDL_SCANCODE_C:
            case SDL_SCANCODE_X:
                if ((base == ModGui || base == ModCtrl) && ts.caret != ts.anchor) {
                    SDL_SetClipboardText(b.substr(selMin(), selMax() - selMin()).c_str());
                    if (k.key == SDL_SCANCODE_X) eraseSel();
                }
                break;
            case SDL_SCANCODE_V:
                if (base == ModGui || base == ModCtrl) {
                    std::string s = clipboard();
                    std::erase_if(s, [](char c) { return c == '\n' || c == '\r'; });
                    insert(s);
                }
                break;
            case SDL_SCANCODE_Z:
                if (base == ModGui || base == ModCtrl) {
                    b = ts.original;
                    ts.caret = ts.anchor = b.size();
                }
                break;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER:
            case SDL_SCANCODE_TAB: res.committed = true; break;
            case SDL_SCANCODE_ESCAPE: res.canceled = true; break;
            default: break;
        }
    }
    if (!ui.input().text.empty()) {
        std::string t = ui.input().text;
        std::erase_if(t, [](char c) { return uint8_t(c) < 0x20; });
        insert(t);
    }

    // Keep the caret in view.
    auto stops = ui.painter.caretStops(b, role);
    float cx = stops[std::min(ts.caret, b.size())];
    if (cx - ts.scroll > inner.w - ui.dp(2)) ts.scroll = cx - inner.w + ui.dp(2);
    if (cx - ts.scroll < 0) ts.scroll = cx;
    if (stops.back() - ts.scroll < inner.w) ts.scroll = std::max(0.0f, stops.back() - inner.w + ui.dp(2));

    ui.painter.fill(r, ui.col(Slot::Field));
    ui.painter.pushClip(inner);
    float lh = ui.painter.lineHeight(role);
    float ty = r.y + std::floor((r.h - lh) / 2);
    if (ts.caret != ts.anchor) {
        float x0 = stops[selMin()] - ts.scroll, x1 = stops[selMax()] - ts.scroll;
        ui.painter.fill({inner.x + x0, ty, x1 - x0, lh}, ui.col(Slot::TextSelection));
    }
    ui.painter.text(inner.x - ts.scroll, ty, b, ui.col(Slot::Text), role);
    ui.painter.fill({inner.x + cx - ts.scroll, ty, std::max(1.0f, ui.dp(1)), lh}, ui.col(Slot::Caret));
    ui.painter.popClip();
    res.editing = true;
    return res;
}

void startEditing(Context& ui, WidgetId id, std::string value, bool selectAll) {
    ui.focus(id);
    TextState& ts = ui.text();
    ts.id = id;
    ts.original = value;
    ts.buf = std::move(value);
    ts.caret = ts.buf.size();
    ts.anchor = selectAll ? 0 : ts.caret;
    ts.scroll = 0;
}

void stopEditing(Context& ui) {
    ui.text().id = 0;
    ui.blur();
}

}  // namespace

FieldResult textField(Context& ui, const Rect& r, std::string_view label, std::string& value, FontRole role,
                      std::string_view placeholder) {
    WidgetId id = ui.id(label);
    Hit h = ui.interact(id, r);
    if (h.hovered) ui.setCursor(Cursor::Text);
    FieldResult res;
    if (auto committed = ui.takeCommit(id)) {
        value = *committed;
        res.committed = true;
    }
    TextState& ts = ui.text();
    // A press, or focus given with ui.focus(id), starts editing.
    if ((h.pressed || ui.hasFocus(id)) && ts.id != id) startEditing(ui, id, value, false);
    if (ui.hasFocus(id) && ts.id == id) {
        ui.setTyping();
        FieldResult e = editLine(ui, id, r, h, role);
        if (e.committed) {
            bool edited = ts.buf != value;
            value = ts.buf;
            stopEditing(ui);
            res.committed = edited || res.committed;
        } else if (e.canceled) {
            stopEditing(ui);
            res.canceled = true;
        } else {
            res.editing = true;
        }
        return res;
    }
    ui.painter.fill(r, h.hovered ? ui.col(Slot::Hover) : ui.col(Slot::Field));
    Rect inner = r.inset(ui.dp(6), 0);
    if (value.empty() && !placeholder.empty()) ui.painter.textIn(inner, placeholder, ui.col(Slot::TextFaint), Align::Left, role);
    else ui.painter.textIn(inner, value, ui.col(Slot::Text), Align::Left, role);
    return res;
}

static std::optional<double> parseDouble(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    double d = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), d);
    if (ec != std::errc() || s.empty()) return std::nullopt;
    return d;
}

DragResult numberField(Context& ui, const Rect& r, std::string_view label, double& value, const NumberStyle& st) {
    WidgetId id = ui.id(label);
    Hit h = ui.interact(id, r);
    DragResult res;
    auto clamp = [&](double v) { return std::clamp(v, st.min, st.max); };
    auto apply = [&](const std::string& text) {
        if (auto d = parseDouble(text); d && clamp(*d) != value) {
            value = clamp(*d);
            res.committed = res.changed = true;
        }
    };
    if (auto committed = ui.takeCommit(id)) apply(*committed);
    TextState& ts = ui.text();
    if (ui.hasFocus(id) && ts.id == id) {
        ui.setTyping();
        FieldResult e = editLine(ui, id, r, h, FontRole::Ui);
        if (e.committed) {
            std::string text = ts.buf;
            stopEditing(ui);
            apply(text);
        } else if (e.canceled) {
            stopEditing(ui);
        }
        return res;
    }

    double& start = ui.number(id);
    double& dragging = ui.number(id ^ 0x5bd1e995);
    if (h.hovered || h.held) ui.setCursor(Cursor::ResizeH);
    if (h.pressed) {
        start = value;
        dragging = 0;
    }
    if (h.held) {
        float dx = ui.mouse().x - h.origin.x;
        if (dragging == 0 && std::abs(dx) > ui.dp(3)) dragging = 1;
        if (dragging != 0) {
            double fine = (ui.input().mods & ModShift) ? 0.1 : 1.0;
            double v = start + double(dx / ui.dp(1)) * st.step * fine;
            if (st.decimals >= 0) v = std::round(v * std::pow(10.0, st.decimals)) / std::pow(10.0, st.decimals);
            v = clamp(v);
            if (dragging == 1) {
                res.began = true;
                dragging = 2;
            }
            if (v != value) {
                value = v;
                res.changed = true;
            }
        }
    }
    if (h.released) {
        if (dragging == 0 && h.clicked) startEditing(ui, id, formatNumber(value, std::max(st.decimals, 0)), true);
        else if (dragging != 0) res.ended = true;
        dragging = 0;
    }
    ui.painter.fill(r, h.held ? ui.col(Slot::Pressed) : h.hovered ? ui.col(Slot::Hover) : ui.col(Slot::Field));
    std::string text = formatNumber(value, st.decimals);
    if (!st.unit.empty()) text += fmt::format(" {}", st.unit);
    ui.painter.textIn(r.inset(ui.dp(6), 0), text, ui.col(Slot::Text));
    return res;
}

DragResult slider(Context& ui, const Rect& r, std::string_view label, double& value, double min, double max, int decimals) {
    WidgetId id = ui.id(label);
    Hit h = ui.interact(id, r);
    DragResult res;
    if (h.pressed) res.began = true;
    if (h.held && r.w > 0) {
        double f = std::clamp(double(ui.mouse().x - r.x) / r.w, 0.0, 1.0);
        double v = min + f * (max - min);
        if (decimals >= 0) v = std::round(v * std::pow(10.0, decimals)) / std::pow(10.0, decimals);
        if (v != value) {
            value = v;
            res.changed = true;
        }
    }
    if (h.released) res.ended = true;
    ui.painter.fill(r, h.hovered || h.held ? ui.col(Slot::Hover) : ui.col(Slot::Field));
    double f = max > min ? std::clamp((value - min) / (max - min), 0.0, 1.0) : 0;
    ui.painter.fill({r.x, r.y, float(std::round(r.w * f)), r.h}, ui.col(Slot::Selection));
    ui.painter.textIn(r.inset(ui.dp(6), 0), formatNumber(value, decimals), ui.col(Slot::Text));
    return res;
}

bool selectable(Context& ui, const Rect& r, std::string_view label, bool selected, Hit* hit, FontRole role) {
    WidgetId id = ui.id(label);
    Hit h = ui.interact(id, r, true);
    if (hit) *hit = h;
    if (selected) ui.painter.fill(r, ui.col(Slot::Selection));
    else if (h.hovered) ui.painter.fill(r, ui.col(Slot::Hover));
    ui.painter.textIn(r.inset(ui.dp(8), 0), displayLabel(label), ui.col(Slot::Text), Align::Left, role);
    return h.pressed;
}

// ---- scrolling --------------------------------------------------------------------

namespace {

struct ScrollFrame {
    WidgetId id;
    Rect view;
    float content;
};
std::vector<ScrollFrame> g_scroll;

}  // namespace

float beginScroll(Context& ui, const Rect& view, std::string_view label, float contentHeight) {
    WidgetId id = ui.id(label);
    float& off = ui.scroll(id);
    float maxOff = std::max(0.0f, contentHeight - view.h);
    Vec w = ui.takeWheel(view);
    if (w.y != 0) off -= w.y * ui.dp(48);
    // The scrollbar track takes drags on the right edge.
    if (maxOff > 0) {
        Rect track{view.r() - ui.dp(8), view.y, ui.dp(8), view.h};
        Hit h = ui.interact(id ^ 0x7f4a7c15, track);
        if (h.held) {
            float thumbH = std::max(ui.dp(20), view.h * view.h / contentHeight);
            float f = (ui.mouse().y - view.y - thumbH / 2) / std::max(1.0f, view.h - thumbH);
            off = f * maxOff;
        }
    }
    off = std::clamp(off, 0.0f, maxOff);
    off = std::round(off);
    g_scroll.push_back({id, view, contentHeight});
    ui.painter.pushClip(view);
    return off;
}

void endScroll(Context& ui) {
    ScrollFrame f = g_scroll.back();
    g_scroll.pop_back();
    ui.painter.popClip();
    float maxOff = std::max(0.0f, f.content - f.view.h);
    if (maxOff <= 0) return;
    float off = ui.scroll(f.id);
    float thumbH = std::max(ui.dp(20), f.view.h * f.view.h / f.content);
    float y = f.view.y + (f.view.h - thumbH) * (off / maxOff);
    ui.painter.fill({f.view.r() - ui.dp(5), y, ui.dp(3), thumbH}, ui.col(Slot::TextFaint));
}

void scrollTo(Context& ui, std::string_view label, float y, float h, float viewHeight) {
    float& off = ui.scroll(ui.id(label));
    if (y < off) off = y;
    else if (y + h > off + viewHeight) off = y + h - viewHeight;
}

}  // namespace mf::ui
