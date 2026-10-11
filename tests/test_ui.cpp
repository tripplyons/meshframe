#include "fixtures.hpp"
#include "ui/actions.hpp"
#include "ui/code_editor.hpp"
#include "ui/settings.hpp"
#include "ui/widgets.hpp"
#include "ui_harness.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace mf::ui;

TEST(UiSettings, JsonAndFileRoundTrip) {
    Settings s;
    s.theme[Slot::Accent] = *mf::parseColor("#c04040");
    s.theme[Slot::Playhead] = *mf::parseColor("#f0e0d0");
    s.bindings = {{"space.1", {"Ctrl+1"}}};
    s.uiSize = 15;
    s.monoFont = "/tmp/missing.ttf";
    s.workspace = {{"current", 3}};

    auto j = s.toJson();
    EXPECT_EQ(j["theme"].size(), 2u) << "only changed colors are written";
    EXPECT_EQ(j["theme"]["accent"], "#c04040");

    auto path = fixtures::dir() / "ui" / "settings.json";
    std::filesystem::create_directories(path.parent_path());
    std::string err;
    ASSERT_TRUE(s.save(path.string(), &err)) << err;
    Settings back = Settings::load(path.string(), &err);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(back.toJson(), j);
    EXPECT_EQ(back.theme[Slot::Accent], s.theme[Slot::Accent]);
    EXPECT_EQ(back.theme[Slot::Text], Theme{}[Slot::Text]);
    EXPECT_EQ(back.uiSize, 15);
    EXPECT_EQ(back.monoFont, "/tmp/missing.ttf");

    // A damaged file keeps defaults for whatever it cannot read.
    {
        std::ofstream f(path);
        f << R"({"theme": {"accent": "nope", "text": "#101010", "unknown": "#ffffff"}, "fonts": {"uiSize": 900}})";
    }
    Settings damaged = Settings::load(path.string(), &err);
    EXPECT_EQ(damaged.theme[Slot::Accent], Theme{}[Slot::Accent]);
    EXPECT_EQ(damaged.theme[Slot::Text], *mf::parseColor("#101010"));
    EXPECT_LE(damaged.uiSize, 40);

    Settings missing = Settings::load((fixtures::dir() / "ui" / "none.json").string(), &err);
    EXPECT_EQ(missing.toJson(), Settings{}.toJson());
}

TEST(UiKeys, ChordsParseAndFormat) {
    auto c = parseChord("option+shift+3");
    ASSERT_TRUE(c);
    EXPECT_EQ(c->key, SDL_SCANCODE_3);
    EXPECT_EQ(c->mods, ModAlt | ModShift);
    EXPECT_EQ(formatChord(*c), "Alt+Shift+3");
    EXPECT_EQ(formatChord(*parseChord("cmd+z")), "Cmd+Z");
    EXPECT_EQ(formatChord(*parseChord("Space")), "Space");
    EXPECT_EQ(parseChord("Ctrl+Shift+Left")->mods, ModCtrl | ModShift);
    EXPECT_FALSE(parseChord("Cmd+"));
    EXPECT_FALSE(parseChord("Hyper+K"));
    EXPECT_FALSE(parseChord("NotAKey"));
}

TEST(UiActions, BindingsOverridesAndDispatch) {
    Actions a;
    int undo = 0, play = 0, space = 0;
    a.add({"edit.undo", "Undo", "Edit", {{SDL_SCANCODE_Z, ModGui}}, [&] { ++undo; }});
    a.add({"transport.toggle", "Play or pause", "Transport", {{SDL_SCANCODE_SPACE, 0}}, [&] { ++play; }});
    a.add({"space.2", "Switch to space 2", "Spaces", {{SDL_SCANCODE_2, ModAlt}}, [&] { ++space; }, true});

    // Typing in a field keeps Cmd+Z and Space for the field; Alt+2 still switches.
    EXPECT_FALSE(a.dispatch({SDL_SCANCODE_Z, ModGui}, true));
    EXPECT_FALSE(a.dispatch({SDL_SCANCODE_SPACE, 0}, true));
    EXPECT_TRUE(a.dispatch({SDL_SCANCODE_2, ModAlt}, true));
    EXPECT_TRUE(a.dispatch({SDL_SCANCODE_Z, ModGui}, false));
    EXPECT_TRUE(a.dispatch({SDL_SCANCODE_SPACE, 0}, false));
    EXPECT_EQ(undo, 1);
    EXPECT_EQ(play, 1);
    EXPECT_EQ(space, 1);

    a.setBindings("transport.toggle", {{SDL_SCANCODE_K, 0}, {SDL_SCANCODE_SPACE, ModShift}});
    EXPECT_FALSE(a.dispatch({SDL_SCANCODE_SPACE, 0}, false));
    EXPECT_TRUE(a.dispatch({SDL_SCANCODE_K, 0}, false));
    EXPECT_EQ(a.bound({SDL_SCANCODE_K, 0}), std::vector<std::string>{"transport.toggle"});

    auto saved = a.overridesJson();
    EXPECT_EQ(saved, (nlohmann::json{{"transport.toggle", {"K", "Shift+Space"}}}));

    // Setting the defaults again removes the override.
    a.setBindings("transport.toggle", {{SDL_SCANCODE_SPACE, 0}});
    EXPECT_TRUE(a.overridesJson().empty());

    Actions b;
    b.add({"transport.toggle", "Play or pause", "Transport", {{SDL_SCANCODE_SPACE, 0}}, [&] { ++play; }});
    b.loadOverrides({{"transport.toggle", {"K", "Bogus+Key"}}, {"no.such.action", {"X"}}});
    EXPECT_EQ(b.bindings("transport.toggle").size(), 1u);
    EXPECT_TRUE(b.run("transport.toggle"));
    EXPECT_FALSE(b.run("no.such.action"));
    b.resetAll();
    EXPECT_EQ(b.bindings("transport.toggle")[0], (KeyChord{SDL_SCANCODE_SPACE, 0}));
}

