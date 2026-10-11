#pragma once

#include "ui/geometry.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mf::ui {

// A layout is a tree of splits. Leaves are pane slots numbered in reading order.
struct LayoutNode {
    enum class Kind { Pane, Row, Column } kind = Kind::Pane;  // Row: side by side; Column: stacked
    std::vector<LayoutNode> children;
    std::vector<float> weights;  // default sizes of the children
};

struct LayoutPreset {
    std::string name;
    std::string label;
    LayoutNode root;
    std::vector<std::string> views;  // default view per pane
    int panes() const { return int(views.size()); }
};

const std::vector<LayoutPreset>& layoutPresets();
const LayoutPreset* findLayout(std::string_view name);

// Per-pane state owned by the view layer (scroll, zoom, editor buffers).
struct PaneState {
    virtual ~PaneState() = default;
};

struct Pane {
    std::string view;
    std::unique_ptr<PaneState> state;
};

struct Space {
    std::string layout;
    // May hold more panes than the layout shows; extra panes are kept so a
    // round trip through a smaller layout does not lose views.
    std::vector<Pane> panes;
    // Divider positions as fractions of each split, in depth-first order.
    std::vector<float> dividers;
};

// A divider between two children of a split; dragging it moves `index` in Space::dividers.
struct Divider {
    Rect rect;       // the 1 px border line
    Rect split;      // the split's whole area
    bool vertical;   // a vertical line between side-by-side panes
    int index;       // into Space::dividers
    float min, max;  // allowed fraction range
};

struct LayoutRects {
    std::vector<Rect> panes;
    std::vector<Divider> dividers;
};

// Ten spaces, i3 style. Space n (1-10) is index n-1; key 0 maps to space 10.
class Workspace {
public:
    static constexpr int kSpaces = 10;

    Workspace();

    int current() const { return current_; }
    Space& space(int index) { return spaces_[index]; }
    const Space& space(int index) const { return spaces_[index]; }
    Space& active() { return spaces_[current_]; }

    void switchTo(int index);
    // Swaps the contents of the current space and `index`, then shows `index`,
    // so the content the user was looking at follows them.
    void swapWith(int index);
    // Changes a space's layout. Existing panes keep their views in order; new
    // slots take the preset's defaults.
    void setLayout(int index, const std::string& layout);
    void setPaneView(int space, int pane, const std::string& view);

    // Pane rectangles and dividers for a space inside `area`, separated by 1 px borders.
    static LayoutRects compute(const Space& s, const Rect& area, float minPane);

    nlohmann::json toJson() const;
    void loadJson(const nlohmann::json& j);
    // Restores the built-in arrangement of all spaces.
    void reset();

private:
    std::array<Space, kSpaces> spaces_;
    int current_ = 0;
};

// Divider count a layout needs.
int dividerCount(const LayoutNode& n);
std::vector<float> defaultDividers(const LayoutNode& n);

}  // namespace mf::ui
