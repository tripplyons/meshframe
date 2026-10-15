#include "fixtures.hpp"
#include "media/transcribe.hpp"
#include "model/edit.hpp"
#include "render/image.hpp"
#include "render/sheet.hpp"
#include "session/commands.hpp"

#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>

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
    EXPECT_EQ(ok(c, {"help", "clip.add"})["usage"], "clip.add <kind> <track> <start> <duration> [name] [props] [insert]");

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

TEST(Commands, AudioAnalyzeMeasuresTheMix) {
    Session s;
    Commands c(s);
    ok(c, {"script.set", "mf.synth('tone', function(time) return sin(time * 2 * pi * 1000) * 0.5 end)\n"
                         "mf.synth('hot', function(time) return sin(time * 2 * pi * 100) * 1.5 end)"});
    ok(c, {"clip.add", "synth", "A1", "0", "2", "tone", R"(props={"synth":"tone"})"});
    ok(c, {"clip.add", "synth", "A1", "3", "1", "hot", R"(props={"synth":"hot"})"});
    ok(c, {"marker.add", "hit", "3"});

    // A stereo 1 kHz sine at -6.02 dBFS peak reads -6.02 LUFS (BS.1770).
    json tone = ok(c, {"audio.analyze", "clip=tone"});
    EXPECT_EQ(tone["source"], "mix");
    EXPECT_NEAR(tone["integratedLufs"].get<double>(), -6.02, 0.1);
    EXPECT_TRUE(tone["loudnessRangeLu"].is_null()) << "loudness range needs 3 s";
    EXPECT_NEAR(tone["truePeakDbtp"].get<double>(), -6.02, 0.1);
    EXPECT_NEAR(tone["samplePeakDbfs"].get<double>(), -6.02, 0.01);
    EXPECT_NEAR(tone["rmsDbfs"].get<double>(), -9.03, 0.01);
    EXPECT_TRUE(tone["silence"].empty());
    EXPECT_TRUE(tone["clipping"].empty());
    EXPECT_EQ(tone["steps"].size(), 4u);

    // The whole mix: a gap of silence, then a clipping tone.
    json mix = ok(c, {"audio.analyze", "step=1", "waveform=wave.png", "width=400"});
    ASSERT_EQ(mix["silence"].size(), 1u) << mix.dump();
    EXPECT_NEAR(mix["silence"][0]["start"].get<double>(), 2, 0.02);
    EXPECT_NEAR(mix["silence"][0]["end"].get<double>(), 3, 0.02);
    ASSERT_EQ(mix["clipping"].size(), 1u) << mix.dump();
    EXPECT_NEAR(mix["clipping"][0]["start"].get<double>(), 3, 0.01);
    EXPECT_NEAR(mix["clipping"][0]["end"].get<double>(), 4, 0.01);
    EXPECT_GT(mix["clipping"][0]["samples"].get<int>(), 1000);
    EXPECT_NEAR(mix["samplePeakDbfs"].get<double>(), 3.52, 0.01);
    ASSERT_EQ(mix["steps"].size(), 4u);
    EXPECT_LT(mix["steps"][2]["lufs"].get<double>(), -40) << "only the filter's tail is left";
    EXPECT_TRUE(mix["steps"][2]["peakDbfs"].is_null());
    EXPECT_EQ(mix["waveform"], (fixtures::dir() / "wave.png").string());
    EXPECT_EQ(pngSize(fixtures::dir() / "wave.png").first, 400);

    EXPECT_NEAR(ok(c, {"audio.analyze", "start=@hit", "end=3.5"})["samplePeakDbfs"].get<double>(), 3.52, 0.01);
    EXPECT_NE(fails(c, {"audio.analyze", "start=5", "end=5"}).find("range is empty"), std::string::npos);
    EXPECT_NE(fails(c, {"audio.analyze", "clip=tone", "media=x"}).find("not both"), std::string::npos);
}

TEST(Commands, AudioAnalyzeReadsMedia) {
    if (!fixtures::haveFfmpeg()) GTEST_SKIP() << "ffmpeg is not installed";
    Session s;
    Commands c(s);
    ok(c, {"media.import", fixtures::sineAudio()});
    json r = ok(c, {"audio.analyze", "media=sine.wav"});
    EXPECT_EQ(r["source"], "media");
    EXPECT_NEAR(r["end"].get<double>(), 2, 0.01);
    EXPECT_NEAR(r["samplePeakDbfs"].get<double>(), -18.06, 0.1);  // ffmpeg's sine is 1/8 full scale
    EXPECT_TRUE(r["integratedLufs"].is_number());
}

