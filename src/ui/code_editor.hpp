#pragma once

#include "ui/context.hpp"

#include <span>
#include <string>
#include <vector>

namespace mf::ui {

struct CodeMarker {
    int line = 0;  // 1-based
    std::string message;
};

// A multi-line monospace editor with Lua highlighting, line numbers, error
// markers, and its own undo stack.
class CodeEditor {
public:
    CodeEditor();

    // Replaces the text. Keeps the caret where it can and clears undo when `resetUndo`.
    void setText(const std::string& text, bool resetUndo = true);
    std::string text() const;
    int lineCount() const { return int(lines_.size()); }
    // Draws and edits. Returns true when the text changed this frame.
    bool draw(Context& ui, const Rect& r, std::string_view label, std::span<const CodeMarker> markers = {});
    bool focused(const Context& ui) const { return ui.hasFocus(id_); }
    double lastEditTime() const { return lastEdit_; }
    void gotoLine(int line);

private:
    struct Pos {
        int line = 0;
        size_t col = 0;
        bool operator==(const Pos&) const = default;
        auto operator<=>(const Pos&) const = default;
    };
    struct Snapshot {
        std::vector<std::string> lines;
        Pos caret, anchor;
    };

    Pos clampPos(Pos p) const;
    bool hasSelection() const { return caret_ != anchor_; }
    std::pair<Pos, Pos> selection() const { return std::minmax(caret_, anchor_); }
    std::string selectedText() const;
    void eraseSelection();
    void insert(std::string_view s);
    void pushUndo(int kind, double now);
    void handleKeys(Context& ui);
    Pos posAt(Context& ui, const Rect& textArea, Vec m) const;

    std::vector<std::string> lines_;
    Pos caret_, anchor_;
    float scrollX_ = 0, scrollY_ = 0;
    float preferredX_ = -1;
    bool ensureVisible_ = false;
    int pendingGoto_ = -1;
    std::vector<Snapshot> undo_, redo_;
    int lastKind_ = 0;
    double lastPush_ = -1;
    double lastEdit_ = 0;
    WidgetId id_ = 0;
    bool changed_ = false;
};

}  // namespace mf::ui
