#pragma once

#include "session/session.hpp"
#include "ui/context.hpp"
#include "ui/workspace.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mf::ui {

class Shell;

// What a view gets each frame.
struct ViewEnv {
    Context& ui;
    Session& session;
    Shell& shell;
    bool focused;  // the pane was clicked last
};

// The content of a pane. Each pane owns its view, so two timelines can have
// different zoom levels, and a view moves with its pane when spaces swap.
class View : public PaneState {
public:
    // Draws the body; the pane clips to it.
    virtual void draw(ViewEnv& env, const Rect& body) = 0;
    // Optional controls in the pane header, right of the view selector.
    virtual void header(ViewEnv&, const Rect&) {}
    // Runs a view action such as "zoom-in". Returns true when handled.
    virtual bool command(ViewEnv&, std::string_view) { return false; }
};

struct ViewType {
    std::string id;     // "timeline"
    std::string label;  // "Timeline"
    std::function<std::unique_ptr<View>()> make;
};

const std::vector<ViewType>& viewTypes();
const ViewType* findViewType(std::string_view id);

}  // namespace mf::ui
