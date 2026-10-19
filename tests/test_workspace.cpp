#include "fixtures.hpp"
#include "model/serialize.hpp"
#include "shell_harness.hpp"
#include "ui/workspace.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

using namespace mf;
using namespace mf::ui;

namespace {

bool waitForRecovery(const std::function<bool()>& ready) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return ready();
}

struct PaneMarker : PaneState {
    int value = 0;
    explicit PaneMarker(int v) : value(v) {}
};

}  // namespace

TEST(Workspace, SwitchAndSwapFollow) {
    Workspace ws;
    EXPECT_EQ(ws.current(), 0);
    EXPECT_EQ(ws.space(0).layout, "editor");
    EXPECT_EQ(ws.space(2).layout, "code");
    ws.space(0).panes[1].state = std::make_unique<PaneMarker>(7);

    ws.switchTo(4);
    EXPECT_EQ(ws.current(), 4);
    ws.switchTo(10);
    EXPECT_EQ(ws.current(), 4) << "out of range is ignored";

    // Swap space 1 with space 3 while on space 1: the content moves to 3 and so does the user.
    ws.switchTo(0);
    ws.swapWith(2);
    EXPECT_EQ(ws.current(), 2);
    EXPECT_EQ(ws.space(2).layout, "editor");
    EXPECT_EQ(ws.space(0).layout, "code");
    auto* m = dynamic_cast<PaneMarker*>(ws.space(2).panes[1].state.get());
    ASSERT_NE(m, nullptr) << "pane state follows the swap";
    EXPECT_EQ(m->value, 7);

    ws.swapWith(2);
    EXPECT_EQ(ws.current(), 2) << "swapping with the current space does nothing";
    EXPECT_EQ(ws.space(2).layout, "editor");
}

TEST(Workspace, LayoutChangesKeepPaneViews) {
    Workspace ws;
    ws.setPaneView(0, 3, "graph");
    ws.setLayout(0, "single");
    EXPECT_EQ(ws.space(0).panes.size(), 4u);
    EXPECT_EQ(Workspace::compute(ws.space(0), {0, 0, 100, 100}, 10).panes.size(), 1u);
    ws.setLayout(0, "editor");
    EXPECT_EQ(ws.space(0).panes[3].view, "graph");
    ws.setLayout(0, "bogus");
    EXPECT_EQ(ws.space(0).layout, "editor");
    ws.setLayout(5, "grid-2x2");
    EXPECT_EQ(ws.space(5).panes[0].view, "settings") << "existing panes keep their view";
    EXPECT_EQ(ws.space(5).panes[1].view, "audio-effects") << "new panes take the preset default";
}

TEST(Workspace, PanesAndBordersTileTheAreaExactly) {
    for (const auto& preset : layoutPresets()) {
        Workspace ws;
        ws.setLayout(0, preset.name);
        for (float width : {1001.0f, 640.0f, 333.0f}) {
            Rect area{0, 0, width, 401};
            LayoutRects lr = Workspace::compute(ws.space(0), area, 20);
            ASSERT_EQ(int(lr.panes.size()), preset.panes()) << preset.name;
            std::vector<int> cover(size_t(area.w * area.h), 0);
            auto paint = [&](const Rect& r) {
                EXPECT_EQ(r.x, std::round(r.x));
                EXPECT_EQ(r.y, std::round(r.y));
                EXPECT_EQ(r.w, std::round(r.w));
                EXPECT_EQ(r.h, std::round(r.h));
                for (int y = int(r.y); y < int(r.b()); ++y)
                    for (int x = int(r.x); x < int(r.r()); ++x) ++cover[size_t(y * area.w + x)];
            };
            for (const auto& r : lr.panes) {
                EXPECT_GE(r.w, 20) << preset.name;
                EXPECT_GE(r.h, 20) << preset.name;
                paint(r);
            }
            // A divider crossing a perpendicular one shares its pixels only where the
            // outer split's border runs; inner dividers stop at it.
            for (const auto& d : lr.dividers) paint(d.rect);
            int gaps = 0, overlaps = 0;
            for (int c : cover) gaps += c == 0, overlaps += c > 1;
            EXPECT_EQ(gaps, 0) << preset.name << " at width " << width;
            EXPECT_EQ(overlaps, 0) << preset.name << " at width " << width;
        }
    }
}

