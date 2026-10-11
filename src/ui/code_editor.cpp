#include "ui/code_editor.hpp"

#include "ui/widgets.hpp"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>
#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace mf::ui {

namespace {

constexpr std::string_view kKeywords[] = {"and",   "break", "do",  "else", "elseif", "end",    "false", "for",
                                          "function", "goto", "if", "in",   "local",  "nil",    "not",   "or",
                                          "repeat", "return", "then", "true", "until", "while"};

bool isKeyword(std::string_view w) { return std::ranges::contains(kKeywords, w); }

struct Token {
    size_t begin, end;
    Slot color;
};

// Splits one line into colored tokens. `inBlock` carries a --[[ comment across lines.
std::vector<Token> tokenize(std::string_view s, bool& inBlock) {
    std::vector<Token> out;
    size_t i = 0;
    auto push = [&](size_t b, size_t e, Slot c) {
        if (e > b) out.push_back({b, e, c});
    };
    while (i < s.size()) {
        if (inBlock) {
            size_t close = s.find("]]", i);
            size_t e = close == std::string_view::npos ? s.size() : close + 2;
            push(i, e, Slot::CodeComment);
            if (close != std::string_view::npos) inBlock = false;
            i = e;
            continue;
        }
        char c = s[i];
        if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            if (s.substr(i + 2, 2) == "[[") {
                inBlock = true;
                push(i, i + 4, Slot::CodeComment);
                i += 4;
                continue;
            }
            push(i, s.size(), Slot::CodeComment);
            break;
        }
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < s.size() && s[j] != c) j += s[j] == '\\' ? 2 : 1;
            j = std::min(j + 1, s.size());
            push(i, j, Slot::CodeString);
            i = j;
            continue;
        }
        if (std::isdigit(uint8_t(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(uint8_t(s[i + 1])))) {
            size_t j = i;
            while (j < s.size() && (std::isalnum(uint8_t(s[j])) || s[j] == '.')) ++j;
            push(i, j, Slot::CodeNumber);
            i = j;
            continue;
        }
        if (std::isalpha(uint8_t(c)) || c == '_') {
            size_t j = i;
            while (j < s.size() && (std::isalnum(uint8_t(s[j])) || s[j] == '_')) ++j;
            push(i, j, isKeyword(s.substr(i, j - i)) ? Slot::CodeKeyword : Slot::Text);
            i = j;
            continue;
        }
        size_t j = i + 1;
        while (j < s.size() && !std::isalnum(uint8_t(s[j])) && s[j] != '_' && s[j] != '"' && s[j] != '\'' && s[j] != '-') ++j;
        push(i, j, Slot::Text);
        i = j;
    }
    return out;
}

}  // namespace

CodeEditor::CodeEditor() : lines_{""} {}

void CodeEditor::setText(const std::string& text, bool resetUndo) {
    lines_.clear();
    size_t start = 0;
    while (true) {
        size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines_.push_back(std::move(line));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    caret_ = clampPos(caret_);
    anchor_ = clampPos(anchor_);
    if (resetUndo) {
        undo_.clear();
        redo_.clear();
    }
}

std::string CodeEditor::text() const {
    std::string out;
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (i) out += '\n';
        out += lines_[i];
    }
    return out;
}

void CodeEditor::gotoLine(int line) { pendingGoto_ = line; }

CodeEditor::Pos CodeEditor::clampPos(Pos p) const {
    p.line = std::clamp(p.line, 0, int(lines_.size()) - 1);
    p.col = std::min(p.col, lines_[p.line].size());
    return p;
}

std::string CodeEditor::selectedText() const {
    auto [a, b] = selection();
    if (a.line == b.line) return lines_[a.line].substr(a.col, b.col - a.col);
    std::string s = lines_[a.line].substr(a.col);
    for (int l = a.line + 1; l < b.line; ++l) s += "\n" + lines_[l];
    s += "\n" + lines_[b.line].substr(0, b.col);
    return s;
}

