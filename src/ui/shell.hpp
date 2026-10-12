#pragma once

#include "session/commands.hpp"
#include "session/recovery.hpp"
#include "ui/actions.hpp"
#include "ui/context.hpp"
#include "ui/settings.hpp"
#include "ui/view.hpp"
#include "ui/workspace.hpp"

#include <functional>
#include <memory>
#include <optional>
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
        // Shows a file in the system file manager.
        std::function<void(const std::string& path)> reveal;
        std::string revealLabel = "Show in folder";
    };

    Shell(Session& session, Painter& painter, Settings settings, std::string settingsPath);
    ~Shell();

    // Ticks the session and draws one frame into the painter's renderer.
    void frame(const Input& in, Rect screen, float displayScale, double now);
    // Call after frame() and before presenting: saves requested screenshots.
    void afterDraw();
    // Runs the action bound to a key. Returns true when one ran, or when the
    // key was taken by a binding capture.
    bool handleKey(const KeyChord& c);
    // True while something animates or waits on the next frame.
    bool wantsFrame() const;
    bool quitRequested() const { return quit_; }
    // Quits, after asking to save unsaved changes.
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
    const std::string& settingsPath() const { return settingsPath_; }

    // Rebinding by key press: the next key binds `action` instead of running
    // an action. `index` replaces that chord; -1 appends one. Escape or a
    // mouse press cancels.
    struct BindingCapture {
        std::string action;
        int index = -1;
    };
    void captureBinding(std::string action, int index);
    void cancelCapture() { capture_.reset(); }
    const std::optional<BindingCapture>& capture() const { return capture_; }
    // Binds a chord to an action and removes it from any other action, so one
    // chord runs one action. Saves the bindings.
    void bindChord(const std::string& action, const KeyChord& c, int index = -1);
    // Marks the workspace changed so it is saved with the settings.
    void workspaceChanged();

    // A short message in the status bar; errors also go to the log.
    void notify(std::string text, bool error = false);
    // A message with a button that runs `action`. It stays longer than a plain one.
    void notify(std::string text, std::string actionLabel, std::function<void()> action);
    // Runs a project edit and reports an EditError in the status bar.
    bool edit(const std::string& label, const std::function<void(Project&)>& fn);

    // File operations that may show a dialog. New and open ask to save
    // unsaved changes first. `then` runs after a successful save.
    void newProject();
    void openProject();
    void openProjectFile(const std::string& path);
    // Project files opened or saved lately, most recent first.
    const std::vector<std::string>& recentProjects() const { return settings_.recent; }
    void clearRecentProjects();
    void saveProject(bool saveAs, std::function<void()> then = {});
    void importMedia();

    // Runs `next` now when the project has no unsaved changes. Otherwise a
    // dialog asks to save them, discard them, or cancel; `next` runs after
    // a save or a discard. `action` completes "before ...", as in "quitting".
    void confirmUnsaved(std::string action, std::function<void()> next);
    // Picks a dialog choice by id, such as "save". False when no open dialog has it.
    bool answerDialog(std::string_view choice);
    bool dialogOpen() const { return dialog_.has_value(); }

    // Keeps copies of unsaved work in `dir`, and offers to restore copies
    // that an editor left there when it did not quit normally.
    void enableRecovery(std::string dir);

    // The command palette: a searchable list of every action.
    void openPalette();
    void closePalette();
    bool paletteOpen() const { return palette_.has_value(); }

    // "Cmd+E" for the first key bound to an action, or "".
    std::string shortcutText(std::string_view action) const;
    // "Export (Cmd+E)", or text alone when the action has no key.
    std::string withShortcut(std::string_view text, std::string_view action) const;

    int focusedPane() const { return focusedPane_; }
    // The view in a pane of the active space, created on demand.
    View* paneView(int pane);
    // Sends an action to the focused pane when it shows `type`, else to the
    // first pane of that type in the active space.
    bool routeToView(std::string_view type, std::string_view action);
    // Shows a view: focuses a pane that has it, or switches the focused pane to it.
    void showView(const std::string& type);

    // GUI commands for the CLI: ui.state, ui.space, ui.swap, ui.layout, ui.pane,
    // ui.actions, ui.action, ui.bind, ui.theme, ui.settings, ui.dialog, ui.screenshot.
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
    void drawRecentMenu(WidgetId id);
    void drawNotice(Rect area);
    void drawDialog();
    nlohmann::json dialogJson() const;
    bool noticeShown() const;
    nlohmann::json noticeJson() const;
    void drawPalette();
    void runPaletteItem(int index);
    // Offers the next copy left by an editor that did not quit normally.
    void offerRecovery();
    // Tracks the project file and export state that frame() reports on.
    void watchSession();
    void markSettingsDirty();
    // The project's file name, or its name before the first save.
    std::string projectTitle() const;

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
    // A modal question. The first choice is the default (Return); choices
    // marked `left` sit apart on the left, for ones that lose work.
    struct Choice {
        std::string id, label;
        KeyChord key;
        std::function<void()> run;
        bool left = false;
    };
    struct Dialog {
        std::string text, detail;
        std::vector<Choice> choices;
    };
    std::optional<Dialog> dialog_;
    void showDialog(Dialog d);

    struct Palette {
        std::string query;
        std::string searched;              // the query `matches` belongs to
        std::vector<std::string> matches;  // action ids
        int selected = 0;
        bool focus = true;                 // give the search field focus on the next draw
        WidgetId field = 0;
    };
    std::optional<Palette> palette_;

    std::unique_ptr<Recovery> recovery_;
    std::vector<Recovery::Copy> orphans_;

    std::string watchedPath_;
    uint64_t exportsSeen_ = 0;  // ExportJob::started() when the last result was reported
    double exportedAt_ = -100;

    std::string notice_;
    bool noticeError_ = false;
    double noticeAt_ = -10;
    std::string noticeActionLabel_;
    std::function<void()> noticeAction_;
    bool settingsDirty_ = false;
    double settingsDirtyAt_ = 0;

    struct Screenshot {
        std::string path;
        Reply reply;
    };
    std::vector<Screenshot> screenshots_;
    std::optional<BindingCapture> capture_;
};

// `path` with the home directory written as "~".
std::string shortPath(const std::string& path);

}  // namespace mf::ui