TEST(Workspace, JsonRoundTripAndBadInput) {
    Workspace ws;
    ws.setLayout(3, "columns-3");
    ws.setPaneView(3, 2, "console");
    ws.space(3).dividers = {0.2f, 0.7f};
    ws.switchTo(3);
    Workspace back;
    back.loadJson(ws.toJson());
    EXPECT_EQ(back.toJson(), ws.toJson());
    EXPECT_EQ(back.current(), 3);

    Workspace bad;
    bad.loadJson({{"current", 99}, {"spaces", {{{"layout", "nope"}}, {{"layout", "rows-2"}, {"dividers", {"x", 5}}}}}});
    EXPECT_EQ(bad.current(), 0);
    EXPECT_EQ(bad.space(0).layout, "editor") << "an unknown layout keeps the default";
    EXPECT_EQ(bad.space(1).layout, "rows-2");
    EXPECT_EQ(bad.space(1).panes.size(), 2u);
    ASSERT_EQ(bad.space(1).dividers.size(), 1u);
    EXPECT_FLOAT_EQ(bad.space(1).dividers[0], 0.98f);
}

TEST(Shell, OptionKeysSwitchAndSwapSpaces) {
    ShellHarness h;
    h.frame();
    Workspace& ws = h.shell->workspace();
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_3, ModAlt}));
    EXPECT_EQ(ws.current(), 2);
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_0, ModAlt}));
    EXPECT_EQ(ws.current(), 9) << "Option+0 is space 10";
    h.shell->handleKey({SDL_SCANCODE_3, ModAlt});
    // Option+Shift+1 on space 3: space 1 gets the code layout and the user follows it.
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_1, ModAlt | ModShift}));
    EXPECT_EQ(ws.current(), 0);
    EXPECT_EQ(ws.space(0).layout, "code");
    EXPECT_EQ(ws.space(2).layout, "editor");
    h.frame();
    EXPECT_EQ(h.run({"ui.state"})["result"]["space"], 1);
}

TEST(Shell, CommandsControlSpacesLayoutsPanesAndBindings) {
    ShellHarness h;
    h.frame();
    auto r = h.run({"ui.layout", "rows-2", "space=5"});
    ASSERT_TRUE(r["ok"]) << r;
    r = h.run({"ui.pane", "2", "graph", "space=5"});
    ASSERT_TRUE(r["ok"]) << r;
    EXPECT_EQ(r["result"]["spaces"][4]["layout"], "rows-2");
    EXPECT_EQ(r["result"]["spaces"][4]["panes"], (json{"history", "graph"}));
    EXPECT_FALSE(h.run({"ui.pane", "3", "graph", "space=5"})["ok"]);
    auto bad = h.run({"ui.pane", "1", "nope"});
    EXPECT_FALSE(bad["ok"]);
    EXPECT_NE(bad["error"].get<std::string>().find("timeline"), std::string::npos) << "the error lists the views";
    EXPECT_FALSE(h.run({"ui.space", "11"})["ok"]);

    r = h.run({"ui.space", "5"});
    EXPECT_EQ(r["result"]["space"], 5);
    r = h.run({"ui.swap", "2"});
    EXPECT_EQ(r["result"]["space"], 2);
    EXPECT_EQ(r["result"]["spaces"][1]["layout"], "rows-2");
    EXPECT_EQ(r["result"]["spaces"][4]["layout"], "grid-2x2");

    r = h.run({"ui.bind", "space.1", "Ctrl+1, Alt+1"});
    ASSERT_TRUE(r["ok"]) << r;
    EXPECT_EQ(r["result"], (json{"Ctrl+1", "Alt+1"}));
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_1, ModCtrl}));
    EXPECT_EQ(h.shell->workspace().current(), 0);
    EXPECT_FALSE(h.run({"ui.bind", "space.1", "Ctrl+Nope"})["ok"]);
    h.run({"ui.bind", "space.1"});
    EXPECT_FALSE(h.shell->handleKey({SDL_SCANCODE_1, ModCtrl}));

    r = h.run({"ui.theme", "border", "#ff00ff"});
    EXPECT_EQ(r["result"], "#ff00ff");
    EXPECT_EQ(h.shell->settings().theme[Slot::Border], *parseColor("#ff00ff"));
    EXPECT_FALSE(h.run({"ui.theme", "nope", "#ffffff"})["ok"]);
    r = h.run({"ui.settings", R"(set={"fonts": {"uiSize": 15}})"});
    ASSERT_TRUE(r["ok"]) << r;
    EXPECT_EQ(h.shell->settings().uiSize, 15);

    r = h.run({"ui.action", "space.4"});
    EXPECT_EQ(r["result"]["space"], 4);
    EXPECT_FALSE(h.run({"ui.action", "no.such"})["ok"]);
    auto actions = h.run({"ui.actions"})["result"];
    bool found = false;
    for (const auto& a : actions)
        if (a["id"] == "space.swap.3") found = a["keys"] == json{"Alt+Shift+3"};
    EXPECT_TRUE(found);
}

