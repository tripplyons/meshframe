#include "shell_harness.hpp"
#include "ui/views/views.hpp"

#include <gtest/gtest.h>

using namespace mf;
using namespace mf::ui;

namespace {

json ok(ShellHarness& h, std::vector<std::string> words) {
    json r = h.run(words);
    EXPECT_TRUE(r["ok"].get<bool>()) << r.dump();
    return r["result"];
}

// The body of the only pane in the "single" layout.
Rect singleBody(ShellHarness& h) {
    Rect a = h.paneArea();
    float row = h.shell->ui().rowHeight();
    return {a.x, a.y + row + 1, a.w, a.h - row - 1};
}

void key(ShellHarness& h, SDL_Scancode k) {
    h.in.keys.push_back({k, 0});
    h.frame();
}

}  // namespace

TEST(Views, EveryViewDrawsAPopulatedProject) {
    ShellHarness h;
    ok(h, {"script.set", "source=mf.signal(\"beat\", function() return floor(t * 2) end)\n"
                         "mf.generator(\"bars\", function(g) g:clear(\"#000000\") end)\n"
                         "mf.clip{ track = \"V2\", text = \"Title\", start = 0, duration = 2 }"});
    Id bg = ok(h, {"clip.add", "solid", "V1", "0", "4", "bg"})["id"];
    ok(h, {"prop.key", "bg", "opacity", "0", "0"});
    ok(h, {"prop.key", "bg", "opacity", "2", "1", "ease-in"});
    ok(h, {"prop.expr", "bg", "scale", "1 + sig.beat"});
    ok(h, {"prop.expr", "bg", "x", "nil + 1"});
    json fx = ok(h, {"effect.add", "bg", "blur"});
    ok(h, {"effect.add", "master", "compressor"});
    json right = ok(h, {"clip.split", "bg", "3"});
    json tr = ok(h, {"transition.add", std::to_string(bg), right[0].dump(), "duration=0.5"});
    if (fixtures::haveFfmpeg()) {
        json m = ok(h, {"media.import", fixtures::videoWithAudio()});
        ok(h, {"clip.media", m["name"], "start=1"});
    }
    ok(h, {"ui.layout", "single"});

    // Each selection kind reaches the inspector, effects, and curves views.
    std::string bgId = std::to_string(bg);
    std::vector<std::vector<std::string>> selections = {
        {"select"},
        {"select", "clips=" + bgId, "prop=opacity"},
        {"select", "clips=" + bgId, "effect=" + fx["id"].dump()},
        {"select", "transition=" + tr["id"].dump()},
        {"select", "track=V1"},
    };
    Rect body = singleBody(h);
    for (const ViewType& type : viewTypes()) {
        SCOPED_TRACE(type.id);
        ok(h, {"ui.pane", "1", type.id});
        for (const auto& sel : selections) {
            ok(h, sel);
            h.in.mouse = {body.x + body.w / 2, body.y + body.h / 2};
            h.frame();
            h.in.wheelY = -1;
            h.frame();
            h.frame();
        }
        EXPECT_EQ(h.shell->workspace().active().panes[0].view, type.id);
    }
}

TEST(Views, HistoryClickUndoesOrRedoesToThatEntry) {
    ShellHarness h;
    for (const char* name : {"a", "b", "c"}) ok(h, {"clip.add", "solid", "V1", "0", "1", name});
    ok(h, {"ui.layout", "single"});
    ok(h, {"ui.pane", "1", "history"});
    h.frame();
    Rect body = singleBody(h);
    float row = h.shell->ui().rowHeight();
    auto clickRow = [&](int i) { h.click(body.x + 40, body.y + row * (float(i) + 0.5f)); };

    clickRow(1);  // after the first edit
    EXPECT_EQ(h.session.history().undoLabels().size(), 1u);
    EXPECT_EQ(h.session.history().redoLabels().size(), 2u);
    EXPECT_EQ(h.session.project().clips.size(), 1u);
    clickRow(0);  // the initial state
    EXPECT_TRUE(h.session.project().clips.empty());
    clickRow(3);  // redo everything
    EXPECT_EQ(h.session.project().clips.size(), 3u);
    EXPECT_TRUE(h.session.history().redoLabels().empty());
}

