#include "fixtures.hpp"
#include "model/edit.hpp"
#include "render/image.hpp"
#include "render/sheet.hpp"
#include "session/commands.hpp"

#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace mf;

namespace {

json ok(Commands& c, const std::vector<std::string>& words) {
    json r = c.runSync(Commands::requestFromWords(words, fixtures::dir().string()));
    EXPECT_TRUE(r["ok"].get<bool>()) << r.dump();
    return r["result"];
}

std::string fails(Commands& c, const std::vector<std::string>& words) {
    json r = c.runSync(Commands::requestFromWords(words, fixtures::dir().string()));
    EXPECT_FALSE(r["ok"].get<bool>()) << r.dump();
    return r.value("error", "");
}

}  // namespace

TEST(Session, ApplyUndoAndGestures) {
    Session s;
    size_t tracks = s.project().tracks.size();
    s.apply("add track", [](Project& p) { edit::addTrack(p, TrackKind::Video, "V4"); });
    EXPECT_EQ(s.project().tracks.size(), tracks + 1);
    EXPECT_TRUE(s.dirty());

    // A failed edit leaves the project and history unchanged.
    EXPECT_THROW(s.apply("bad", [](Project& p) { edit::removeTrack(p, 999); }), EditError);
    EXPECT_EQ(s.history().undoLabels().size(), 1u);

    Id clip = 0;
    s.apply("add clip", [&](Project& p) {
        Clip c;
        c.kind = ClipKind::Solid;
        c.track = p.findTrackByName("V1")->id;
        c.duration = 2;
        clip = edit::addClip(p, c);
    });
    // A drag is many updates but one undo step.
    s.beginGesture("drag");
    for (int i = 1; i <= 5; ++i) s.updateGesture([&](Project& p) { edit::moveClip(p, clip, i * 0.5); });
    s.endGesture();
    EXPECT_DOUBLE_EQ(s.project().findClip(clip)->start, 2.5);
    EXPECT_EQ(s.history().undoLabels().back(), "drag");
    ASSERT_TRUE(s.undo());
    EXPECT_DOUBLE_EQ(s.project().findClip(clip)->start, 0);
    ASSERT_TRUE(s.redo());
    EXPECT_DOUBLE_EQ(s.project().findClip(clip)->start, 2.5);

    // The evaluator follows edits.
    EXPECT_NE(s.evaluator().project().findClip(clip), nullptr);
    s.undo();
    s.undo();
    EXPECT_EQ(s.evaluator().project().findClip(clip), nullptr);
}

TEST(Commands, ArgumentsAndErrors) {
    Session s;
    Commands c(s);
    json help = ok(c, {"help"});
    EXPECT_GT(help.size(), 40u);
    EXPECT_EQ(ok(c, {"help", "clip.add"})["usage"], "clip.add <kind> <track> <start> <duration> [name] [props]");

    EXPECT_NE(fails(c, {"nope"}).find("unknown command"), std::string::npos);
    EXPECT_NE(fails(c, {"clip.add", "text"}).find("missing argument 'track'"), std::string::npos);
    EXPECT_NE(fails(c, {"seek", "abc"}).find("expected time"), std::string::npos);
    EXPECT_NE(fails(c, {"seek", "1", "2"}).find("too many"), std::string::npos);
    EXPECT_NE(fails(c, {"clip.add", "text", "V9", "0", "1"}).find("no track named 'V9'"), std::string::npos);

    // Positional and key=value words mix; JSON requests take typed args.
    json clip = ok(c, {"clip.add", "text", "V1", "duration=4", "1", "name=title", R"(props={"text":"Hi","size":40})"});
    EXPECT_EQ(clip["start"], 1.0);
    EXPECT_EQ(clip["duration"], 4.0);
    EXPECT_EQ(clip["props"]["text"], "Hi");
    json r = c.runSync({{"cmd", "clip.move"}, {"args", {{"clip", "title"}, {"start", 2}}}});
    ASSERT_TRUE(r["ok"]) << r.dump();
    EXPECT_EQ(r["result"]["start"], 2.0);

    // A rejected edit adds no undo step.
    size_t steps = s.history().undoLabels().size();
    fails(c, {"clip.trim", "title", "2", "-1"});
    EXPECT_EQ(s.history().undoLabels().size(), steps);
}