TEST(Shell, ScreenshotHasSingleBordersAndNoGaps) {
    Settings s;
    s.theme[Slot::Border] = *parseColor("#ff00ff");
    ShellHarness h(s);
    h.run({"ui.layout", "editor"});
    h.frame();
    json reply;
    auto path = (fixtures::dir() / "ui" / "shell.png").string();
    std::filesystem::create_directories(fixtures::dir() / "ui");
    std::filesystem::remove(path);
    h.commands.run({{"cmd", "ui.screenshot"}, {"args", {{"path", path}}}}, [&](json j) { reply = std::move(j); });
    EXPECT_TRUE(reply.is_null()) << "the reply waits for a drawn frame";
    h.frame();
    ASSERT_TRUE(reply["ok"]) << reply;
    EXPECT_EQ(reply["result"]["width"], h.w);
    EXPECT_TRUE(std::filesystem::exists(path));

    auto isBorder = [&](int x, int y) {
        SDL_Color c = h.pixel(x, y);
        return c.r == 255 && c.g == 0 && c.b == 255;
    };
    // No two border lines touch: a 2x2 block of border pixels means a double border.
    int doubled = 0;
    for (int y = 0; y + 1 < h.h; ++y)
        for (int x = 0; x + 1 < h.w; ++x)
            doubled += isBorder(x, y) && isBorder(x + 1, y) && isBorder(x, y + 1) && isBorder(x + 1, y + 1);
    EXPECT_EQ(doubled, 0);
    // No frame along the window edges: an edge pixel is a border only where a
    // line runs into the edge, so its inward neighbor is a border too.
    for (int x = 0; x < h.w; ++x) EXPECT_TRUE(!isBorder(x, 0) || isBorder(x, 1)) << x;
    for (int y = 0; y < h.h; ++y) {
        EXPECT_TRUE(!isBorder(0, y) || isBorder(1, y)) << y;
        EXPECT_TRUE(!isBorder(h.w - 1, y) || isBorder(h.w - 2, y)) << y;
    }

    // Each divider is drawn, and the pixels beside it belong to the panes.
    LayoutRects lr = Workspace::compute(h.shell->workspace().active(), h.paneArea(), h.shell->ui().rowHeight() * 2 + 1);
    ASSERT_EQ(lr.dividers.size(), 3u);
    for (const auto& d : lr.dividers) {
        int mx = int(d.rect.x + d.rect.w / 2), my = int(d.rect.y + d.rect.h / 2);
        EXPECT_TRUE(isBorder(mx, my));
        if (d.vertical) EXPECT_FALSE(isBorder(mx - 1, my) || isBorder(mx + 1, my));
        else EXPECT_FALSE(isBorder(mx, my - 1) || isBorder(mx, my + 1));
    }
    // The status bar border runs the full width, one pixel high.
    int statusTop = int(h.paneArea().b());
    for (int x = 0; x < h.w; x += 7) EXPECT_TRUE(isBorder(x, statusTop)) << x;
}