TEST(Views, EffectsAddMenuAddsTheClickedEffect) {
    ShellHarness h;
    Id bg = ok(h, {"clip.add", "solid", "V1", "0", "4", "bg"})["id"];
    ok(h, {"select", "clips=" + std::to_string(bg)});
    ok(h, {"ui.layout", "single"});
    ok(h, {"ui.pane", "1", "video-effects"});
    h.frame();
    Rect a = h.paneArea();
    Context& ui = h.shell->ui();
    float row = ui.rowHeight();
    // The menu opens below the header's Add button and extends past the header.
    h.click(a.r() - ui.dp(30), a.y + row / 2);
    h.click(a.r() - ui.dp(100), a.y + row + ui.dp(4) + row / 2);
    const Clip* c = h.session.project().findClip(bg);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->videoEffects.size(), 1u);
    EXPECT_EQ(c->videoEffects[0].type, videoEffectSpecs()[0].type);
}

TEST(Views, ConsoleEvaluatesLuaAndRunsCommands) {
    ShellHarness h;
    ok(h, {"ui.layout", "single"});
    ok(h, {"ui.pane", "1", "console"});
    h.frame();
    Rect body = singleBody(h);
    float row = h.shell->ui().rowHeight();
    h.click(body.x + body.w / 2, body.b() - row / 2);

    h.in.text = "1 + 2";
    h.frame();
    key(h, SDL_SCANCODE_RETURN);
    ASSERT_FALSE(h.session.logLines().empty());
    EXPECT_EQ(h.session.logLines().back().text, "3");

    // The prompt keeps focus, so the next line needs no click.
    h.in.text = ":clip.add solid V1 0 2 \"my clip\"";
    h.frame();
    key(h, SDL_SCANCODE_RETURN);
    ASSERT_EQ(h.session.project().clips.size(), 1u);
    EXPECT_EQ(h.session.project().clips[0].name, "my clip");

    h.in.text = ":clip.add bogus";
    h.frame();
    key(h, SDL_SCANCODE_RETURN);
    EXPECT_EQ(h.session.logLines().back().level, LogLine::Level::Error);
}

namespace {

// Opens the settings view in a single pane and filters it by typing in the header.
void openSettings(ShellHarness& h, const std::string& filter) {
    ok(h, {"ui.layout", "single"});
    ok(h, {"ui.pane", "1", "settings"});
    h.frame();
    float row = h.shell->ui().rowHeight();
    h.click(float(h.w) - 60, row / 2);
    h.in.text = filter;
    h.frame();
    key(h, SDL_SCANCODE_RETURN);
}

}  // namespace

TEST(Views, SettingsRebindsByCaptureAndPersists) {
    auto path = (fixtures::dir() / "ui" / "settings-view.json").string();
    std::filesystem::create_directories(fixtures::dir() / "ui");
    std::filesystem::remove(path);
    const KeyChord cmdK{SDL_SCANCODE_K, ModGui}, redo{SDL_SCANCODE_Z, ModGui | ModShift};
    {
        ShellHarness h({}, path);
        openSettings(h, "edit.undo");
        // Rows: three section headers with their borders, the hint, the group, then Undo.
        Rect body = singleBody(h);
        float row = h.shell->ui().rowHeight(), rowH = row + 2;
        float y = body.y + 3 * (row + 1) + rowH + row + rowH / 2;
        float chordX = body.x + std::min(170.0f, body.w * 0.4f) + 6;
        h.click(chordX, y);
        ASSERT_TRUE(h.shell->capture());
        EXPECT_EQ(h.shell->capture()->action, "edit.undo");

        // Escape cancels; the binding stays.
        EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_ESCAPE, 0}));
        EXPECT_FALSE(h.shell->capture());
        EXPECT_EQ(h.shell->actions().bindings("edit.undo")[0], (KeyChord{SDL_SCANCODE_Z, ModGui}));

        // A captured key replaces the chord and does not run an action.
        h.click(chordX, y);
        ASSERT_TRUE(h.shell->capture());
        EXPECT_TRUE(h.shell->handleKey(cmdK));
        EXPECT_EQ(h.shell->actions().bindings("edit.undo"), std::vector<KeyChord>{cmdK});
        h.frame();

        // Binding a chord that another action uses moves it.
        h.click(chordX, y);
        EXPECT_TRUE(h.shell->handleKey(redo));
        EXPECT_EQ(h.shell->actions().bindings("edit.undo"), std::vector<KeyChord>{redo});
        EXPECT_TRUE(h.shell->actions().bindings("edit.redo").empty());

        // A mouse press elsewhere cancels a capture.
        h.click(chordX, y);
        ASSERT_TRUE(h.shell->capture());
        h.click(body.x + body.w - 10, body.b() - 10);
        EXPECT_FALSE(h.shell->capture());
    }  // saved on destruction
    ShellHarness h(Settings::load(path), path);
    EXPECT_EQ(h.shell->actions().bindings("edit.undo"), std::vector<KeyChord>{redo});
    EXPECT_TRUE(h.shell->actions().bindings("edit.redo").empty());
}