TEST(Commands, ClipBoundsFollowTransformsAndEffects) {
    Session s;
    Commands c(s);
    ok(c, {"settings", "width=320", "height=180"});
    ok(c, {"clip.add", "solid", "V1", "0", "4", "box", R"(props={"scale":0.5})"});
    auto box = [&](const char* time = "1") { return ok(c, {"clip.bounds", "box", time}); };
    auto near = [](const json& b, double l, double t, double r, double bt, double tol = 1) {
        EXPECT_NEAR(b["left"].get<double>(), l, tol) << b.dump();
        EXPECT_NEAR(b["top"].get<double>(), t, tol) << b.dump();
        EXPECT_NEAR(b["right"].get<double>(), r, tol) << b.dump();
        EXPECT_NEAR(b["bottom"].get<double>(), bt, tol) << b.dump();
    };

    json b = box();
    near(b["box"], 80, 45, 240, 135);
    EXPECT_EQ(b["center"]["x"], 0.0);
    EXPECT_EQ(b["inFrame"], 1.0);
    EXPECT_TRUE(b["crosses"].empty());
    EXPECT_TRUE(b["titleSafe"].get<bool>());

    // Moved half out of frame on the right.
    ok(c, {"prop.set", "box", "x", "100"});
    b = box();
    near(b["box"], 180, 45, 340, 135);
    EXPECT_NEAR(b["center"]["x"].get<double>(), 100, 0.5);
    EXPECT_NEAR(b["inFrame"].get<double>(), 0.875, 0.01);
    EXPECT_EQ(b["crosses"], json::array({"right"}));
    EXPECT_FALSE(b["titleSafe"].get<bool>());

    // Rotated a quarter turn: the box swaps width and height.
    ok(c, {"prop.set", "box", "x", "0"});
    ok(c, {"prop.set", "box", "rotation", "90"});
    near(box()["box"], 115, 10, 205, 170);

    // Effects count: blur spreads a generator's drawing.
    ok(c, {"script.set", "mf.generator('dot', function(g) g:rect(140, 70, 40, 40, gray(1)) end)"});
    ok(c, {"clip.add", "generator", "V2", "0", "4", "dot", R"(props={"generator":"dot"})"});
    near(ok(c, {"clip.bounds", "dot", "1"})["box"], 140, 70, 180, 110);
    ok(c, {"effect.add", "dot", "blur"});
    json blurred = ok(c, {"clip.bounds", "dot", "1"})["box"];
    EXPECT_LT(blurred["left"].get<double>(), 138);
    EXPECT_GT(blurred["right"].get<double>(), 182);
    ok(c, {"prop.set", "box", "rotation", "0"});

    // Larger than the margin around the frame, and fully transparent.
    ok(c, {"prop.set", "box", "scale", "3"});
    EXPECT_TRUE(box()["truncated"].get<bool>());
    ok(c, {"prop.set", "box", "opacity", "0"});
    EXPECT_FALSE(box()["visible"].get<bool>());

    ok(c, {"clip.add", "synth", "A1", "0", "1", "tone"});
    EXPECT_NE(fails(c, {"clip.bounds", "tone"}).find("not on a video track"), std::string::npos);
}

TEST(Commands, TextMeasureMatchesTheRenderedBlock) {
    Session s;
    Commands c(s);
    ok(c, {"settings", "width=640", "height=360"});
    json one = ok(c, {"text.measure", "Hello", "size=60"});
    EXPECT_EQ(one["lines"], json::array({"Hello"}));
    json r = c.runSync({{"cmd", "text.measure"}, {"args", {{"text", "Hello\nworld"}, {"size", 60}}}});
    ASSERT_TRUE(r["ok"]) << r.dump();
    EXPECT_EQ(r["result"]["lines"], json::array({"Hello", "world"}));
    EXPECT_NEAR(r["result"]["height"].get<double>(), 2 * r["result"]["lineHeight"].get<double>() + 2, 1);
    EXPECT_GT(ok(c, {"text.measure", "Hello", "size=120"})["width"].get<double>(), 1.9 * one["width"].get<double>());

    // The block holds the drawn glyphs.
    ok(c, {"clip.add", "text", "V1", "0", "1", "t", R"(props={"text":"Hello","size":60})"});
    json b = ok(c, {"clip.bounds", "t", "0.5"})["box"];
    EXPECT_LE(b["width"].get<double>(), one["width"].get<double>());
    EXPECT_LE(b["height"].get<double>(), one["height"].get<double>());
    EXPECT_GT(b["width"].get<double>(), one["width"].get<double>() * 0.8);
    EXPECT_NE(fails(c, {"text.measure", "x", "font=/nope.ttf"}).find("nope"), std::string::npos);
}