TEST(Shell, DividerDragAndPaneViewSelector) {
    ShellHarness h;
    h.run({"ui.layout", "columns-2"});
    h.frame();
    LayoutRects lr = Workspace::compute(h.shell->workspace().active(), h.paneArea(), h.shell->ui().rowHeight() * 2 + 1);
    ASSERT_EQ(lr.dividers.size(), 1u);
    float x = lr.dividers[0].rect.x, y = 300;
    h.in.mouse = {x + 2, y};
    h.frame();
    h.in.down[0] = h.in.pressed[0] = true;
    h.frame();
    h.in.mouse = {x - 200, y};
    h.frame();
    h.in.down[0] = false;
    h.in.released[0] = true;
    h.frame();
    EXPECT_NEAR(h.shell->workspace().active().dividers[0], (x - 200) / h.w, 0.01);

    // Pick "Graph" from the second pane's view selector.
    Rect pane = Workspace::compute(h.shell->workspace().active(), h.paneArea(), h.shell->ui().rowHeight() * 2 + 1).panes[1];
    float row = h.shell->ui().rowHeight();
    h.click(pane.x + 20, row / 2);
    int graph = 0;
    for (size_t i = 0; i < viewTypes().size(); ++i)
        if (viewTypes()[i].id == "graph") graph = int(i);
    h.click(pane.x + 20, row + 4 + row * (graph + 0.5f));
    EXPECT_EQ(h.shell->workspace().active().panes[1].view, "graph");
    EXPECT_EQ(h.shell->focusedPane(), 1);
}

TEST(Shell, WorkspaceAndBindingsPersistInSettings) {
    auto path = (fixtures::dir() / "ui" / "shell-settings.json").string();
    std::filesystem::create_directories(fixtures::dir() / "ui");
    std::filesystem::remove(path);
    {
        ShellHarness h({}, path);
        h.frame();
        h.run({"ui.layout", "columns-3", "space=7"});
        h.run({"ui.pane", "1", "scopes", "space=7"});
        h.run({"ui.space", "7"});
        h.run({"ui.bind", "transport.toggle", "K"});
    }  // saved on destruction
    Settings loaded = Settings::load(path);
    ShellHarness h(loaded, path);
    h.frame();
    auto st = h.run({"ui.state"})["result"];
    EXPECT_EQ(st["space"], 7);
    EXPECT_EQ(st["spaces"][6]["layout"], "columns-3");
    EXPECT_EQ(st["spaces"][6]["panes"][0], "scopes");
    EXPECT_EQ(h.shell->actions().bindings("transport.toggle")[0], (KeyChord{SDL_SCANCODE_K, 0}));
}

TEST(Shell, CleanProjectQuitsWithoutAsking) {
    ShellHarness h;
    h.frame();
    EXPECT_TRUE(h.run({"ui.action", "app.quit"})["ok"]);
    EXPECT_TRUE(h.shell->quitRequested());
}

TEST(Shell, UnsavedChangesDialogAsksBeforeQuitting) {
    ShellHarness h;
    h.frame();
    ASSERT_TRUE(h.run({"track.add", "video"})["ok"]);
    h.run({"ui.action", "app.quit"});
    h.frame();
    EXPECT_FALSE(h.shell->quitRequested());
    auto dialog = h.run({"ui.state"})["result"]["dialog"];
    ASSERT_TRUE(dialog.is_object());
    EXPECT_NE(dialog["text"].get<std::string>().find("before quitting"), std::string::npos);
    EXPECT_EQ(dialog["choices"], nlohmann::json({"save", "cancel", "discard"}));
    EXPECT_FALSE(h.run({"ui.dialog", "later"})["ok"]) << "not a choice of this dialog";

    // Clicks under the dialog do nothing: the pane's view picker stays shut.
    float row = h.shell->ui().rowHeight();
    std::string view = h.shell->workspace().active().panes[0].view;
    h.click(20, row / 2);
    h.click(20, row * 3);
    EXPECT_EQ(h.shell->workspace().active().panes[0].view, view);

    // Escape cancels and keeps the editor open.
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_ESCAPE, 0}));
    h.frame();
    EXPECT_FALSE(h.shell->quitRequested());
    EXPECT_TRUE(h.run({"ui.state"})["result"]["dialog"].is_null());
    EXPECT_FALSE(h.run({"ui.dialog", "save"})["ok"]) << "no dialog is open";

    // Discard quits without saving.
    h.run({"ui.action", "app.quit"});
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_D, ModGui}));
    EXPECT_TRUE(h.shell->quitRequested());
    EXPECT_TRUE(h.session.dirty());
}

