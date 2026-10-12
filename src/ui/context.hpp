#pragma once

#include "ui/geometry.hpp"
#include "ui/keys.hpp"
#include "ui/painter.hpp"
#include "ui/theme.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mf::ui {

using WidgetId = uint64_t;

// Input gathered for one frame, in physical pixels.
struct Input {
    Vec mouse{-1, -1};
    bool down[3] = {};      // left, right, middle
    bool pressed[3] = {};
    bool released[3] = {};
    int clicks = 0;         // click count of the latest left press (2 = double click)
    // Wheel steps in the system scroll direction: positive moves content right or down.
    float wheelX = 0, wheelY = 0;
    std::vector<KeyChord> keys;  // presses and repeats not taken by actions
    std::string text;            // typed text
    uint8_t mods = 0;
};

enum class Cursor { Arrow, Text, ResizeH, ResizeV, Hand, Move };

// Pointer interaction with one widget this frame.
struct Hit {
    bool hovered = false;
    bool pressed = false;   // press started on this widget this frame
    bool held = false;      // active (pressed earlier, not released)
    bool released = false;  // release ended this widget's press
    bool clicked = false;   // released while still over the widget
    bool doubleClicked = false;
    bool rightClicked = false;
    Vec delta;              // mouse movement this frame while held
    Vec origin;             // where the press started
};

// Something being dragged between widgets, such as a media item onto the timeline.
struct Payload {
    std::string kind;
    uint64_t id = 0;
    std::string label;
};

// Single-line text editing state of the focused field.
struct TextState {
    WidgetId id = 0;
    std::string buf;
    std::string original;
    size_t caret = 0, anchor = 0;
    float scroll = 0;
    bool selectAllOnFocus = false;
};

// Immediate-mode UI state. Widgets draw as they are called; hover goes to the
// last widget drawn under the mouse in the previous frame, so later (upper)
// widgets win over earlier ones.
class Context {
public:
    Context(Painter& p, const Theme& theme);

    void beginFrame(const Input& in, Rect screen, float scale, double now);
    void endFrame();
    // True when hover, focus, or an animation changed and another frame is needed.
    bool wantsFrame() const { return wantsFrame_; }
    void requestFrame() { wantsFrame_ = true; }

    Painter& painter;
    const Theme& theme;
    Color col(Slot s) const { return theme[s]; }
    float scale() const { return scale_; }
    // Device-independent size to physical pixels, rounded.
    float dp(float v) const { return std::round(v * scale_); }
    float rowHeight() const { return rowHeight_; }
    float pad() const { return dp(6); }
    Rect screen() const { return screen_; }
    const Input& input() const { return in_; }
    Vec mouse() const { return in_.mouse; }
    double now() const { return now_; }

    // Ids hash a label with the id stack. "Text##key" displays "Text" and hashes all of it.
    WidgetId id(std::string_view label) const;
    WidgetId id(uint64_t n) const;
    void pushId(std::string_view s) { idStack_.push_back(id(s)); }
    void pushId(uint64_t n) { idStack_.push_back(id(n)); }
    void popId() { idStack_.pop_back(); }

    // Pointer.
    bool mouseIn(const Rect& r) const;  // inside r and the clip, not blocked by an overlay
    Hit interact(WidgetId id, const Rect& r, bool rightButton = false);
    bool isActive(WidgetId id) const { return active_ == id; }
    WidgetId active() const { return active_; }
    bool anyActive() const { return active_ != 0; }
    void setCursor(Cursor c) { cursor_ = c; }
    Cursor cursor() const { return cursor_; }
    // Wheel movement over r, consumed by the first caller.
    Vec takeWheel(const Rect& r);
    // Records a widget that takes no input, such as a disabled button, so
    // tooltip() can describe it.
    void markItem(WidgetId id, const Rect& r);
    // Describes the last widget drawn. The text shows near the pointer after
    // it rests on that widget, and hides on a press until the pointer leaves.
    void tooltip(std::string_view text);
    // The tooltip drawn this frame, or "".
    const std::string& shownTooltip() const { return tipShown_; }