TEST(Views, SettingsColorEditsApplyLiveAndPersist) {
    auto path = (fixtures::dir() / "ui" / "settings-colors.json").string();
    std::filesystem::create_directories(fixtures::dir() / "ui");
    std::filesystem::remove(path);
    {
        ShellHarness h({}, path);
        openSettings(h, "panel");
        Rect body = singleBody(h);
        float row = h.shell->ui().rowHeight(), rowH = row + 2;
        // The panel color row follows the Interface and Colors section headers.
        float y = body.y + 2 * (row + 1) + rowH / 2;
        float hexX = body.x + std::min(170.0f, body.w * 0.4f) + rowH + 20;
        h.click(hexX, y);
        h.in.keys.push_back({SDL_SCANCODE_A, ModGui});
        h.frame();
        h.in.text = "#204060";
        h.frame();
        key(h, SDL_SCANCODE_RETURN);
        Color want = *parseColor("#204060");
        EXPECT_EQ(h.shell->settings().theme[Slot::Panel], want);
        // The pane header uses the panel color on the next frame.
        h.frame();
        SDL_Color px = h.pixel(int(body.x) + 200, int(row / 2));
        EXPECT_EQ(px.r, 0x20);
        EXPECT_EQ(px.g, 0x40);
        EXPECT_EQ(px.b, 0x60);
    }
    Settings loaded = Settings::load(path);
    EXPECT_EQ(loaded.theme[Slot::Panel], *parseColor("#204060"));
}

TEST(Views, MarkerKeysAddAndJump) {
    ShellHarness h;
    ok(h, {"ui.layout", "single"});
    ok(h, {"ui.pane", "1", "timeline"});
    ok(h, {"clip.add", "solid", "V1", "0", "10"});
    ok(h, {"seek", "2"});
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_M, 0}));
    ok(h, {"seek", "5"});
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_M, 0}));
    json markers = ok(h, {"show", "markers"});
    ASSERT_EQ(markers.size(), 2u);
    EXPECT_EQ(markers[0]["name"], "M1");
    EXPECT_EQ(markers[1]["time"], 5.0);

    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_LEFT, ModAlt}));
    EXPECT_DOUBLE_EQ(h.session.time(), 2.0);
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_RIGHT, ModAlt}));
    EXPECT_DOUBLE_EQ(h.session.time(), 5.0);
    EXPECT_TRUE(h.shell->handleKey({SDL_SCANCODE_RIGHT, ModAlt}));
    EXPECT_DOUBLE_EQ(h.session.time(), 5.0);
    EXPECT_EQ(ok(h, {"ui.state"})["notice"]["text"], "No later marker");
    h.frame();  // draws the markers
}

TEST(Views, QuitCommandAsksAboutUnsavedChanges) {
    ShellHarness h;
    ok(h, {"clip.add", "solid", "V1", "0", "1"});
    json state = ok(h, {"quit"});
    EXPECT_FALSE(h.shell->quitRequested());
    EXPECT_TRUE(state["dialog"]["choices"].dump().find("discard") != std::string::npos) << state.dump();
    ok(h, {"ui.dialog", "discard"});
    EXPECT_TRUE(h.shell->quitRequested());
}
