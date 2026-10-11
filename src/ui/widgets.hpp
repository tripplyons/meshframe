#pragma once

#include "ui/context.hpp"

#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace mf::ui {

// The part of a label before "##", which only disambiguates ids.
std::string_view displayLabel(std::string_view label);

bool button(Context& ui, const Rect& r, std::string_view label, bool selected = false, bool enabled = true);
// A checkbox with a label. Returns true when toggled.
bool toggle(Context& ui, const Rect& r, std::string_view label, bool& value);
// Returns true when the choice changed.
bool dropdown(Context& ui, const Rect& r, std::string_view label, int& index, std::span<const std::string> options);

struct FieldResult {
    bool committed = false;  // Enter, Tab, or focus moved away with an edit
    bool canceled = false;   // Escape
    bool editing = false;    // has focus
};
FieldResult textField(Context& ui, const Rect& r, std::string_view label, std::string& value,
                      FontRole role = FontRole::Ui, std::string_view placeholder = {});

struct DragResult {
    bool began = false;      // first change of a drag
    bool changed = false;    // the value changed this frame
    bool ended = false;      // the drag that changed the value ended
    bool committed = false;  // a typed value was entered
};
struct NumberStyle {
    double step = 0.01;  // per pixel of drag
    double min = -std::numeric_limits<double>::infinity();
    double max = std::numeric_limits<double>::infinity();
    int decimals = 2;
    std::string_view unit;
};
// Drag horizontally to change; click to type a value.
DragResult numberField(Context& ui, const Rect& r, std::string_view label, double& value, const NumberStyle& style = {});
DragResult slider(Context& ui, const Rect& r, std::string_view label, double& value, double min, double max, int decimals = 2);

// A list row. `hit` receives the full interaction for drags and double clicks.
bool selectable(Context& ui, const Rect& r, std::string_view label, bool selected, Hit* hit = nullptr,
                FontRole role = FontRole::Ui);

// Vertical scrolling: clips to view and returns the offset to subtract from content y.
float beginScroll(Context& ui, const Rect& view, std::string_view label, float contentHeight);
void endScroll(Context& ui);
// Scrolls so that [y, y + h) in content coordinates is visible.
void scrollTo(Context& ui, std::string_view label, float y, float h, float viewHeight);

// Popup menus. Open with ui.openPopup(id, position).
bool beginMenu(Context& ui, WidgetId id, float width = 0);
bool menuItem(Context& ui, std::string_view label, std::string_view shortcut = {}, bool enabled = true, bool checked = false);
void menuSeparator(Context& ui);
void endMenu(Context& ui);

// Small glyph-free icons.
void drawChevronDown(Context& ui, Vec center, float size, const Color& c);

// Formats a number with a fixed count of decimals, trimming trailing zeros.
std::string formatNumber(double v, int decimals);

}  // namespace mf::ui