TEST(Commands, EditPropertiesAndInspect) {
    Session s;
    Commands c(s);
    Id solid = ok(c, {"clip.add", "solid", "V1", "0", "4", "bg"})["id"];
    ok(c, {"prop.set", "bg", "color", "#ff0000"});
    EXPECT_EQ(ok(c, {"prop.get", "bg", "color"})["value"], "#ff0000");

    ok(c, {"prop.key", "bg", "opacity", "0", "0"});
    json prop = ok(c, {"prop.key", "bg", "opacity", "2", "1", "ease-in"});
    ASSERT_EQ(prop["curve"].size(), 2u);
    EXPECT_NEAR(ok(c, {"prop.get", "bg", "opacity", "time=1"})["value"].get<double>(), 0.5, 1e-9);
    // A key's ease shapes the segment that starts at it.
    ok(c, {"prop.key", "bg", "opacity", "0", "0", "ease-in"});
    EXPECT_LT(ok(c, {"prop.get", "bg", "opacity", "time=1"})["value"].get<double>(), 0.4);

    ok(c, {"signal.set", "level", "value=0.25"});
    ok(c, {"prop.expr", "bg", "scale", "1 + sig.level"});
    EXPECT_DOUBLE_EQ(ok(c, {"clip.get", "bg"})["values"]["scale"].get<double>(), 1.25);
    EXPECT_DOUBLE_EQ(ok(c, {"eval", "sig.level * 4"}).get<double>(), 1.0);

    json fx = ok(c, {"effect.add", "bg", "blur"});
    ok(c, {"prop.set", std::to_string(fx["id"].get<Id>()), "radius", "9"});
    EXPECT_EQ(ok(c, {"prop.get", std::to_string(fx["id"].get<Id>()), "radius"})["value"], 9.0);
    json master = ok(c, {"effect.add", "master", "compressor"});
    EXPECT_EQ(s.project().masterAudio.at(0).id, master["id"].get<Id>());
    EXPECT_NE(fails(c, {"effect.add", "bg", "fuzz"}).find("unknown effect"), std::string::npos);

    json right = ok(c, {"clip.split", "bg", "3"});
    ASSERT_EQ(right.size(), 1u);
    ok(c, {"clip.rename", right[0].dump(), "bg2"});
    json tr = ok(c, {"transition.add", std::to_string(solid), right[0].dump(), "type=dip", "duration=0.5"});
    ok(c, {"transition.curve", tr["id"].dump(), "linear"});
    EXPECT_EQ(s.project().transitions.at(0).type, "dip");

    json graph = ok(c, {"graph"});
    bool sawSignal = false;
    for (const auto& n : graph) sawSignal |= n["key"] == "signal:level";
    EXPECT_TRUE(sawSignal) << graph.dump();
    EXPECT_TRUE(ok(c, {"errors"}).empty());
    // A bad expression is reported at once, even on a clip after the playhead.
    json reply = ok(c, {"prop.expr", "bg2", "x", "nil + 1"});
    ASSERT_EQ(reply["errors"].size(), 1u) << reply.dump();
    EXPECT_NE(reply["errors"][0]["message"].get<std::string>().find("arithmetic"), std::string::npos);
    EXPECT_FALSE(ok(c, {"errors"}).empty());

    json summary = ok(c, {"show"});
    EXPECT_EQ(summary[2]["name"], "V1");  // top track first: V3, V2, V1
    EXPECT_EQ(summary[2]["clips"].size(), 2u);

    ok(c, {"select", "clips=bg", "prop=opacity"});
    EXPECT_EQ(s.selection.primaryClip(), solid);
    EXPECT_EQ(s.selection.prop, "opacity");

    EXPECT_TRUE(ok(c, {"undo"}).get<bool>());
    EXPECT_EQ(ok(c, {"history"})["redo"].size(), 1u);
}