void CodeEditor::eraseSelection() {
    if (!hasSelection()) return;
    auto [a, b] = selection();
    lines_[a.line] = lines_[a.line].substr(0, a.col) + lines_[b.line].substr(b.col);
    lines_.erase(lines_.begin() + a.line + 1, lines_.begin() + b.line + 1);
    caret_ = anchor_ = a;
    changed_ = true;
}

void CodeEditor::insert(std::string_view s) {
    eraseSelection();
    std::string tail = lines_[caret_.line].substr(caret_.col);
    lines_[caret_.line].resize(caret_.col);
    size_t start = 0;
    while (true) {
        size_t nl = s.find('\n', start);
        std::string_view part = s.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        std::string clean;
        for (char c : part)
            if (c == '\t') clean += "  ";
            else if (uint8_t(c) >= 0x20 || (uint8_t(c) & 0x80)) clean += c;
        lines_[caret_.line] += clean;
        caret_.col = lines_[caret_.line].size();
        if (nl == std::string_view::npos) break;
        lines_.insert(lines_.begin() + caret_.line + 1, "");
        ++caret_.line;
        start = nl + 1;
    }
    lines_[caret_.line] += tail;
    anchor_ = caret_;
    changed_ = true;
}

void CodeEditor::pushUndo(int kind, double now) {
    // Typing and deleting coalesce into one step until a pause or a different kind of edit.
    if (kind != 0 && kind == lastKind_ && now - lastPush_ < 1.0) {
        lastPush_ = now;
        return;
    }
    undo_.push_back({lines_, caret_, anchor_});
    if (undo_.size() > 500) undo_.erase(undo_.begin());
    redo_.clear();
    lastKind_ = kind;
    lastPush_ = now;
}

