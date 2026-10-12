#include "ui/context.hpp"

#include <algorithm>
#include <cctype>

namespace mf::ui {

namespace {

constexpr double kTooltipDelay = 0.5;

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

}  // namespace

Context::Context(Painter& p, const Theme& t) : painter(p), theme(t) {}

void Context::beginFrame(const Input& in, Rect screen, float scale, double now) {
    in_ = in;
    screen_ = screen;
    scale_ = scale;
    now_ = now;
    rowHeight_ = std::round(painter.lineHeight() + dp(8));
    idStack_.clear();
    hot_ = hotNext_;
    hotNext_ = 0;
    activeSeen_ = false;
    cursor_ = Cursor::Arrow;
    wheelTaken_ = false;
    wantsFrame_ = false;
    focusClaimed_ = false;
    commitNow_ = std::move(commit_);
    commit_.reset();
    typing_ = typingNext_;
    typingNext_ = false;
    inOverlay_ = false;
    inModal_ = false;
    overlays_.clear();
    lastItem_ = 0;
    lastHovered_ = false;
    tipNext_ = 0;
    tipShown_.clear();
    if (in_.pressed[0] || in_.pressed[1] || in_.pressed[2]) tipMuted_ = tipId_;

    // A press outside every overlay closes the open popup and is swallowed.
    if (popup_ && (in_.pressed[0] || in_.pressed[1])) {
        bool inside = false;
        for (const auto& r : blockers_) inside |= r.contains(in_.mouse);
        if (!inside) {
            popup_ = 0;
            in_.pressed[0] = in_.pressed[1] = false;
            blockers_.clear();
        }
    }
    if (!popup_) blockers_.clear();
    if (in_.mouse.x != lastMouse_.x || in_.mouse.y != lastMouse_.y) wantsFrame_ = true;
    prevMouse_ = lastMouse_;
    lastMouse_ = in_.mouse;
}

void Context::endFrame() {
    // Releasing ends the active widget even if it was not drawn this frame.
    if (active_ && (!activeSeen_ || (!in_.down[0] && !in_.down[1]))) active_ = 0;
    // A press that no widget took as focus clears focus.
    if ((in_.pressed[0] || in_.pressed[1]) && !focusClaimed_ && focused_) focus(0);
    if (commit_ || hot_ != hotNext_) wantsFrame_ = true;

    if (drag_) {
        Payload p = *drag_;
        overlay({in_.mouse.x + dp(12), in_.mouse.y + dp(4), painter.measure(p.label) + dp(12), rowHeight_}, [this, p] {
            Rect r{in_.mouse.x + dp(12), in_.mouse.y + dp(4), painter.measure(p.label) + dp(12), rowHeight_};
            painter.fill(r, col(Slot::Selection));
            painter.outline(r, col(Slot::Border));
            painter.textIn(r.inset(dp(6), 0), p.label, col(Slot::Text));
        });
        if (!in_.down[0]) drag_.reset();
        wantsFrame_ = true;
    }

    std::vector<Rect> next;
    for (auto& [r, draw] : overlays_) {
        draw();
        next.push_back(r);
    }
    overlays_.clear();
    // Popup overlays block the pointer for widgets underneath on the next frame.
    blockers_ = popup_ ? std::move(next) : std::vector<Rect>{};
    drawTooltip();
}

void Context::drawTooltip() {
    if (tipNext_ != tipId_) {
        tipId_ = tipNext_;
        tipSince_ = now_;
    }
    bool held = in_.down[0] || in_.down[1] || in_.down[2];
    if (tipMuted_ && tipId_ != tipMuted_ && !held) tipMuted_ = 0;
    if (!tipId_ || tipId_ == tipMuted_) return;
    if (now_ - tipSince_ < kTooltipDelay) {
        wantsFrame_ = true;
        return;
    }
    tipShown_ = tipText_;
    float margin = dp(4);
    float w = std::min(painter.measure(tipShown_) + dp(16), screen_.w - 2 * margin);
    Rect r{in_.mouse.x + dp(10), in_.mouse.y + dp(18), w, rowHeight_};
    // Above the pointer when there is no room below, as in the status bar.
    if (r.b() > screen_.b() - margin) r.y = in_.mouse.y - r.h - dp(6);
    r.x = std::clamp(r.x, screen_.x + margin, std::max(screen_.x + margin, screen_.r() - margin - r.w));
    r.y = std::max(r.y, screen_.y + margin);
    painter.fill(r, col(Slot::Panel));
    painter.outline(r, col(Slot::Border));
    painter.textIn(r.inset(dp(8), 0), tipShown_, col(Slot::Text));
}

void Context::markItem(WidgetId id, const Rect& r) {
    lastItem_ = id;
    lastHovered_ = !active_ && mouseIn(r);
}

void Context::tooltip(std::string_view text) {
    if (!lastHovered_ || text.empty()) return;
    tipNext_ = lastItem_;
    tipText_ = text;
}

void Context::focus(WidgetId id) {
    focusClaimed_ = true;
    if (focused_ == id) return;
    if (text_.id && text_.id != id) {
        if (text_.buf != text_.original) commit_ = {text_.id, text_.buf};
        text_.id = 0;
    }
    focused_ = id;
    wantsFrame_ = true;
}

std::optional<std::string> Context::takeCommit(WidgetId id) {
    if (!commitNow_ || commitNow_->first != id) return std::nullopt;
    std::string s = std::move(commitNow_->second);
    commitNow_.reset();
    return s;
}

void Context::overlayAt(size_t index, const Rect& area, std::function<void()> draw) {
    overlays_.insert(overlays_.begin() + std::min(index, overlays_.size()), {area, std::move(draw)});
}

WidgetId Context::id(std::string_view label) const {
    uint64_t h = idStack_.empty() ? 1469598103934665603ull : idStack_.back();
    for (unsigned char c : label) h = (h ^ c) * 1099511628211ull;
    return h ? h : 1;
}

WidgetId Context::id(uint64_t n) const {
    uint64_t h = idStack_.empty() ? 1469598103934665603ull : idStack_.back();
    h = mix(h, n);
    return h ? h : 1;
}

bool Context::mouseIn(const Rect& r) const {
    if (!r.contains(in_.mouse)) return false;
    if (modal_ && !inModal_) return false;
    // Overlays draw after the frame, outside the clip of the code that opened them.
    if (inOverlay_) return true;
    if (!painter.clip().contains(in_.mouse)) return false;
    for (const auto& b : blockers_)
        if (b.contains(in_.mouse)) return false;
    return true;
}

Hit Context::interact(WidgetId id, const Rect& r, bool rightButton) {
    Hit h;
    bool inside = mouseIn(r);
    if (inside && (!active_ || active_ == id)) hotNext_ = id;
    h.hovered = hot_ == id && inside && (!active_ || active_ == id);
    if (h.hovered && !active_) {
        if (in_.pressed[0]) {
            active_ = id;
            pressPos_ = in_.mouse;
            h.pressed = true;
            h.doubleClicked = in_.clicks >= 2;
        } else if (rightButton && in_.pressed[1]) {
            h.rightClicked = true;
        }
    }
    if (active_ == id) {
        activeSeen_ = true;
        h.held = in_.down[0];
        h.origin = pressPos_;
        h.delta = {in_.mouse.x - prevMouse_.x, in_.mouse.y - prevMouse_.y};
        if (in_.released[0]) {
            h.released = true;
            h.held = false;
            h.clicked = inside;
            active_ = 0;
        }
    }
    lastItem_ = id;
    lastHovered_ = h.hovered && !h.held;
    return h;
}

Vec Context::takeWheel(const Rect& r) {
    if (wheelTaken_ || !mouseIn(r)) return {};
    if (in_.wheelX == 0 && in_.wheelY == 0) return {};
    wheelTaken_ = true;
    return {in_.wheelX, in_.wheelY};
}

bool Context::key(SDL_Scancode sc, uint8_t mods) const {
    for (const auto& k : in_.keys)
        if (k.key == sc && k.mods == mods) return true;
    return false;
}

void Context::consumeKey(SDL_Scancode sc, uint8_t mods) {
    std::erase_if(in_.keys, [&](const KeyChord& k) { return k.key == sc && k.mods == mods; });
}

void Context::overlay(const Rect& area, std::function<void()> draw) { overlays_.emplace_back(area, std::move(draw)); }

std::optional<Payload> Context::drop(const Rect& r) {
    if (!drag_ || !in_.released[0]) return std::nullopt;
    bool saved = inOverlay_;
    inOverlay_ = false;
    bool inside = mouseIn(r);
    inOverlay_ = saved;
    if (!inside) return std::nullopt;
    Payload p = *drag_;
    drag_.reset();
    return p;
}

size_t prevBoundary(std::string_view s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (uint8_t(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

size_t nextBoundary(std::string_view s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (uint8_t(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

static bool wordChar(char c) { return std::isalnum(uint8_t(c)) || c == '_' || (uint8_t(c) & 0x80); }

size_t prevWord(std::string_view s, size_t i) {
    while (i > 0 && !wordChar(s[i - 1])) --i;
    while (i > 0 && wordChar(s[i - 1])) --i;
    return i;
}

size_t nextWord(std::string_view s, size_t i) {
    while (i < s.size() && !wordChar(s[i])) ++i;
    while (i < s.size() && wordChar(s[i])) ++i;
    return i;
}

}  // namespace mf::ui