    // A modal dialog: while one is open, only widgets drawn between
    // beginModal() and endModal() get the pointer and wheel.
    void setModal(bool open) { modal_ = open; }
    bool modal() const { return modal_; }
    void beginModal() { inModal_ = true; }
    void endModal() { inModal_ = false; }

    // Keyboard.
    bool key(SDL_Scancode sc, uint8_t mods = 0) const;  // exact chord pressed this frame
    void consumeKey(SDL_Scancode sc, uint8_t mods = 0);
    // Moving focus away from an edited field queues its text as a commit.
    void focus(WidgetId id);
    void blur() { focus(0); }
    WidgetId focused() const { return focused_; }
    bool hasFocus(WidgetId id) const { return focused_ == id; }
    // Text of a field that lost focus with an edit; returned once to that field.
    std::optional<std::string> takeCommit(WidgetId id);
    // A text field or editor has focus, so plain keys type instead of running actions.
    bool typing() const { return typing_; }
    void setTyping() { typingNext_ = true; }
    TextState& text() { return text_; }

    // Popups and overlays. Overlay draws run after the frame; their rects block
    // the pointer for widgets underneath on the next frame.
    void overlay(const Rect& area, std::function<void()> draw);
    bool inOverlay() const { return inOverlay_; }
    void setInOverlay(bool v) { inOverlay_ = v; }
    void openPopup(WidgetId id, Vec pos) {
        popup_ = id;
        popupPos_ = pos;
    }
    void closePopup() { popup_ = 0; }
    bool popupOpen(WidgetId id) const { return popup_ == id; }
    WidgetId popup() const { return popup_; }
    Vec popupPos() const { return popupPos_; }
    float& popupHeight(WidgetId id) { return popupHeights_[id]; }

    // Per-widget scroll offsets and other persistent numbers.
    float& scroll(WidgetId id) { return floats_[id]; }
    double& number(WidgetId id) { return numbers_[id]; }
    // Overlay insertion point, so a popup background can go under items drawn earlier.
    size_t overlayMark() const { return overlays_.size(); }
    void overlayAt(size_t index, const Rect& area, std::function<void()> draw);

    // Drag and drop.
    void beginDrag(Payload p) { drag_ = std::move(p); }
    const Payload* dragging() const { return drag_ ? &*drag_ : nullptr; }
    // Returns the payload when it is released over r.
    std::optional<Payload> drop(const Rect& r);

private:
    void drawTooltip();

    Input in_;
    Rect screen_;
    float scale_ = 1;
    double now_ = 0;
    float rowHeight_ = 22;
    std::vector<WidgetId> idStack_;
    WidgetId hot_ = 0, hotNext_ = 0, active_ = 0;
    bool activeSeen_ = false;
    Vec pressPos_;
    WidgetId focused_ = 0;
    std::optional<std::pair<WidgetId, std::string>> commit_, commitNow_;
    bool focusClaimed_ = false;
    bool typing_ = false, typingNext_ = false;
    TextState text_;
    Cursor cursor_ = Cursor::Arrow;
    bool wheelTaken_ = false;
    std::vector<std::pair<Rect, std::function<void()>>> overlays_;
    std::vector<Rect> blockers_;
    bool inOverlay_ = false;
    WidgetId popup_ = 0;
    Vec popupPos_;
    std::unordered_map<WidgetId, float> popupHeights_;
    std::unordered_map<WidgetId, float> floats_;
    std::unordered_map<WidgetId, double> numbers_;
    std::optional<Payload> drag_;
    bool modal_ = false, inModal_ = false;
    WidgetId lastItem_ = 0;
    bool lastHovered_ = false;
    WidgetId tipId_ = 0, tipNext_ = 0;
    std::string tipText_;
    double tipSince_ = 0;
    WidgetId tipMuted_ = 0;  // pressed; no tooltip until the pointer leaves it
    std::string tipShown_;
    bool wantsFrame_ = false;
    Vec lastMouse_, prevMouse_;
};

// UTF-8 caret movement.
size_t prevBoundary(std::string_view s, size_t i);
size_t nextBoundary(std::string_view s, size_t i);
size_t prevWord(std::string_view s, size_t i);
size_t nextWord(std::string_view s, size_t i);

}  // namespace mf::ui