TEST(UiWidgets, ButtonClickNeedsPressAndReleaseInside) {
    UiHarness h;
    Rect r{10, 10, 80, 24};
    int clicks = 0;
    auto draw = [&](Context& ui) {
        if (button(ui, r, "Go")) ++clicks;
    };
    h.move(20, 20);
    h.frame(draw);  // hover is known from the next frame on
    h.press();
    h.frame(draw);
    h.release();
    h.frame(draw);
    EXPECT_EQ(clicks, 1);

    // Pressing inside and releasing outside is not a click.
    h.press();
    h.frame(draw);
    h.move(200, 200);
    h.frame(draw);
    h.release();
    h.frame(draw);
    EXPECT_EQ(clicks, 1);

    // The later of two overlapping widgets takes the click.
    int under = 0, over = 0;
    auto stacked = [&](Context& ui) {
        if (button(ui, {0, 0, 100, 100}, "under")) ++under;
        if (button(ui, {40, 40, 20, 20}, "over")) ++over;
    };
    h.move(50, 50);
    h.frame(stacked);
    h.frame(stacked);
    h.press();
    h.frame(stacked);
    h.release();
    h.frame(stacked);
    EXPECT_EQ(under, 0);
    EXPECT_EQ(over, 1);
}

TEST(UiWidgets, TextFieldEditsCommitsAndCancels) {
    UiHarness h;
    std::string value = "clip";
    FieldResult last;
    auto draw = [&](Context& ui) { last = textField(ui, {0, 0, 200, 24}, "name", value); };
    h.move(100, 10);
    h.frame(draw);
    h.press();
    h.frame(draw);
    h.release();
    h.frame(draw);
    EXPECT_TRUE(last.editing);
    EXPECT_TRUE(h.ui->typing());

    h.in.text = "s 1";
    h.frame(draw);
    h.key(SDL_SCANCODE_BACKSPACE);
    h.frame(draw);
    EXPECT_EQ(value, "clip") << "the value changes only on commit";
    h.key(SDL_SCANCODE_RETURN);
    h.frame(draw);
    EXPECT_TRUE(last.committed);
    EXPECT_EQ(value, "clips ");

    // Escape restores the value.
    h.press();
    h.frame(draw);
    h.release();
    h.in.text = "xyz";
    h.frame(draw);
    h.key(SDL_SCANCODE_ESCAPE);
    h.frame(draw);
    EXPECT_TRUE(last.canceled);
    EXPECT_EQ(value, "clips ");

    // Clicking elsewhere commits an edit on the next frame.
    std::string other = "a";
    auto two = [&](Context& ui) {
        last = textField(ui, {0, 0, 200, 24}, "name", value);
        textField(ui, {0, 40, 200, 24}, "other", other);
    };
    h.frame(two);
    h.press();
    h.frame(two);
    h.release();
    h.key(SDL_SCANCODE_A, ModGui);
    h.in.text = "new";
    h.frame(two);
    h.move(100, 50);
    h.frame(two);
    h.press();
    h.frame(two);
    h.release();
    h.frame(two);
    EXPECT_TRUE(last.committed);
    EXPECT_EQ(value, "new");
}

TEST(UiWidgets, NumberFieldDragsAndTypes) {
    UiHarness h;
    double v = 1.0;
    DragResult last;
    NumberStyle style{.step = 0.5, .min = 0, .max = 10, .decimals = 1};
    bool began = false, ended = false;
    auto draw = [&](Context& ui) {
        last = numberField(ui, {0, 0, 100, 24}, "gain", v, style);
        began |= last.began;
        ended |= last.ended;
    };
    h.move(50, 10);
    h.frame(draw);
    h.press();
    h.frame(draw);
    h.move(60, 10);
    h.frame(draw);
    EXPECT_DOUBLE_EQ(v, 6.0);
    h.move(200, 10);
    h.frame(draw);
    EXPECT_DOUBLE_EQ(v, 10.0) << "clamped to max";
    h.release();
    h.frame(draw);
    EXPECT_TRUE(began);
    EXPECT_TRUE(ended);

    // A click without movement starts typing with the value selected.
    h.move(50, 10);
    h.frame(draw);
    h.press();
    h.frame(draw);
    h.release();
    h.frame(draw);
    h.in.text = "2.5";
    h.frame(draw);
    h.key(SDL_SCANCODE_RETURN);
    h.frame(draw);
    EXPECT_TRUE(last.committed);
    EXPECT_DOUBLE_EQ(v, 2.5);
}