TEST(Commands, FilesFramesAndExport) {
    Session s;
    Commands c(s);
    auto dir = fixtures::dir() / "session";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    ok(c, {"settings", "width=64", "height=36", "fps=10"});
    ok(c, {"clip.add", "solid", "V1", "0", "1", R"(props={"color":[0,0,1]})"});
    // Relative paths resolve against the client's working directory.
    json frame = ok(c, {"frame", "session/f.png", "time=0.5", "width=32"});
    EXPECT_EQ(frame["height"], 18);
    EXPECT_EQ(frame["path"], (dir / "f.png").string());
    SDL_Surface* png = SDL_LoadPNG((dir / "f.png").c_str());
    ASSERT_NE(png, nullptr);
    EXPECT_EQ(png->w, 32);
    SDL_DestroySurface(png);

    ok(c, {"script.set", "mf.log('script ran')"});
    ok(c, {"save", "session/p.mfproj"});
    EXPECT_FALSE(s.dirty());
    ok(c, {"new"});
    EXPECT_TRUE(s.project().clips.empty());
    ok(c, {"open", "session/p.mfproj"});
    EXPECT_EQ(s.project().clips.size(), 1u);
    EXPECT_EQ(s.project().settings.width, 64);
    {
        // A new session's log includes the opened script's lines before anything else evaluates it.
        Session fresh;
        Commands fc(fresh);
        ok(fc, {"open", "session/p.mfproj"});
        json log = ok(fc, {"log"});
        ASSERT_FALSE(log.empty());
        EXPECT_EQ(log.back()["text"], "script ran") << log.dump();
    }

    // wait=true defers the reply until the background export finishes.
    json done = ok(c, {"export", "session/out.wav", "wait=true"});
    EXPECT_TRUE(done["finished"].get<bool>());
    EXPECT_TRUE(std::filesystem::exists(dir / "out.wav"));
    EXPECT_NE(fails(c, {"export", "session/out.mov", "wait=true"}).find(".mp4"), std::string::npos);

    if (fixtures::haveFfmpeg()) {
        json m = ok(c, {"media.import", fixtures::videoWithAudio()});
        json ids = ok(c, {"clip.media", m["name"], "start=1"});
        EXPECT_EQ(ids.size(), 2u);
        EXPECT_EQ(ok(c, {"media.list"}).size(), 1u);
    }
    EXPECT_NE(fails(c, {"media.import", "missing.mp4"}).size(), 0u);
}

TEST(Commands, SchemaNamesAreAccepted) {
    Session s;
    Commands c(s);
    json schema = ok(c, {"schema"});
    EXPECT_EQ(ok(c, {"schema", "eases"}), schema["eases"]);
    EXPECT_NE(fails(c, {"schema", "nope"}).find("unknown section"), std::string::npos);

    // Every listed clip kind and property is what clip.get reports.
    ok(c, {"clip.add", "generator", "V1", "0", "4", "gen", R"(props={"generator":"probe"})"});
    for (const auto& clip : schema["clips"]) {
        std::string kind = clip["kind"];
        if (kind == "media") continue;
        std::string track = clip["track"] == "audio" ? "A1" : "V2";
        Id id = ok(c, {"clip.add", kind, track, "0", "1"})["id"];
        json values = ok(c, {"clip.get", std::to_string(id), "0"})["values"];
        EXPECT_EQ(values.size(), clip["props"].size()) << kind;
        for (const auto& prop : clip["props"]) EXPECT_TRUE(values.contains(prop["name"])) << kind << " " << prop["name"];
        ok(c, {"clip.remove", std::to_string(id)});
    }
    for (const auto& blend : schema["blendModes"]) ok(c, {"prop.set", "gen", "blend", blend});

    // Every effect, transition, and ease is accepted by the editing commands.
    for (const auto& e : schema["videoEffects"]) ok(c, {"effect.add", "gen", e["type"]});
    for (const auto& e : schema["audioEffects"]) ok(c, {"effect.add", "master", e["type"]});
    ok(c, {"clip.add", "solid", "V2", "0", "2", "a"});
    ok(c, {"clip.add", "solid", "V2", "2", "2", "b"});
    Id tr = ok(c, {"transition.add", "a", "b"})["id"];
    for (const auto& t : schema["transitions"]) ok(c, {"transition.set", std::to_string(tr), t["type"]});
    for (const auto& e : schema["eases"]) ok(c, {"prop.key", "gen", "opacity", "0", "1", e});

    // Every listed Lua name exists in the script environment.
    const json& lua = schema["lua"];
    for (const auto& v : lua["variables"])
        EXPECT_EQ(ok(c, {"eval", fmt::format("return {} ~= nil", v["name"].get<std::string>())}), 1.0) << v["name"];
    for (const auto& f : lua["functions"])
        EXPECT_EQ(ok(c, {"eval", fmt::format("return type({})", f["name"].get<std::string>())}), "function") << f["name"];
    for (const auto& f : lua["declarations"])
        EXPECT_EQ(ok(c, {"eval", fmt::format("return type(mf.{})", f["name"].get<std::string>())}), "function") << f["name"];
    std::string checks;
    for (const auto& m : lua["canvas"])
        checks += fmt::format("assert(type(g.{0}) == 'function', 'missing {0}')\n", m["name"].get<std::string>());
    json script = ok(c, {"script.set", "mf.generator('probe', function(g)\n" + checks + "end)"});
    EXPECT_FALSE(script.contains("error")) << script.dump();
    ok(c, {"frame", "schema_probe.png", "0", "64"});
    EXPECT_TRUE(ok(c, {"errors"}).empty()) << ok(c, {"errors"}).dump();
}