TEST(Shell, UnsavedChangesDialogSavesThenContinues) {
    ShellHarness h;
    auto path = (fixtures::dir() / "dialog-save.mfproj").string();
    std::filesystem::remove(path);
    ASSERT_TRUE(h.run({"save", path})["ok"]);
    ASSERT_TRUE(h.run({"track.add", "video", "name=Extra"})["ok"]);
    ASSERT_TRUE(h.session.dirty());
    size_t tracks = h.session.project().tracks.size();

    h.run({"ui.action", "file.new"});
    h.frame();
    EXPECT_EQ(h.session.project().tracks.size(), tracks) << "the project stays until the user answers";
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_RETURN, 0}));
    EXPECT_TRUE(h.run({"ui.state"})["result"]["dialog"].is_null());
    EXPECT_FALSE(h.session.dirty());
    EXPECT_NE(h.session.project().tracks.size(), tracks) << "a new project replaced the saved one";

    // The save wrote the extra track to the file.
    ASSERT_TRUE(h.run({"open", path})["ok"]);
    EXPECT_EQ(h.session.project().tracks.size(), tracks);
    std::filesystem::remove(path);
}

namespace {

// Leaves the files of an editor that crashed with unsaved changes.
void leaveCrashCopy(const std::filesystem::path& dir, const std::string& stem, const Project& p, const std::string& path) {
    std::filesystem::create_directories(dir);
    saveProject(p, (dir / (stem + ".mfproj")).string());
    std::ofstream(dir / (stem + ".json")) << nlohmann::json{{"path", path}, {"name", p.name}, {"savedAt", 1700000000}}.dump();
}

bool hasTrack(const Project& p, const std::string& name) {
    return std::ranges::any_of(p.tracks, [&](const Track& t) { return t.name == name; });
}

}  // namespace

TEST(Shell, RecoveryRestoresCopiesOfEditorsThatDidNotQuit) {
    namespace fs = std::filesystem;
    fs::path dir = fixtures::dir() / "recovery-restore";
    fs::remove_all(dir);
    std::string path = (fixtures::dir() / "recover-me.mfproj").string();
    fs::remove(path);
    {
        ShellHarness a;
        a.shell->enableRecovery(dir.string());
        ASSERT_TRUE(a.run({"save", path})["ok"]);
        ASSERT_TRUE(a.run({"track.add", "video", "name=Unsaved"})["ok"]);
        a.frame();
        ASSERT_TRUE(waitForRecovery([&] {
            for (const auto& e : fs::directory_iterator(dir))
                if (e.path().extension() == ".json") return true;
            return false;
        }));
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir)) files.push_back(e.path());
        ASSERT_EQ(files.size(), 3u) << "a copy, its details, and a lock";

        // An editor that still runs keeps its copy to itself.
        ShellHarness b;
        b.shell->enableRecovery(dir.string());
        b.frame();
        EXPECT_FALSE(b.shell->dialogOpen());

        // The same files under a name no editor holds: a crash.
        for (const fs::path& f : files)
            if (f.extension() != ".lock") fs::copy_file(f, dir / ("crashed" + f.extension().string()));
    }
    EXPECT_FALSE(fs::exists(dir / "crashed.lock"));
    std::vector<std::string> left;
    for (const auto& e : fs::directory_iterator(dir)) left.push_back(e.path().filename().string());
    std::ranges::sort(left);
    EXPECT_EQ(left, (std::vector<std::string>{"crashed.json", "crashed.mfproj"})) << "editors that quit remove their own files";

    ShellHarness c;
    c.shell->enableRecovery(dir.string());
    c.frame();
    auto dialog = c.run({"ui.state"})["result"]["dialog"];
    ASSERT_TRUE(dialog.is_object());
    EXPECT_NE(dialog["text"].get<std::string>().find("recover-me.mfproj"), std::string::npos);
    EXPECT_EQ(dialog["choices"], nlohmann::json({"restore", "later", "discard"}));
    EXPECT_TRUE(c.shell->handleKey({SDL_SCANCODE_RETURN, 0}));
    EXPECT_FALSE(c.shell->dialogOpen());
    EXPECT_TRUE(hasTrack(c.session.project(), "Unsaved"));
    EXPECT_TRUE(c.session.dirty()) << "restored changes stay unsaved";
    EXPECT_EQ(c.session.path(), fs::absolute(path).string());
    EXPECT_FALSE(fs::exists(dir / "crashed.mfproj"));
    EXPECT_FALSE(fs::exists(dir / "crashed.json"));
    c.frame();
    EXPECT_FALSE(c.shell->dialogOpen()) << "nothing else to restore";
    fs::remove(path);
}