TEST(UiWidgets, DropdownMenuBlocksClicksUnderneath) {
    UiHarness h;
    std::vector<std::string> options{"preview", "timeline", "media"};
    int index = 0, behind = 0;
    auto draw = [&](Context& ui) {
        if (button(ui, {0, 30, 200, 100}, "behind")) ++behind;
        dropdown(ui, {0, 0, 120, 24}, "view", index, options);
    };
    h.move(10, 10);
    h.frame(draw);
    h.press();
    h.frame(draw);
    h.release();
    h.frame(draw);
    // The menu opens below the dropdown, over the button. Pick the third item.
    float row = h.ui->rowHeight();
    h.move(20, 24 + row * 2.5f);
    h.frame(draw);
    h.frame(draw);
    h.press();
    h.frame(draw);
    h.release();
    h.frame(draw);
    EXPECT_EQ(index, 2);
    EXPECT_EQ(behind, 0);

    // A press outside an open menu closes it without clicking what is under it.
    h.move(10, 10);
    h.frame(draw);
    h.press();
    h.frame(draw);
    h.release();
    h.frame(draw);
    h.move(150, 110);
    h.frame(draw);
    h.press();
    h.frame(draw);
    h.release();
    h.frame(draw);
    EXPECT_EQ(behind, 0);
    EXPECT_EQ(index, 2);
    h.press();
    h.frame(draw);
    h.release();
    h.frame(draw);
    EXPECT_EQ(behind, 1);
}

TEST(UiWidgets, CodeEditorTypesUndoesAndIndents) {
    UiHarness h;
    CodeEditor ed;
    ed.setText("function f()\nend");
    bool changed = false;
    auto draw = [&](Context& ui) { changed = ed.draw(ui, {0, 0, 400, 300}, "code"); };
    h.move(200, 150);
    h.frame(draw);
    h.press();
    h.frame(draw);
    h.release();
    h.frame(draw);
    EXPECT_TRUE(ed.focused(*h.ui));

    // Click lands past the last line, so the caret is at the end of "end".
    h.key(SDL_SCANCODE_UP);
    h.frame(draw);
    h.key(SDL_SCANCODE_END);
    h.frame(draw);
    h.key(SDL_SCANCODE_RETURN);
    h.frame(draw);
    EXPECT_TRUE(changed);
    h.in.text = "return 1";
    h.frame(draw);
    EXPECT_EQ(ed.text(), "function f()\n  return 1\nend");

    h.key(SDL_SCANCODE_Z, ModGui);
    h.frame(draw);
    EXPECT_EQ(ed.text(), "function f()\n  \nend");
    h.key(SDL_SCANCODE_Z, ModGui | ModShift);
    h.frame(draw);
    EXPECT_EQ(ed.text(), "function f()\n  return 1\nend");
}

TEST(UiPainter, BordersAreSinglePixelsAndTextRenders) {
    UiHarness h(64, 32);
    Theme& t = h.theme;
    h.frame([&](Context& ui) {
        ui.painter.fill({0, 0, 64, 32}, t[Slot::Background]);
        ui.painter.vline(20, 0, 32, t[Slot::Border]);
        ui.painter.outline({30, 4, 20, 20}, t[Slot::Border]);
        ui.painter.text(2, 2, "Hi", t[Slot::Text]);
    });
    auto same = [](SDL_Color a, mf::Color b) {
        auto e = [](uint8_t x, float y) { return std::abs(int(x) - int(std::round(y * 255))) <= 1; };
        return e(a.r, b.r) && e(a.g, b.g) && e(a.b, b.b);
    };
    EXPECT_TRUE(same(h.pixel(20, 16), t[Slot::Border]));
    EXPECT_TRUE(same(h.pixel(19, 16), t[Slot::Background]));
    EXPECT_TRUE(same(h.pixel(21, 16), t[Slot::Background]));
    EXPECT_TRUE(same(h.pixel(30, 10), t[Slot::Border]));
    EXPECT_TRUE(same(h.pixel(31, 10), t[Slot::Background]));
    EXPECT_TRUE(same(h.pixel(49, 10), t[Slot::Border]));
    EXPECT_TRUE(same(h.pixel(50, 10), t[Slot::Background]));
    int lit = 0;
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 18; ++x) lit += h.pixel(x, y).r > 128;
    EXPECT_GT(lit, 5) << "text drew glyphs";
}