TEST(Commands, TimeArgumentsTakeFramesTimecodeAndClock) {
    Session s;
    Commands c(s);
    ok(c, {"settings", "fps=30"});
    auto at = [&](const char* time) { return ok(c, {"eval", "t", time}).get<double>(); };
    EXPECT_DOUBLE_EQ(at("90f"), 3.0);
    EXPECT_DOUBLE_EQ(at("00:00:03:00"), 3.0);
    EXPECT_DOUBLE_EQ(at("00:01:02:15"), 62.5);
    EXPECT_DOUBLE_EQ(at("1:15.5"), 75.5);
    EXPECT_DOUBLE_EQ(at("1:00:01"), 3601.0);

    // Durations, clip-local key times, and JSON requests take them too.
    json clip = ok(c, {"clip.add", "solid", "V1", "15f", "0:02", "bg"});
    EXPECT_EQ(clip["start"], 0.5);
    EXPECT_EQ(clip["duration"], 2.0);
    ok(c, {"prop.key", "bg", "opacity", "30f", "0.5"});
    EXPECT_EQ(ok(c, {"prop.get", "bg", "opacity"})["property"]["curve"][0]["t"], 1.0);
    json r = c.runSync({{"cmd", "clip.move"}, {"args", {{"clip", "bg"}, {"start", "60f"}}}});
    ASSERT_TRUE(r["ok"]) << r.dump();
    EXPECT_EQ(r["result"]["start"], 2.0);

    for (const char* bad : {"1:60", "00:00:01:30", "1.5:00", "x f", "1::2", "1:2:3:4:5", "-1:00"})
        EXPECT_NE(fails(c, {"seek", bad}).find("expected time"), std::string::npos) << bad;
}

TEST(Commands, BatchIsOneUndoStepOrNothing) {
    Session s;
    Commands c(s);
    size_t steps = s.history().undoLabels().size();
    json batch = {{"cmd", "batch"},
                  {"args",
                   {{"label", "titles"},
                    {"commands",
                     {"clip.add solid V1 0 4 bg",
                      R"(clip.add text V2 0 4 title 'props={"text":"Hi"}')",
                      {{"cmd", "prop.key"}, {"args", {{"target", "title"}, {"name", "opacity"}, {"time", 0}, {"value", 0}}}},
                      "frame batch.png 1 32"}}}},
                  {"cwd", fixtures::dir().string()}};
    json r = c.runSync(batch);
    ASSERT_TRUE(r["ok"]) << r.dump();
    const json& results = r["result"]["results"];
    ASSERT_EQ(results.size(), 4u);
    EXPECT_EQ(results[1]["name"], "title");
    EXPECT_EQ(results[3]["path"], (fixtures::dir() / "batch.png").string());
    EXPECT_EQ(r["result"]["undo"], "titles");
    EXPECT_EQ(s.history().undoLabels().size(), steps + 1);
    EXPECT_EQ(s.history().undoLabels().back(), "titles");
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(s.project().clips.size(), 0u);

    // A failing command undoes the ones before it and leaves history alone.
    Session clean;
    Commands cc(clean);
    json before = ok(cc, {"show", "project"});
    std::string err = fails(cc, {"batch", R"(["clip.add solid V1 0 4 bg", "select clips=bg", "clip.move nope 1"])"});
    EXPECT_NE(err.find("command 3 (clip.move) failed: no clip named 'nope'"), std::string::npos) << err;
    EXPECT_NE(err.find("no changes were made"), std::string::npos);
    EXPECT_EQ(ok(cc, {"show", "project"}), before);
    EXPECT_TRUE(clean.history().undoLabels().empty());
    EXPECT_TRUE(clean.selection.clips.empty());
    EXPECT_FALSE(clean.dirty());

    // Commands that touch files, history, or playback are refused up front.
    EXPECT_NE(fails(cc, {"batch", R"(["clip.add solid V1 0 4 bg", "undo"])"}).find("command 2 (undo) cannot run in a batch"),
              std::string::npos);
    EXPECT_EQ(cc.runSync({{"cmd", "show"}, {"args", {{"what", "clips"}}}})["result"].size(), 0u);

    // A batch of reads adds no undo step.
    EXPECT_TRUE(ok(cc, {"batch", R"(["status", "show clips"])"})["undo"].is_null());
    EXPECT_TRUE(clean.history().undoLabels().empty());
}