TEST(Shell, RecoveryCanWaitOrDiscard) {
    namespace fs = std::filesystem;
    fs::path dir = fixtures::dir() / "recovery-later";
    fs::remove_all(dir);
    Project p = Project::makeDefault();
    p.name = "Lost work";
    leaveCrashCopy(dir, "old", p, "");
    {
        ShellHarness h;
        h.shell->enableRecovery(dir.string());
        h.frame();
        ASSERT_TRUE(h.shell->dialogOpen());
        EXPECT_TRUE(h.run({"ui.dialog", "later"})["ok"]);
        h.frame();
        EXPECT_FALSE(h.shell->dialogOpen()) << "one offer per launch";
        EXPECT_FALSE(h.session.dirty());
    }
    EXPECT_TRUE(fs::exists(dir / "old.mfproj")) << "Later keeps the copy for the next launch";

    ShellHarness h;
    h.shell->enableRecovery(dir.string());
    h.frame();
    ASSERT_TRUE(h.shell->dialogOpen());
    EXPECT_NE(h.run({"ui.state"})["result"]["dialog"]["text"].get<std::string>().find("Lost work"), std::string::npos);
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_D, ModGui}));
    EXPECT_FALSE(fs::exists(dir / "old.mfproj"));
    EXPECT_FALSE(fs::exists(dir / "old.json"));
    EXPECT_FALSE(h.session.dirty());
}

TEST(Shell, RecoveryKeepsCopiesOfUnsavedWorkOnly) {
    namespace fs = std::filesystem;
    fs::path dir = fixtures::dir() / "recovery-copies";
    fs::remove_all(dir);
    std::string path = (fixtures::dir() / "recovery-copies.mfproj").string();
    ShellHarness h;
    h.shell->enableRecovery(dir.string());
    auto copies = [&] {
        int n = 0;
        for (const auto& e : fs::directory_iterator(dir)) n += e.path().extension() == ".mfproj";
        return n;
    };
    h.frame();
    EXPECT_EQ(copies(), 0) << "nothing unsaved";
    ASSERT_TRUE(h.run({"track.add", "video", "name=First"})["ok"]);
    h.frame();
    ASSERT_TRUE(waitForRecovery([&] { return copies() == 1; }));

    // Later edits wait for the interval.
    ASSERT_TRUE(h.run({"track.add", "video", "name=Second"})["ok"]);
    h.frame();
    auto copy = [&] {
        for (const auto& e : fs::directory_iterator(dir))
            if (e.path().extension() == ".mfproj") return loadProject(e.path().string());
        return Project{};
    };
    EXPECT_FALSE(hasTrack(copy(), "Second"));
    h.now += Recovery::kInterval;
    h.frame();
    ASSERT_TRUE(waitForRecovery([&] { return hasTrack(copy(), "Second"); }));

    // Saving removes the copy.
    ASSERT_TRUE(h.run({"save", path})["ok"]);
    h.frame();
    EXPECT_EQ(copies(), 0);
    fs::remove(path);
}

TEST(Shell, CommandPaletteFindsAndRunsActions) {
    ShellHarness h;
    h.frame();
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_K, ModGui}));
    EXPECT_TRUE(h.shell->paletteOpen());

    // Keys type into the search instead of running their actions.
    EXPECT_FALSE(h.shell->handleKey({SDL_SCANCODE_SPACE, 0}));
    h.frame();
    h.frame();
    EXPECT_TRUE(h.run({"ui.state"})["result"]["typing"]) << "the search field has focus";
    EXPECT_FALSE(h.session.player().playing());
    bool loop = h.session.player().loop();
    h.in.text = "loop";
    h.frame();
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_RETURN, 0}));
    EXPECT_FALSE(h.shell->paletteOpen());
    EXPECT_NE(h.session.player().loop(), loop) << "Return ran Toggle loop";
    h.frame();
    h.frame();
    EXPECT_FALSE(h.run({"ui.state"})["result"]["typing"]);

    // Escape closes it without running anything, and so does Cmd+K.
    h.shell->openPalette();
    h.frame();
    h.in.text = "loop";
    h.frame();
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_ESCAPE, 0}));
    EXPECT_FALSE(h.shell->paletteOpen());
    h.shell->openPalette();
    h.frame();
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_K, ModGui}));
    EXPECT_FALSE(h.shell->paletteOpen());
    h.frame();
    EXPECT_NE(h.session.player().loop(), loop);

    // Return with no match runs nothing and leaves the search open.
    h.shell->openPalette();
    h.frame();
    h.in.text = "no such action";
    h.frame();
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_RETURN, 0}));
    EXPECT_TRUE(h.shell->paletteOpen());
    EXPECT_NE(h.session.player().loop(), loop);
}

