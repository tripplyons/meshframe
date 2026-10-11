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