TEST(Commands, MarkersNameTimes) {
    Session s;
    Commands c(s);
    ok(c, {"settings", "fps=30"});
    json m = ok(c, {"marker.add", "chorus", "2"});
    EXPECT_EQ(m["name"], "chorus");
    EXPECT_EQ(m["time"], 2.0);
    EXPECT_EQ(ok(c, {"marker.add"})["name"], "M1");  // at the playhead, 0
    EXPECT_NE(fails(c, {"marker.add", "chorus", "3"}).find("exists"), std::string::npos);
    EXPECT_NE(fails(c, {"marker.add", "2nd", "3"}).find("must use letters"), std::string::npos);

    // Time arguments take @name with an optional offset.
    json clip = ok(c, {"clip.add", "solid", "V1", "@chorus", "4", "bg"});
    EXPECT_EQ(clip["start"], 2.0);
    EXPECT_DOUBLE_EQ(ok(c, {"eval", "t", "@chorus+15f"}).get<double>(), 2.5);
    EXPECT_DOUBLE_EQ(ok(c, {"eval", "t", "@chorus-0:01"}).get<double>(), 1.0);
    EXPECT_NE(fails(c, {"seek", "@verse"}).find("no marker named 'verse'"), std::string::npos);

    // Key times are clip-local, so a marker key lands at the marker on the timeline.
    ok(c, {"marker.add", "drop", "3"});
    ok(c, {"prop.key", "bg", "opacity", "@drop", "0.25"});
    EXPECT_EQ(ok(c, {"prop.get", "bg", "opacity"})["property"]["curve"][0]["t"], 1.0);
    ok(c, {"prop.unkey", "bg", "opacity", "@drop"});
    EXPECT_EQ(ok(c, {"prop.get", "bg", "opacity"})["mode"], "constant");

    // Expressions and the script read marker.NAME, and follow moves.
    ok(c, {"prop.expr", "bg", "x", "marker.drop * 10"});
    EXPECT_EQ(ok(c, {"clip.get", "bg", "2"})["values"]["x"], 30.0);
    ok(c, {"script.set", "mf.clip{track='V2', kind='solid', name='late', start=marker.drop, duration=1}"});
    ok(c, {"marker.move", "drop", "3.5"});
    EXPECT_EQ(ok(c, {"clip.get", "bg", "2"})["values"]["x"], 35.0);
    EXPECT_EQ(ok(c, {"clip.get", "late"})["start"], 3.5);

    // Markers stay sorted, rename, remove, undo, and save.
    ok(c, {"marker.rename", "@drop", "bridge"});
    json markers = ok(c, {"show", "markers"});
    ASSERT_EQ(markers.size(), 3u);
    EXPECT_EQ(markers[0]["name"], "M1");
    EXPECT_EQ(markers[2]["name"], "bridge");
    ok(c, {"marker.remove", "M1"});
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(ok(c, {"show", "markers"}).size(), 3u);
    ok(c, {"save", "markers.mfproj"});
    Session loaded;
    Commands lc(loaded);
    ok(lc, {"open", "markers.mfproj"});
    EXPECT_EQ(ok(lc, {"show", "markers"}), ok(c, {"show", "markers"}));
}

namespace {

// Width and height from a PNG's IHDR chunk.
std::pair<int, int> pngSize(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char b[24] = {};
    in.read(reinterpret_cast<char*>(b), sizeof b);
    auto be = [&](int i) { return (b[i] << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3]; };
    return {be(16), be(20)};
}

}  // namespace