TEST(Shell, CommandPaletteRunsAClickedRow) {
    ShellHarness h;
    h.frame();
    h.shell->openPalette();
    h.frame();
    h.in.text = "settings";
    h.frame();
    // The first row sits under the search field, near the top center.
    float row = h.shell->ui().rowHeight();
    float top = std::round(h.h * 0.12f) + h.shell->ui().dp(4) + row + 1;
    h.click(h.w / 2.0f, top + row / 2);
    EXPECT_FALSE(h.shell->paletteOpen());
    EXPECT_EQ(h.shell->workspace().active().panes[size_t(h.shell->focusedPane())].view, "settings");

    // A press outside closes it.
    h.shell->openPalette();
    h.frame();
    h.click(10, h.h - row * 3);
    EXPECT_FALSE(h.shell->paletteOpen());
}

TEST(Shell, RecentProjectsFollowOpenAndSave) {
    namespace fs = std::filesystem;
    std::string a = fs::absolute(fixtures::dir() / "recent-a.mfproj").string();
    std::string b = fs::absolute(fixtures::dir() / "recent-b.mfproj").string();
    ShellHarness h;
    ASSERT_TRUE(h.run({"save", a})["ok"]);
    h.frame();
    ASSERT_TRUE(h.run({"save", b})["ok"]);
    h.frame();
    EXPECT_EQ(h.shell->recentProjects(), (std::vector<std::string>{b, a}));
    ASSERT_TRUE(h.run({"open", a})["ok"]);
    h.frame();
    EXPECT_EQ(h.shell->recentProjects(), (std::vector<std::string>{a, b}));
    EXPECT_EQ(Settings::fromJson(h.shell->settings().toJson()).recent, h.shell->recentProjects());

    // A project that is gone leaves the list when it fails to open.
    fs::remove(b);
    h.shell->openProjectFile(b);
    h.frame();
    EXPECT_EQ(h.shell->recentProjects(), (std::vector<std::string>{a}));
    EXPECT_TRUE(h.run({"ui.state"})["result"]["notice"]["error"]);

    h.shell->clearRecentProjects();
    EXPECT_TRUE(h.shell->recentProjects().empty());
    fs::remove(a);
}

TEST(Shell, FinishedExportOffersToShowTheFile) {
    std::string out = (fixtures::dir() / "notice.wav").string();
    std::filesystem::remove(out);
    ShellHarness h;
    std::string revealed;
    h.shell->hooks.reveal = [&](const std::string& path) { revealed = path; };
    h.shell->hooks.revealLabel = "Reveal";
    h.frame();
    ASSERT_TRUE(h.run({"export", out, "end=0.1"})["ok"]);
    h.session.exportJob().wait();
    h.frame();
    auto notice = h.run({"ui.state"})["result"]["notice"];
    ASSERT_TRUE(notice.is_object());
    EXPECT_EQ(notice["text"], "Exported notice.wav");
    EXPECT_EQ(notice["action"], "Reveal");

    EXPECT_TRUE(revealed.empty());

    // Each export reports once, even one that ends before the next frame.
    ASSERT_TRUE(h.run({"export", (fixtures::dir() / "notice.mov").string()})["ok"]);
    h.session.exportJob().wait();
    h.frame();
    notice = h.run({"ui.state"})["result"]["notice"];
    EXPECT_TRUE(notice["error"]);
    EXPECT_NE(notice["text"].get<std::string>().find("Export failed"), std::string::npos);
    EXPECT_FALSE(notice.contains("action"));
    std::filesystem::remove(out);
}