TEST(Commands, CheckFindsEachKindOfProblem) {
    if (!fixtures::haveFfmpeg()) GTEST_SKIP() << "ffmpeg is not installed";
    auto dir = fixtures::dir() / "check";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::filesystem::copy_file(fixtures::countingVideo(), dir / "gone.mp4");

    Session s;
    Commands c(s);
    ok(c, {"settings", "width=320", "height=180", "fps=10"});
    ok(c, {"script.set", "mf.synth('hot', function(time) return sin(time * 2 * pi * 100) * 1.5 end)"});
    ok(c, {"clip.add", "solid", "V1", "0", "2", "buried", R"(props={"color":"#ff0000"})"});
    ok(c, {"clip.add", "solid", "V2", "0", "4", "bg", R"(props={"color":"#606060"})"});
    ok(c, {"clip.add", "solid", "V2", "5", "1", "black", R"(props={"color":"#000000"})"});
    ok(c, {"prop.expr", "black", "x", "nope()"});
    ok(c, {"clip.add", "text", "V3", "0", "1", "edge", R"(props={"text":"Edge","size":40,"x":150})"});
    ok(c, {"clip.add", "text", "V3", "1", "1", "high", R"(props={"text":"High","size":16,"y":-78})"});
    ok(c, {"clip.add", "text", "V3", "2", "1", "a", R"(props={"text":"Same","size":30})"});
    ok(c, {"track.add", "video", "V4"});
    ok(c, {"clip.add", "text", "V4", "2", "1", "b", R"(props={"text":"Place","size":30})"});
    ok(c, {"media.import", fixtures::countingVideo()});
    json placed = ok(c, {"clip.media", "counting.mp4", "6"});
    ok(c, {"clip.trim", std::to_string(placed[0].get<Id>()), "6", "4"});
    ok(c, {"media.import", (dir / "gone.mp4").string()});
    ok(c, {"clip.media", "gone.mp4", "10", "V2"});
    ok(c, {"media.import", fixtures::sineAudio()});
    ok(c, {"clip.add", "synth", "A1", "0", "1", "loud", R"(props={"synth":"hot"})"});
    std::filesystem::remove(dir / "gone.mp4");

    json r = ok(c, {"check"});
    std::map<std::string, std::vector<json>> byKind;
    for (const auto& i : r["issues"]) byKind[i["kind"]].push_back(i);
    for (const char* kind : {"gap", "missing-media", "past-media", "unused-media", "hidden", "black", "expression",
                             "off-frame", "title-safe", "text-overlap", "clipping", "loudness", "true-peak"})
        EXPECT_TRUE(byKind.contains(kind)) << kind << " in " << r.dump(1);
    EXPECT_EQ(r["errors"].get<int>() + r["warnings"].get<int>(), int(r["issues"].size()));

    ASSERT_FALSE(byKind["gap"].empty());
    EXPECT_NEAR(byKind["gap"][0]["start"].get<double>(), 4, 1e-6);
    EXPECT_NEAR(byKind["gap"][0]["end"].get<double>(), 5, 1e-6);
    EXPECT_EQ(byKind["hidden"][0]["name"], "buried");
    EXPECT_EQ(byKind["off-frame"][0]["name"], "edge");
    EXPECT_EQ(byKind["title-safe"][0]["name"], "high");
    // The gap before the black clip renders black too.
    EXPECT_LE(byKind["black"][0]["start"].get<double>(), 5);
    EXPECT_GE(byKind["black"][0]["end"].get<double>(), 6);
    EXPECT_NEAR(byKind["past-media"][0]["start"].get<double>(), 9, 0.01);
    EXPECT_EQ(byKind["past-media"][0]["severity"], "error");
    EXPECT_EQ(byKind["unused-media"].size(), 1u);
    EXPECT_NE(byKind["unused-media"][0]["message"].get<std::string>().find("sine.wav"), std::string::npos);

    ok(c, {"script.set", "this is not lua"});
    bool scriptError = false;
    for (const auto& i : ok(c, {"check"})["issues"]) scriptError = scriptError || i["kind"] == "script";
    EXPECT_TRUE(scriptError);
}

TEST(Commands, CheckPassesACleanProject) {
    Session s;
    Commands c(s);
    ok(c, {"settings", "width=320", "height=180", "fps=10"});
    ok(c, {"clip.add", "solid", "V1", "0", "2", "bg", R"(props={"color":"#203040"})"});
    ok(c, {"clip.add", "text", "V2", "0", "2", "title", R"(props={"text":"Hello","size":30})"});
    json r = ok(c, {"check"});
    EXPECT_TRUE(r["issues"].empty()) << r.dump(1);
    EXPECT_GE(r["samples"].get<int>(), 4);
}

TEST(Transcript, ParsesWhisperTokensIntoWords) {
    json j = json::parse(R"({"transcription": [
        {"offsets": {"from": 0, "to": 900}, "text": " [BLANK_AUDIO]", "tokens": []},
        {"offsets": {"from": 1000, "to": 2500}, "text": " Hello meshframe, friends.", "tokens": [
            {"text": "[_BEG_]", "offsets": {"from": 1000, "to": 1000}},
            {"text": " Hello", "offsets": {"from": 1000, "to": 1300}},
            {"text": " mesh", "offsets": {"from": 1300, "to": 1500}},
            {"text": "frame", "offsets": {"from": 1500, "to": 1800}},
            {"text": ",", "offsets": {"from": 1800, "to": 1800}},
            {"text": " friends", "offsets": {"from": 1800, "to": 2400}},
            {"text": ".", "offsets": {"from": 2400, "to": 2500}}]}]})");
    Transcript t = parseWhisperJson(j, "m.bin");
    EXPECT_EQ(t.model, "m.bin");
    ASSERT_EQ(t.segments.size(), 1u) << "bracketed non-speech is dropped";
    EXPECT_EQ(t.segments[0].text, "Hello meshframe, friends.");
    EXPECT_DOUBLE_EQ(t.segments[0].start, 1.0);
    ASSERT_EQ(t.words.size(), 3u);
    EXPECT_EQ(t.words[1].text, "meshframe,");
    EXPECT_DOUBLE_EQ(t.words[1].start, 1.3);
    EXPECT_DOUBLE_EQ(t.words[1].end, 1.8);
    EXPECT_EQ(t.words[2].text, "friends.");
}

TEST(Commands, ClipSpeedKeepsLinkedMediaInSync) {
    if (!fixtures::haveFfmpeg()) GTEST_SKIP() << "ffmpeg is not installed";
    Session s;
    Commands c(s);
    ok(c, {"media.import", fixtures::videoWithAudio()});
    json ids = ok(c, {"clip.media", "av.mp4", "2"});
    ASSERT_EQ(ids.size(), 2u);
    const std::string video = ids[0].dump(), audio = ids[1].dump();
    ok(c, {"clip.trim", video, "2.2", "0.4"});
    json other = ok(c, {"clip.media", "av.mp4", "5"});
    json before = ok(c, {"show", "clips"});
    size_t steps = s.history().undoLabels().size();

    ok(c, {"clip.speed", video, "2"});
    for (const auto& id : ids) {
        json clip = ok(c, {"clip.get", id.dump()});
        EXPECT_EQ(clip["speed"], 2);
        EXPECT_EQ(clip["start"], 2.2);
        EXPECT_NEAR(clip["in"].get<double>(), 0.2, 1e-9);
        EXPECT_NEAR(clip["duration"].get<double>(), 0.4, 1e-9);
    }
    for (const auto& id : other) EXPECT_EQ(ok(c, {"clip.get", id.dump()})["speed"], 1);
    EXPECT_EQ(s.history().undoLabels().size(), steps + 1);
    ok(c, {"undo"});
    EXPECT_EQ(ok(c, {"show", "clips"}), before);
    ok(c, {"redo"});
    for (const auto& id : ids) EXPECT_EQ(ok(c, {"clip.get", id.dump()})["speed"], 2);

    // Either half controls the pair; callers can explicitly edit one side.
    ok(c, {"clip.speed", audio, "0.5"});
    EXPECT_EQ(ok(c, {"clip.get", video})["speed"], 0.5);
    ok(c, {"clip.speed", video, "3", "linked=false"});
    EXPECT_EQ(ok(c, {"clip.get", video})["speed"], 3);
    EXPECT_EQ(ok(c, {"clip.get", audio})["speed"], 0.5);

    before = ok(c, {"show", "clips"});
    steps = s.history().undoLabels().size();
    EXPECT_NE(fails(c, {"clip.speed", video, "0"}).find("speed"), std::string::npos);
    EXPECT_EQ(ok(c, {"show", "clips"}), before);
    EXPECT_EQ(s.history().undoLabels().size(), steps);

    ok(c, {"clip.unlink", video});
    ok(c, {"clip.speed", video, "4"});
    EXPECT_EQ(ok(c, {"clip.get", video})["speed"], 4);
    EXPECT_EQ(ok(c, {"clip.get", audio})["speed"], 0.5);
}

TEST(Commands, TranscriptFindMapsSourceToTimeline) {
    Session s;
    Commands c(s);
    ok(c, {"media.import", fixtures::videoWithAudio()});
    Id media = s.project().media[0].id;
    Transcript t;
    t.segments = {{"Cut the boring part now.", 0, 1}};
    t.words = {{"Cut", 0.0, 0.1}, {"the", 0.1, 0.2}, {"Boring", 0.4, 0.6}, {"part", 0.6, 0.8}, {"now.", 0.8, 1.0}};
    s.apply("transcribe", [&](Project& p) { edit::setTranscript(p, media, std::make_shared<const Transcript>(t)); });

    // One clip plays the whole file at double speed from 10 s; another starts mid-phrase.
    json ids = ok(c, {"clip.media", "av.mp4", "10"});
    ok(c, {"clip.speed", ids[0].dump(), "2"});
    json later = ok(c, {"clip.media", "av.mp4", "20"});
    ok(c, {"clip.trim", std::to_string(later[0].get<Id>()), "20.5", "0.5"});

    json found = ok(c, {"transcript.find", "boring PART"});
    ASSERT_EQ(found.size(), 1u) << found.dump();
    EXPECT_EQ(found[0]["text"], "Boring part");
    EXPECT_DOUBLE_EQ(found[0]["start"].get<double>(), 0.4);
    const json& tl = found[0]["timeline"];
    ASSERT_EQ(tl.size(), 2u) << "one entry per linked pair: " << tl.dump();
    EXPECT_DOUBLE_EQ(tl[0]["start"].get<double>(), 10.2);
    EXPECT_DOUBLE_EQ(tl[0]["end"].get<double>(), 10.4);
    EXPECT_FALSE(tl[0].contains("partial"));
    EXPECT_DOUBLE_EQ(tl[1]["start"].get<double>(), 20.5);
    EXPECT_TRUE(tl[1]["partial"].get<bool>());
    EXPECT_TRUE(ok(c, {"transcript.find", "boring parts"}).empty());

    json part = ok(c, {"transcript.get", "av.mp4", "start=0.3", "end=0.7", "words=true"});
    EXPECT_EQ(part["words"].size(), 2u);
    EXPECT_EQ(part["segments"].size(), 1u);

    // Transcripts save with the project and undo with it.
    ok(c, {"save", "transcript.mfproj"});
    Session loaded;
    Commands lc(loaded);
    ok(lc, {"open", "transcript.mfproj"});
    EXPECT_EQ(ok(lc, {"transcript.find", "boring part"}).size(), 1u);
    EXPECT_FALSE(ok(lc, {"media.list"})[0].contains("transcript")) << "media.list stays short";
}

TEST(Commands, MediaInspectFindsCutsAndSilence) {
    if (!fixtures::haveFfmpeg()) GTEST_SKIP() << "ffmpeg is not installed";
    Session s;
    Commands c(s);
    ok(c, {"media.import", fixtures::cutsVideo()});
    ok(c, {"media.import", fixtures::countingVideo()});
    ok(c, {"media.import", fixtures::toneThenSilence()});

    json cuts = ok(c, {"media.inspect", "cuts.mp4", "sheet=media_sheet.png", "count=3"});
    ASSERT_EQ(cuts["cuts"].size(), 2u) << cuts.dump();
    EXPECT_NEAR(cuts["cuts"][0]["time"].get<double>(), 1, 0.01);
    EXPECT_NEAR(cuts["cuts"][1]["time"].get<double>(), 2, 0.01);
    EXPECT_TRUE(std::filesystem::exists(fixtures::dir() / "media_sheet.png"));
    EXPECT_TRUE(cuts["transcript"].is_null());
    EXPECT_TRUE(ok(c, {"media.inspect", "counting.mp4"})["cuts"].empty()) << "a slow fade is not a cut";

    json audio = ok(c, {"media.inspect", "tone_silence.wav"})["audio"];
    ASSERT_EQ(audio["silence"].size(), 1u) << audio.dump();
    EXPECT_NEAR(audio["silence"][0]["start"].get<double>(), 1, 0.02);
    EXPECT_NEAR(audio["silence"][0]["end"].get<double>(), 2, 0.02);
    EXPECT_FALSE(ok(c, {"media.inspect", "tone_silence.wav"}).contains("cuts"));
}

TEST(Commands, MediaTranscribeRunsWhisper) {
    std::string speech = fixtures::speech();
    if (speech.empty() || findWhisperCli().empty() || !std::filesystem::exists(defaultWhisperModel()))
        GTEST_SKIP() << "needs macOS say, whisper-cli, and " << defaultWhisperModel();
    Session s;
    Commands c(s);
    ok(c, {"media.import", speech});
    EXPECT_NE(fails(c, {"media.transcribe", "speech.aiff", "model=/nope.bin"}).find("no whisper model"), std::string::npos);

    json r = ok(c, {"media.transcribe", "speech.aiff"});
    EXPECT_GT(r["words"].get<int>(), 10);
    EXPECT_NE(r["text"].get<std::string>().find("boring"), std::string::npos) << r.dump();
    EXPECT_EQ(s.history().undoLabels().back(), "transcribe");

    ok(c, {"clip.media", "speech.aiff", "3"});
    json found = ok(c, {"transcript.find", "boring part"});
    ASSERT_EQ(found.size(), 1u);
    double start = found[0]["start"].get<double>();
    EXPECT_GT(start, 1);
    EXPECT_DOUBLE_EQ(found[0]["timeline"][0]["start"].get<double>(), 3 + start);

    // A second request uses the stored transcript.
    size_t steps = s.history().undoLabels().size();
    EXPECT_EQ(ok(c, {"media.transcribe", "speech.aiff"})["words"], r["words"]);
    EXPECT_EQ(s.history().undoLabels().size(), steps);
}

TEST(Commands, DiffReportsChangesSinceARevision) {
    Session s;
    Commands c(s);
    ok(c, {"clip.add", "solid", "V1", "0", "4", "bg"});
    ok(c, {"clip.add", "solid", "V1", "4", "2", "tail"});
    int64_t since = ok(c, {"status"})["revision"];

    ok(c, {"prop.set", "bg", "opacity", "0.5"});
    ok(c, {"clip.remove", "tail"});
    ok(c, {"marker.add", "intro", "1"});
    s.apply("rename", [](Project& p) { p.name = "film"; });  // as the GUI would

    json d = ok(c, {"diff", std::to_string(since)});
    EXPECT_EQ(d["since"], since);
    EXPECT_EQ(d["revision"], s.revision());
    ASSERT_EQ(d["added"].size(), 1u);
    EXPECT_EQ(d["added"][0]["kind"], "marker");
    EXPECT_EQ(d["added"][0]["name"], "intro");
    ASSERT_EQ(d["removed"].size(), 1u);
    EXPECT_EQ(d["removed"][0]["name"], "tail");
    ASSERT_EQ(d["changed"].size(), 2u);
    EXPECT_EQ(d["changed"][0]["kind"], "project");
    EXPECT_EQ(d["changed"][0]["fields"]["name"]["new"], "film");
    EXPECT_EQ(d["changed"][1]["name"], "bg");
    EXPECT_EQ(d["changed"][1]["fields"]["props.opacity"]["old"], 1);
    EXPECT_EQ(d["changed"][1]["fields"]["props.opacity"]["new"], 0.5);

    ASSERT_EQ(d["revisions"].size(), 4u);
    EXPECT_EQ(d["revisions"][0]["source"], "cli");
    EXPECT_EQ(d["revisions"][3]["label"], "rename");
    EXPECT_EQ(d["revisions"][3]["source"], "gui");

    // Undo is a change of its own, and the current revision has no changes.
    ok(c, {"undo"});
    json u = ok(c, {"diff", std::to_string(int64_t(s.revision()) - 1)});
    EXPECT_EQ(u["revisions"][0]["label"], "undo: rename");
    EXPECT_EQ(u["changed"][0]["fields"]["name"]["new"], "untitled");
    json none = ok(c, {"diff", std::to_string(s.revision())});
    EXPECT_TRUE(none["added"].empty() && none["removed"].empty() && none["changed"].empty());

    // Only the last revisions are kept.
    for (size_t i = 0; i < Session::kRevisionsKept; ++i) ok(c, {"clip.move", "bg", std::to_string(i % 2)});
    EXPECT_NE(fails(c, {"diff", std::to_string(since)}).find("the oldest is " + std::to_string(s.revisions().front().number)),
              std::string::npos);
    EXPECT_NE(fails(c, {"diff", std::to_string(s.revision() + 1)}).find("newer"), std::string::npos);
}

TEST(Commands, WaitRepliesWhenTheProjectChanges) {
    Session s;
    Commands c(s);
    std::optional<json> reply;
    auto wait = [&](json args) {
        reply.reset();
        c.run({{"cmd", "wait"}, {"args", std::move(args)}}, [&](json r) { reply = std::move(r); });
    };

    wait(json::object());
    s.tick();
    EXPECT_FALSE(reply);
    s.apply("rename", [](Project& p) { p.name = "film"; });
    s.tick();
    ASSERT_TRUE(reply);
    EXPECT_FALSE((*reply)["result"]["timedOut"]);
    EXPECT_EQ((*reply)["result"]["changed"][0]["fields"]["name"]["new"], "film");

    // An edit made before the wait replies at once.
    wait({{"since", 1}});
    ASSERT_TRUE(reply);
    EXPECT_EQ((*reply)["result"]["revisions"][0]["label"], "rename");

    wait({{"timeout", 0}});
    s.tick();
    ASSERT_TRUE(reply);
    EXPECT_TRUE((*reply)["result"]["timedOut"]);
    EXPECT_TRUE((*reply)["result"]["revisions"].empty());
}

TEST(Commands, RippleEditsShiftLaterClipsOnAllTracks) {
    Session s;
    Commands c(s);
    ok(c, {"clip.add", "solid", "V1", "0", "2", "a"});
    ok(c, {"clip.add", "solid", "V1", "a.end", "3", "b"});
    ok(c, {"clip.add", "solid", "V1", "b.end", "1", "c"});
    ok(c, {"clip.add", "text", "V2", "2", "2", "title"});
    ok(c, {"marker.add", "beat", "3"});
    auto start = [&](const std::string& name) { return ok(c, {"clip.get", name})["start"].get<double>(); };
    auto marker = [&] { return ok(c, {"show", "markers"})[0]["time"].get<double>(); };
    EXPECT_DOUBLE_EQ(start("c"), 5);

    // Ripple delete: everything after a moves left by its 2 seconds.
    ok(c, {"clip.remove", "a", "ripple=true"});
    EXPECT_DOUBLE_EQ(start("b"), 0);
    EXPECT_DOUBLE_EQ(start("c"), 3);
    EXPECT_DOUBLE_EQ(start("title"), 0);
    EXPECT_DOUBLE_EQ(marker(), 1);

    // Ripple trim: b grows by 1, so c moves right; the title starts before b's end.
    ok(c, {"clip.trim", "b", "0", "4", "ripple=true"});
    EXPECT_DOUBLE_EQ(start("c"), 4);
    EXPECT_DOUBLE_EQ(start("title"), 0);
    ok(c, {"clip.trim", "b", "0", "3.5", "ripple=true"});
    EXPECT_DOUBLE_EQ(start("c"), 3.5);
    ok(c, {"clip.trim", "b", "0", "4", "ripple=true"});

    // Insert inside b splits it; clips on other tracks that span the point stay.
    ok(c, {"clip.add", "solid", "V1", "1", "1", "ins", "insert=true"});
    std::vector<std::pair<double, double>> parts;
    for (const auto& clip : ok(c, {"show", "clips"}))
        if (clip["name"] == "b") parts.emplace_back(clip["start"], clip["duration"]);
    std::ranges::sort(parts);
    EXPECT_EQ(parts, (std::vector<std::pair<double, double>>{{0, 1}, {2, 3}}));
    EXPECT_DOUBLE_EQ(start("c"), 5);
    EXPECT_DOUBLE_EQ(start("title"), 0);
    EXPECT_DOUBLE_EQ(marker(), 2);

    // Close gaps removes time no track uses.
    ok(c, {"clip.move", "c", "c.start+1s"});
    json gaps = ok(c, {"close-gaps"});
    ASSERT_EQ(gaps.size(), 1u);
    EXPECT_DOUBLE_EQ(gaps[0]["start"], 5);
    EXPECT_DOUBLE_EQ(gaps[0]["end"], 6);
    EXPECT_DOUBLE_EQ(start("c"), 5);
    EXPECT_TRUE(ok(c, {"close-gaps"}).empty());

    // A clip that starts inside the removed time, or a locked track, blocks a ripple.
    ok(c, {"clip.add", "solid", "V2", "c.start+0.5", "0.2", "flash"});
    EXPECT_NE(fails(c, {"clip.remove", "c", "ripple=true"}).find("flash starts inside"), std::string::npos);
    ok(c, {"clip.remove", "flash"});
    ok(c, {"clip.add", "solid", "V2", "c.end", "1", "after"});
    ok(c, {"track.set", "V2", "locked=true"});
    EXPECT_NE(fails(c, {"clip.remove", "c", "ripple=true"}).find("locked"), std::string::npos);
    EXPECT_DOUBLE_EQ(start("c"), 5);

    EXPECT_NE(fails(c, {"seek", "nope.end"}).find("no clip named 'nope'"), std::string::npos);
}

TEST(Commands, InsertMediaSplitsLinkedPairs) {
    if (!fixtures::haveFfmpeg()) GTEST_SKIP() << "ffmpeg not found";
    Session s;
    Commands c(s);
    json m = ok(c, {"media.import", fixtures::videoWithAudio()});
    ok(c, {"clip.media", m["name"], "0"});
    json ids = ok(c, {"clip.media", m["name"], "0.5", "insert=true"});
    ASSERT_EQ(ids.size(), 2u);
    std::map<double, int> starts;
    for (const auto& clip : ok(c, {"show", "clips"})) ++starts[clip["start"].get<double>()];
    EXPECT_EQ(starts, (std::map<double, int>{{0, 2}, {0.5, 2}, {1.5, 2}}));
}

namespace {

json bounds(Commands& c, const std::string& clip) { return ok(c, {"clip.bounds", clip, "0.5"})["box"]; }

// Leftmost and rightmost lit columns of the frame in rows y0 to y1.
std::pair<int, int> litColumns(const Image& img, int y0, int y1) {
    int lo = img.w, hi = -1;
    for (int y = std::max(0, y0); y < std::min(img.h, y1); ++y)
        for (int x = 0; x < img.w; ++x)
            if (img.at(x, y)[0] > 128) lo = std::min(lo, x), hi = std::max(hi, x);
    return {lo, hi};
}

}  // namespace

TEST(Commands, TextAlignWrapSpacingAndOutline) {
    Session s;
    Commands c(s);
    ok(c, {"settings", "width=640", "height=360"});
    ok(c, {"clip.add", "text", "V1", "0", "1", "t", R"(props={"text":"Wide line\nx","size":40})"});

    // Short lines sit at the left, middle, or right of the block.
    std::map<std::string, std::pair<int, int>> lastLine;
    json box;
    for (std::string align : {"left", "center", "right"}) {
        ok(c, {"prop.set", "t", "align", align});
        box = bounds(c, "t");
        auto frame = s.renderFrame(0.5, 640, 360);
        int mid = int((box["top"].get<double>() + box["bottom"].get<double>()) / 2);
        lastLine[align] = litColumns(*frame, mid + 4, int(box["bottom"].get<double>()) + 1);
    }
    double left = box["left"], right = box["right"];
    EXPECT_NEAR(lastLine["left"].first, left, 4);
    EXPECT_NEAR(lastLine["right"].second, right, 4);
    EXPECT_NEAR((lastLine["center"].first + lastLine["center"].second) / 2.0, (left + right) / 2, 4);

    // Line height spaces the baselines by a multiple of the font's.
    double base = ok(c, {"text.measure", "x", "size=40"})["lineHeight"];
    double h1 = box["height"];
    ok(c, {"prop.set", "t", "lineHeight", "2"});
    EXPECT_NEAR(bounds(c, "t")["height"].get<double>() - h1, base, 2);
    json spaced = ok(c, {"text.measure", "Wide line\nx", "size=40", "lineHeight=2"});
    EXPECT_DOUBLE_EQ(spaced["lineHeight"], 2 * base);
    ok(c, {"prop.set", "t", "lineHeight", "1"});

    // Wrapping breaks at spaces to fit the width.
    ok(c, {"prop.set", "t", "text", "one two three four five"});
    double unwrapped = bounds(c, "t")["width"];
    ok(c, {"prop.set", "t", "wrap", "150"});
    EXPECT_LE(bounds(c, "t")["width"].get<double>(), 150);
    EXPECT_LT(bounds(c, "t")["width"].get<double>(), unwrapped);
    json wrapped = ok(c, {"text.measure", "one two three four five", "size=40", "wrap=150"});
    EXPECT_GT(wrapped["lines"].size(), 1u);
    for (const auto& line : wrapped["lines"]) EXPECT_LE(ok(c, {"text.measure", line, "size=40"})["width"].get<double>(), 152);

    // An outline colors pixels around the glyphs and widens the block.
    ok(c, {"prop.set", "t", "wrap", "0"});
    ok(c, {"prop.set", "t", "text", "HI"});
    json plain = bounds(c, "t");
    ok(c, {"prop.set", "t", "outline", "4"});
    ok(c, {"prop.set", "t", "outlineColor", "#ff0000"});
    json outlined = bounds(c, "t");
    EXPECT_NEAR(outlined["width"].get<double>() - plain["width"].get<double>(), 8, 2);
    auto frame = s.renderFrame(0.5, 640, 360);
    int red = 0, white = 0;
    for (size_t i = 0; i < frame->px.size(); i += 4) {
        const uint8_t* p = frame->px.data() + i;
        red += p[0] > 200 && p[1] < 50;
        white += p[0] > 200 && p[1] > 200;
    }
    EXPECT_GT(red, 100);
    EXPECT_GT(white, 100);
}

TEST(Commands, ShadowCropAndMaskEffects) {
    Session s;
    Commands c(s);
    ok(c, {"settings", "width=640", "height=360"});

    // A drop shadow draws past the clip, offset from it.
    ok(c, {"clip.add", "text", "V1", "0", "1", "t", R"(props={"text":"HI","size":60})"});
    json plain = bounds(c, "t");
    Id shadow = ok(c, {"effect.add", "t", "drop_shadow"})["id"];
    for (auto [k, v] : {std::pair{"x", "20"}, {"y", "10"}, {"blur", "0"}, {"opacity", "1"}, {"color", "#ff0000"}})
        ok(c, {"prop.set", std::to_string(shadow), k, v});
    json shadowed = bounds(c, "t");
    EXPECT_NEAR(shadowed["right"].get<double>() - plain["right"].get<double>(), 20, 1.5);
    EXPECT_NEAR(shadowed["bottom"].get<double>() - plain["bottom"].get<double>(), 10, 1.5);
    EXPECT_NEAR(shadowed["left"].get<double>(), plain["left"].get<double>(), 1);
    ok(c, {"clip.remove", "t"});

    // Crop edges are fractions of the clip.
    ok(c, {"clip.add", "solid", "V1", "0", "1", "bg", R"(props={"color":"#ffffff"})"});
    Id crop = ok(c, {"effect.add", "bg", "crop"})["id"];
    ok(c, {"prop.set", std::to_string(crop), "left", "0.25"});
    ok(c, {"prop.set", std::to_string(crop), "bottom", "0.5"});
    json cropped = bounds(c, "bg");
    EXPECT_NEAR(cropped["left"].get<double>(), 160, 1);
    EXPECT_NEAR(cropped["right"].get<double>(), 640, 1);
    EXPECT_NEAR(cropped["bottom"].get<double>(), 180, 1);
    ok(c, {"effect.remove", std::to_string(crop)});

    // A mask keeps the inside or the outside of a shape around the clip's center.
    Id mask = ok(c, {"effect.add", "bg", "mask"})["id"];
    for (auto [k, v] : {std::pair{"shape", "ellipse"}, {"width", "200"}, {"height", "100"}, {"x", "50"}})
        ok(c, {"prop.set", std::to_string(mask), k, v});
    json masked = bounds(c, "bg");
    EXPECT_NEAR(masked["left"].get<double>(), 270, 1.5);
    EXPECT_NEAR(masked["right"].get<double>(), 470, 1.5);
    EXPECT_NEAR(masked["top"].get<double>(), 130, 1.5);
    auto frame = s.renderFrame(0.5, 640, 360);
    EXPECT_EQ(frame->at(370, 180)[0], 255);
    EXPECT_EQ(frame->at(275, 135)[0], 0);  // inside the box, outside the ellipse
    ok(c, {"prop.set", std::to_string(mask), "keep", "outside"});
    frame = s.renderFrame(0.5, 640, 360);
    EXPECT_EQ(frame->at(370, 180)[0], 0);
    EXPECT_EQ(frame->at(275, 135)[0], 255);
}

TEST(Commands, MediaFitModes) {
    if (!fixtures::haveFfmpeg()) GTEST_SKIP() << "ffmpeg not found";
    Session s;
    Commands c(s);
    ok(c, {"settings", "width=640", "height=360"});
    json m = ok(c, {"media.import", fixtures::redImage()});
    Id id = ok(c, {"clip.media", m["name"], "0"})[0];
    std::string clip = std::to_string(id);
    // The 32x16 image in a 640x360 frame.
    std::map<std::string, std::pair<double, double>> sizes = {
        {"contain", {640, 320}}, {"cover", {720, 360}}, {"stretch", {640, 360}}, {"none", {32, 16}}};
    for (auto [fit, size] : sizes) {
        ok(c, {"prop.set", clip, "fit", fit});
        json b = bounds(c, clip);
        EXPECT_NEAR(b["width"].get<double>(), size.first, 1) << fit;
        EXPECT_NEAR(b["height"].get<double>(), size.second, 1) << fit;
    }
    EXPECT_NE(fails(c, {"prop.set", clip, "fit", "zoom"}).find("invalid value"), std::string::npos);
}