TEST(Commands, SheetSamplesFramesIntoAGrid) {
    Session s;
    Commands c(s);
    ok(c, {"settings", "width=320", "height=180", "fps=10"});
    ok(c, {"clip.add", "solid", "V1", "0", "4", "bg"});
    ok(c, {"prop.expr", "bg", "color", "gray(t / 4)"});
    ok(c, {"clip.add", "solid", "V2", "1", "2", "box", R"(props={"color":"#ff0000","scale":0.5})"});
    ok(c, {"marker.add", "mid", "2"});

    // The default is 12 frames across the project, first frame to last.
    json r = ok(c, {"sheet", "sheet.png"});
    ASSERT_EQ(r["cells"].size(), 12u);
    EXPECT_EQ(r["cells"][0]["time"], 0.0);
    EXPECT_DOUBLE_EQ(r["cells"][11]["time"].get<double>(), 3.9);
    EXPECT_GE(r["columns"].get<int>() * r["rows"].get<int>(), 12);
    EXPECT_EQ(pngSize(fixtures::dir() / "sheet.png"), std::make_pair(r["width"].get<int>(), r["height"].get<int>()));

    // every= steps from the start; a cell on a marker names it.
    EXPECT_NE(fails(c, {"sheet", "sheet.png", "every=1x"}).find("expected time"), std::string::npos);
    r = ok(c, {"sheet", "sheet.png", "every=10f"});
    ASSERT_EQ(r["cells"].size(), 4u);
    EXPECT_EQ(r["cells"][2]["timecode"], "00:00:02:00");
    EXPECT_EQ(r["cells"][2]["marker"], "mid");

    // clip= uses the clip's range; start and end still override it.
    r = ok(c, {"sheet", "sheet.png", "clip=box", "count=3", "columns=3", "width=64"});
    ASSERT_EQ(r["cells"].size(), 3u);
    EXPECT_EQ(r["cells"][0]["time"], 1.0);
    EXPECT_DOUBLE_EQ(r["cells"][2]["time"].get<double>(), 2.9);
    EXPECT_EQ(r["rows"], 1);
    EXPECT_EQ(ok(c, {"sheet", "sheet.png", "clip=box", "start=@mid", "count=1"})["cells"][0]["time"], 2.0);

    EXPECT_NE(fails(c, {"sheet", "sheet.png", "count=2", "every=1"}).find("not both"), std::string::npos);
    EXPECT_NE(fails(c, {"sheet", "sheet.png", "start=5", "end=5"}).find("range is empty"), std::string::npos);
}

TEST(Sheet, CellsHoldTheFrames) {
    Session s;
    Commands c(s);
    ok(c, {"settings", "width=320", "height=180"});
    ok(c, {"clip.add", "solid", "V1", "0", "2", "bg", R"(props={"color":"#3366cc"})"});
    ok(c, {"clip.add", "solid", "V2", "1", "1", "box", R"(props={"color":"#ff0000","scale":0.5})"});
    std::vector<SheetCell> cells = {{s.renderFrame(0.5, 64, 36), "a"}, {s.renderFrame(1.5, 64, 36), "b"}, {s.renderFrame(1.5, 64, 36), "c"}};
    SheetLayout l;
    Image sheet = composeSheet(cells, 2, &l);
    EXPECT_EQ(l.columns, 2);
    EXPECT_EQ(l.rows, 2);
    EXPECT_EQ(sheet.w, l.gap + 2 * (l.cellW + l.gap));
    EXPECT_EQ(sheet.h, l.gap + 2 * (l.cellH + l.gap));
    for (int i = 0; i < 3; ++i)
        for (auto [x, y] : {std::pair{2, 2}, std::pair{32, 18}, std::pair{60, 33}}) {
            const uint8_t* want = cells[i].frame->at(x, y);
            const uint8_t* got = sheet.at(l.x(i) + x, l.y(i) + y);
            EXPECT_EQ(std::vector<int>(want, want + 3), std::vector<int>(got, got + 3)) << i << " " << x << "," << y;
        }
    EXPECT_EQ(squareColumns(12, 16, 9), 3);
    EXPECT_EQ(squareColumns(1, 16, 9), 1);
}
