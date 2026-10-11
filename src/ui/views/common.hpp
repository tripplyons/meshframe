#pragma once

#include "model/edit.hpp"
#include "model/schema.hpp"
#include "ui/shell.hpp"
#include "ui/view.hpp"
#include "ui/widgets.hpp"

#include <optional>
#include <string>

namespace mf::ui::views {

// Property lookups by owner id (clip, effect, or transition) in a project.
const Property* findProperty(const Project& p, Id target, const std::string& name);
// Start of the clip-local time base for a target: the clip's start, the owning
// clip's start for an effect, or the transition window's start.
double targetStart(const Project& p, Id target);
// Evaluated value of a property at the evaluator's current time.
Value evaluatedValue(Evaluator& ev, Id target, const std::string& name);
// The graph error of a property, if any.
std::string propertyError(const Evaluator& ev, Id target, const std::string& name);

// Fill color of a clip on the timeline.
Slot clipSlot(const Clip& c, TrackKind track);

// A number field whose drags become one undo step and whose typed values
// commit as one edit. Returns true when the project changed.
bool editNumber(ViewEnv& env, const Rect& r, std::string_view id, double value, const NumberStyle& style,
                const std::string& label, const std::function<void(Project&, double)>& apply);

// One editable property: name, mode menu, value editor, and keyframe toggle.
// Drags become one undo step. Read-only rows show the value without editors.
void propertyRow(ViewEnv& env, const Rect& row, Id target, const PropSpec& spec, float labelWidth, bool readOnly = false);

// Draws a field label on the left of row and returns the editor area.
Rect labeled(Context& ui, Rect row, std::string_view label, float labelWidth);

// A section title row with an optional count; returns the rest of the row.
Rect sectionHeader(Context& ui, Rect& area, std::string_view title);

// Draws text lines top to bottom in area; returns the height used.
float paragraph(Context& ui, const Rect& area, std::string_view text, Slot color);

// A thin scrollable vertical list layout. Call row() for each item; it returns
// the item's rect (possibly outside the view) and advances.
class ListLayout {
public:
    ListLayout(Context& ui, const Rect& view, std::string_view id, float contentHeight);
    ~ListLayout();
    Rect next(float h);
    bool visible(const Rect& r) const { return r.b() > view_.y && r.y < view_.b(); }
    float width() const { return view_.w; }

private:
    Context& ui_;
    Rect view_;
    float y_;
};

}  // namespace mf::ui::views
