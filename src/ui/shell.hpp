#pragma once

#include "session/commands.hpp"
#include "ui/actions.hpp"
#include "ui/context.hpp"
#include "ui/settings.hpp"
#include "ui/view.hpp"
#include "ui/workspace.hpp"

#include <functional>
#include <string>
#include <vector>

namespace mf::ui {

// The whole window: panes of the active space, dividers, and the status bar.
// The app feeds it input and presents what it draws; tests drive it on a
// software renderer.
class Shell {
public:
    // Platform services the app provides. Tests leave them empty.
    struct Hooks {
        // A native file dialog. `done` gets the chosen path, or "" on cancel.
        std::function<void(bool save, const std::string& title, const std::vector<std::string>& extensions,
                           std::function<void(std::string)> done)>
            fileDialog;
    };

    Shell(Session& session, Painter& painter, Settings settings, std::string settingsPath);
    ~Shell();

    // Ticks the session and draws one frame into the painter's renderer.
    void frame(const Input& in, Rect screen, float displayScale, double now);
    // Call after frame() and before presenting: saves requested screenshots.
    void afterDraw();
    // Runs the action bound to a key. Returns true when one ran.
    bool handleKey(const KeyChord& c);
    // True while something animates or waits on the next frame.
    bool wantsFrame() const;
    bool quitRequested() const { return quit_; }
    // Asks to quit; a dirty project needs a second request.
    void requestQuit();

    Session& session() { return session_; }
    Context& ui() { return ui_; }
    Workspace& workspace() { return workspace_; }
    Actions& actions() { return actions_; }
    Settings& settings() { return settings_; }
    Hooks hooks;

    // Applies changed settings (theme, fonts, bindings) and saves them soon.
    void settingsChanged();
    // Saves settings now when they have a path.
    void saveSettings();
    // Marks the workspace changed so it is saved with the settings.
    void workspaceChanged();

    // A short message in the status bar; errors also go to the log.
    void notify(std::string text, bool error = false);
    // Runs a project edit and reports an EditError in the status bar.
    bool edit(const std::string& label, const std::function<void(Project&)>& fn);

    // File operations that may show a dialog.
    void newProject();
    void openProject();
    void saveProject(bool saveAs);
    void importMedia();

    int focusedPane() const { return focusedPane_; }
    // The view in a pane of the active space, created on demand.
    View* paneView(int pane);
    // Sends an action to the focused pane when it shows `type`, else to the
    // first pane of that type in the active space.
    bool routeToView(std::string_view type, std::string_view action);
    // Shows a view: focuses a pane that has it, or switches the focused pane to it.
    void showView(const std::string& type);

    // GUI commands for the CLI: ui.state, ui.space, ui.swap, ui.layout, ui.pane,
    // ui.actions, ui.action, ui.bind, ui.theme, ui.settings, ui.screenshot.
    void registerCommands(Commands& commands);
    nlohmann::json state() const;
    // The registered command set, for the console view. Null before registerCommands.
    Commands* commands() { return commands_; }

    // Window size in physical pixels of the last frame.
    Rect screen() const { return screen_; }

private:
    void registerActions();
    void applyFonts(float scale);
    void drawPanes(const Rect& area);
    void drawStatus(const Rect& bar);
    void drawFileMenu(WidgetId id);

    Session& session_;
    Painter& painter_;
    Commands* commands_ = nullptr;
    Settings settings_;
    std::string settingsPath_;
    Context ui_;
    Workspace workspace_;
    Actions actions_;

    Rect screen_;
    float fontScale_ = 0;
    bool fontsDirty_ = true;
    int focusedPane_ = 0;
    bool quit_ = false;
    double quitAskedAt_ = -10;
    std::string notice_;
    bool noticeError_ = false;
    double noticeAt_ = -10;
    bool settingsDirty_ = false;
    double settingsDirtyAt_ = 0;

    struct Screenshot {
        std::string path;
        Reply reply;
    };
    std::vector<Screenshot> screenshots_;
};

}  // namespace mf::ui