void CodeEditor::handleKeys(Context& ui) {
    const double now = ui.now();
    std::string_view typed = ui.input().text;
    for (const KeyChord& k : ui.input().keys) {
        bool shift = k.mods & ModShift;
        bool word = k.mods & ModAlt;
        bool cmd = k.mods & (ModGui | ModCtrl);
        uint8_t base = k.mods & ~ModShift;
        bool keepX = false;
        auto move = [&](Pos p) {
            caret_ = clampPos(p);
            if (!shift) anchor_ = caret_;
            ensureVisible_ = true;
        };
        std::string& line = lines_[caret_.line];
        switch (k.key) {
            case SDL_SCANCODE_LEFT:
                if (!shift && hasSelection() && !cmd && !word) move(selection().first);
                else if (cmd) {
                    size_t indent = line.find_first_not_of(' ');
                    if (indent == std::string::npos) indent = line.size();
                    move({caret_.line, caret_.col > indent ? indent : 0});
                } else if (word) {
                    if (caret_.col == 0 && caret_.line > 0) move({caret_.line - 1, lines_[caret_.line - 1].size()});
                    else move({caret_.line, prevWord(line, caret_.col)});
                } else if (caret_.col == 0 && caret_.line > 0) move({caret_.line - 1, lines_[caret_.line - 1].size()});
                else move({caret_.line, prevBoundary(line, caret_.col)});
                break;
            case SDL_SCANCODE_RIGHT:
                if (!shift && hasSelection() && !cmd && !word) move(selection().second);
                else if (cmd) move({caret_.line, line.size()});
                else if (caret_.col >= line.size() && caret_.line + 1 < int(lines_.size())) move({caret_.line + 1, 0});
                else move({caret_.line, word ? nextWord(line, caret_.col) : nextBoundary(line, caret_.col)});
                break;
            case SDL_SCANCODE_UP:
            case SDL_SCANCODE_DOWN:
            case SDL_SCANCODE_PAGEUP:
            case SDL_SCANCODE_PAGEDOWN: {
                int delta = k.key == SDL_SCANCODE_UP ? -1 : k.key == SDL_SCANCODE_DOWN ? 1 : k.key == SDL_SCANCODE_PAGEUP ? -30 : 30;
                if (cmd && (k.key == SDL_SCANCODE_UP || k.key == SDL_SCANCODE_DOWN)) {
                    move(delta < 0 ? Pos{0, 0} : Pos{int(lines_.size()) - 1, lines_.back().size()});
                    break;
                }
                auto stops = ui.painter.caretStops(line, FontRole::Mono);
                if (preferredX_ < 0) preferredX_ = stops[caret_.col];
                int target = std::clamp(caret_.line + delta, 0, int(lines_.size()) - 1);
                auto tstops = ui.painter.caretStops(lines_[target], FontRole::Mono);
                size_t best = 0;
                for (size_t i = 0; i < tstops.size(); ++i)
                    if (std::abs(tstops[i] - preferredX_) < std::abs(tstops[best] - preferredX_)) best = i;
                move({target, best});
                keepX = true;
                break;
            }
            case SDL_SCANCODE_HOME: move(cmd ? Pos{0, 0} : Pos{caret_.line, 0}); break;
            case SDL_SCANCODE_END: move(cmd ? Pos{int(lines_.size()) - 1, lines_.back().size()} : Pos{caret_.line, line.size()}); break;
            case SDL_SCANCODE_BACKSPACE:
                pushUndo(2, now);
                if (hasSelection()) eraseSelection();
                else if (cmd) {
                    anchor_ = {caret_.line, 0};
                    eraseSelection();
                } else if (caret_.col == 0 && caret_.line > 0) {
                    anchor_ = {caret_.line - 1, lines_[caret_.line - 1].size()};
                    eraseSelection();
                } else {
                    size_t from = word ? prevWord(line, caret_.col) : prevBoundary(line, caret_.col);
                    // Deleting inside leading spaces removes a whole indent level.
                    if (!word && caret_.col >= 2 && line.substr(0, caret_.col).find_first_not_of(' ') == std::string::npos)
                        from = caret_.col - (caret_.col % 2 ? 1 : 2);
                    anchor_ = {caret_.line, from};
                    eraseSelection();
                }
                ensureVisible_ = true;
                break;
            case SDL_SCANCODE_DELETE:
                pushUndo(2, now);
                if (!hasSelection()) {
                    if (caret_.col >= line.size() && caret_.line + 1 < int(lines_.size())) anchor_ = {caret_.line + 1, 0};
                    else anchor_ = {caret_.line, word ? nextWord(line, caret_.col) : nextBoundary(line, caret_.col)};
                }
                eraseSelection();
                break;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER: {
                if (cmd) break;  // Cmd+Enter belongs to the view
                pushUndo(3, now);
                size_t indent = line.find_first_not_of(' ');
                if (indent == std::string::npos) indent = line.size();
                indent = std::min(indent, caret_.col);
                std::string before = line.substr(0, caret_.col);
                std::string extra;
                auto ends = [&](std::string_view w) {
                    std::string t = before;
                    while (!t.empty() && t.back() == ' ') t.pop_back();
                    return t.ends_with(w);
                };
                if (ends("then") || ends("do") || (ends(")") && before.find("function") != std::string::npos) || ends("{") || ends("else") || ends("repeat"))
                    extra = "  ";
                insert("\n" + std::string(indent, ' ') + extra);
                ensureVisible_ = true;
                break;
            }
            case SDL_SCANCODE_TAB:
                pushUndo(3, now);
                if (shift || hasSelection()) {
                    auto [a, b] = selection();
                    for (int l = a.line; l <= b.line; ++l) {
                        if (shift) {
                            size_t n = 0;
                            while (n < 2 && n < lines_[l].size() && lines_[l][n] == ' ') ++n;
                            lines_[l].erase(0, n);
                        } else {
                            lines_[l].insert(0, "  ");
                        }
                    }
                    caret_ = clampPos(caret_);
                    anchor_ = clampPos(anchor_);
                    changed_ = true;
                } else {
                    insert(std::string(2 - caret_.col % 2, ' '));
                }
                break;
            case SDL_SCANCODE_A:
                if (base == ModGui || base == ModCtrl) {
                    anchor_ = {0, 0};
                    caret_ = {int(lines_.size()) - 1, lines_.back().size()};
                }
                break;
            case SDL_SCANCODE_C:
            case SDL_SCANCODE_X:
                if ((base == ModGui || base == ModCtrl) && hasSelection()) {
                    SDL_SetClipboardText(selectedText().c_str());
                    if (k.key == SDL_SCANCODE_X) {
                        pushUndo(0, now);
                        eraseSelection();
                    }
                }
                break;
            case SDL_SCANCODE_V:
                if (base == ModGui || base == ModCtrl) {
                    char* t = SDL_GetClipboardText();
                    if (t && *t) {
                        pushUndo(0, now);
                        insert(t);
                        ensureVisible_ = true;
                    }
                    SDL_free(t);
                }
                break;
            case SDL_SCANCODE_Z:
                if (cmd) {
                    auto& from = shift ? redo_ : undo_;
                    auto& to = shift ? undo_ : redo_;
                    if (!from.empty()) {
                        to.push_back({lines_, caret_, anchor_});
                        lines_ = std::move(from.back().lines);
                        caret_ = from.back().caret;
                        anchor_ = from.back().anchor;
                        from.pop_back();
                        lastKind_ = 0;
                        changed_ = true;
                        ensureVisible_ = true;
                    }
                }
                break;
            default: break;
        }
        if (!keepX) preferredX_ = -1;
    }
    if (!typed.empty()) {
        pushUndo(1, now);
        insert(typed);
        preferredX_ = -1;
        ensureVisible_ = true;
    }
}

CodeEditor::Pos CodeEditor::posAt(Context& ui, const Rect& area, Vec m) const {
    float lh = ui.painter.lineHeight(FontRole::Mono);
    int line = std::clamp(int(std::floor((m.y - area.y + scrollY_) / lh)), 0, int(lines_.size()) - 1);
    auto stops = ui.painter.caretStops(lines_[line], FontRole::Mono);
    float x = m.x - area.x + scrollX_;
    size_t best = 0;
    for (size_t i = 0; i < stops.size(); ++i)
        if (std::abs(stops[i] - x) < std::abs(stops[best] - x)) best = i;
    return {line, best};
}

bool CodeEditor::draw(Context& ui, const Rect& r, std::string_view label, std::span<const CodeMarker> markers) {
    id_ = ui.id(label);
    changed_ = false;
    Painter& p = ui.painter;
    float lh = p.lineHeight(FontRole::Mono);
    float digitW = p.measure("0", FontRole::Mono);
    int digits = std::max(3, int(std::to_string(lines_.size()).size()));
    Rect body = r;
    Rect gutter = cutLeft(body, std::round(digitW * digits + ui.dp(16)));
    Rect area = body.inset(ui.dp(6), 0);
    area.y += ui.dp(4);
    area.h -= ui.dp(4);

    Hit h = ui.interact(id_, r);
    if (h.hovered && ui.mouseIn(body)) ui.setCursor(Cursor::Text);
    if (h.pressed) {
        ui.focus(id_);
        caret_ = posAt(ui, area, ui.mouse());
        if (!(ui.input().mods & ModShift)) anchor_ = caret_;
        if (h.doubleClicked) {
            const std::string& l = lines_[caret_.line];
            anchor_ = {caret_.line, prevWord(l, std::min(l.size(), nextWord(l, caret_.col)))};
            caret_ = {caret_.line, nextWord(l, caret_.col)};
        }
        preferredX_ = -1;
    } else if (h.held) {
        caret_ = posAt(ui, area, ui.mouse());
        ensureVisible_ = true;
    }
    if (ui.hasFocus(id_)) {
        ui.setTyping();
        handleKeys(ui);
    }
    if (pendingGoto_ > 0) {
        caret_ = anchor_ = clampPos({pendingGoto_ - 1, 0});
        pendingGoto_ = -1;
        ensureVisible_ = true;
    }

    float contentH = lines_.size() * lh + ui.dp(8);
    Vec wheel = ui.takeWheel(r);
    scrollY_ -= wheel.y * ui.dp(48);
    scrollX_ -= wheel.x * ui.dp(48);
    if (ensureVisible_) {
        float cy = caret_.line * lh;
        if (cy < scrollY_) scrollY_ = cy;
        if (cy + lh > scrollY_ + area.h) scrollY_ = cy + lh - area.h;
        float cx = p.caretStops(lines_[caret_.line], FontRole::Mono)[caret_.col];
        if (cx < scrollX_) scrollX_ = std::max(0.0f, cx - ui.dp(40));
        if (cx > scrollX_ + area.w - ui.dp(8)) scrollX_ = cx - area.w + ui.dp(40);
        ensureVisible_ = false;
    }
    scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, contentH - area.h));
    scrollX_ = std::max(0.0f, scrollX_);
    scrollY_ = std::round(scrollY_);
    scrollX_ = std::round(scrollX_);

    p.fill(r, ui.col(Slot::Field));
    p.fill(gutter, ui.col(Slot::Background));
    p.vline(gutter.r() - 1, gutter.y, gutter.b(), ui.col(Slot::Border));

    int first = std::max(0, int(scrollY_ / lh));
    int last = std::min(int(lines_.size()) - 1, int((scrollY_ + area.h) / lh) + 1);
    auto [selA, selB] = selection();
    bool focused = ui.hasFocus(id_);

    // Block comment state for the first visible line.
    bool inBlock = false;
    for (int l = 0; l < first; ++l) tokenize(lines_[l], inBlock);

    p.pushClip(gutter);
    for (int l = first; l <= last; ++l) {
        float y = area.y + l * lh - scrollY_;
        bool marked = std::ranges::any_of(markers, [&](const CodeMarker& m) { return m.line == l + 1; });
        std::string num = std::to_string(l + 1);
        Color c = marked ? ui.col(Slot::Error) : l == caret_.line && focused ? ui.col(Slot::TextDim) : ui.col(Slot::LineNumber);
        p.text(gutter.r() - ui.dp(8) - p.measure(num, FontRole::Mono), y, num, c, FontRole::Mono);
    }
    p.popClip();

    p.pushClip(body);
    for (int l = first; l <= last; ++l) {
        float y = area.y + l * lh - scrollY_;
        const std::string& line = lines_[l];
        for (const auto& m : markers)
            if (m.line == l + 1) p.fill({body.x, y, body.w, lh}, ui.col(Slot::ErrorBackground));
        auto stops = p.caretStops(line, FontRole::Mono);
        float x0 = area.x - scrollX_;
        if (hasSelection() && l >= selA.line && l <= selB.line) {
            size_t a = l == selA.line ? selA.col : 0;
            size_t b = l == selB.line ? selB.col : line.size();
            float extra = l < selB.line ? digitW : 0;
            p.fill({x0 + stops[a], y, stops[b] - stops[a] + extra, lh}, ui.col(Slot::TextSelection));
        }
        for (const Token& t : tokenize(line, inBlock))
            p.text(x0 + stops[t.begin], y, std::string_view(line).substr(t.begin, t.end - t.begin), ui.col(t.color), FontRole::Mono);
        if (focused && l == caret_.line) p.fill({x0 + stops[caret_.col], y, std::max(1.0f, ui.dp(1.5f)), lh}, ui.col(Slot::Caret));
    }
    // Marker messages sit at the end of their line.
    for (const auto& m : markers) {
        int l = m.line - 1;
        if (l < first || l > last) continue;
        float y = area.y + l * lh - scrollY_;
        float w = p.measure(m.message, FontRole::Mono);
        float x = std::max(area.x - scrollX_ + p.measure(lines_[l], FontRole::Mono) + ui.dp(24), body.r() - w - ui.dp(12));
        p.text(x, y, m.message, ui.col(Slot::Error), FontRole::Mono);
    }
    p.popClip();

    if (changed_) lastEdit_ = ui.now();
    return changed_;
}

}  // namespace mf::ui
