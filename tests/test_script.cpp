#include <gtest/gtest.h>

#include "engine/evaluator.hpp"
#include "model/edit.hpp"

#include <fmt/format.h>

#include <algorithm>

using namespace mf;

namespace {

Id addText(Project& p, const char* track, double start, double dur, const char* name = "") {
    Clip c;
    c.kind = ClipKind::Text;
    c.track = p.findTrackByName(track)->id;
    c.start = start;
    c.duration = dur;
    c.name = name;
    return edit::addClip(p, c);
}

}  // namespace

TEST(Script, ExpressionsReadTimeAndTrackDependencies) {
    Project p = Project::makeDefault();
    p.script = "";
    Id clip = addText(p, "V1", 1, 4);
    edit::setPropExpr(p, clip, "x", "t * 100");
    edit::setPropExpr(p, clip, "y", "lt * 10");
    Evaluator ev;
    ev.setProject(p);
    ev.setTime(2.0);
    const Clip& c = *ev.project().findClip(clip);
    EXPECT_DOUBLE_EQ(ev.num(c, "x"), 200.0);
    EXPECT_DOUBLE_EQ(ev.num(c, "y"), 10.0);
    auto node = ev.graph().find(fmt::format("prop:{}.x", clip));
    ASSERT_NE(node, Graph::kNone);
    ASSERT_EQ(ev.graph().node(node).deps.size(), 1u);
    EXPECT_EQ(ev.graph().node(ev.graph().node(node).deps[0]).key, "time");
}

TEST(Script, SignalsFromScriptAndProject) {
    Project p = Project::makeDefault();
    p.script = R"(
mf.signal("beat", function() return floor(t * 2) end)
mf.signal("base", 10)
function double(x) return x * 2 end
)";
    edit::setSignal(p, "boost", Property::expression("sig.beat + sig.base"));
    Id clip = addText(p, "V1", 0, 10);
    edit::setPropExpr(p, clip, "rotation", "double(sig.boost)");
    Evaluator ev;
    ev.setProject(p);
    ev.setTime(1.6);
    EXPECT_DOUBLE_EQ(asNumber(ev.signal("beat")), 3.0);
    EXPECT_DOUBLE_EQ(ev.num(*ev.project().findClip(clip), "rotation"), 26.0);
    // rotation depends on boost, which depends on beat and base.
    auto& g = ev.graph();
    auto boost = g.find("signal:boost");
    std::vector<std::string> deps;
    for (auto d : g.node(boost).deps) deps.push_back(g.node(d).key);
    std::sort(deps.begin(), deps.end());
    EXPECT_EQ(deps, (std::vector<std::string>{"signal:base", "signal:beat"}));
    EXPECT_TRUE(ev.errors().empty());
}

TEST(Script, CurvesUseClipLocalTime) {
    Project p = Project::makeDefault();
    Id clip = addText(p, "V1", 10, 4);
    edit::setKeyframe(p, clip, "opacity", 0, 0);
    edit::setKeyframe(p, clip, "opacity", 2, 1);
    Evaluator ev;
    ev.setProject(p);
    ev.setTime(11);
    EXPECT_DOUBLE_EQ(ev.num(*ev.project().findClip(clip), "opacity"), 0.5);
}

TEST(Script, ErrorsAreReportedNotThrown) {
    Project p = Project::makeDefault();
    p.script = "local x = 1\nerror('broken here')\n";
    Id clip = addText(p, "V1", 0, 4);
    edit::setPropExpr(p, clip, "x", "nil + 1");
    edit::setPropExpr(p, clip, "y", "sig.missing");
    Evaluator ev;
    ev.setProject(p);
    ev.setTime(1);
    const Clip& c = *ev.project().findClip(clip);
    EXPECT_DOUBLE_EQ(ev.num(c, "x"), 0.0);
    ev.num(c, "y");
    auto errs = ev.errors();
    ASSERT_GE(errs.size(), 3u);
    EXPECT_EQ(errs[0].key, "script");
    EXPECT_EQ(errs[0].line, 2);
    EXPECT_NE(errs[0].message.find("broken here"), std::string::npos);
}

TEST(Script, ScriptDeclaresClips) {
    Project p = Project::makeDefault();
    p.script = R"(
for i = 0, 2 do
  mf.clip{ track = "V2", text = "Item " .. i, start = i, duration = 1, x = function() return lt * 100 end }
end
mf.clip{ track = "V1", kind = "solid", color = "#202020", start = 0, duration = 3 }
mf.clip{ track = "V1", kind = "bogus" }
)";
    Evaluator ev;
    ev.setProject(p);
    // The bogus kind is a script error; nothing is declared after a failed run.
    EXPECT_FALSE(ev.errors().empty());
    p.script.erase(p.script.find("mf.clip{ track = \"V1\", kind = \"bogus\" }"));
    ev.setProject(p);
    EXPECT_TRUE(ev.errors().empty()) << ev.errors()[0].message;
    ASSERT_EQ(ev.project().clips.size(), 4u);
    const Clip& c = ev.project().clips[1];
    EXPECT_TRUE(c.scriptOwned());
    EXPECT_EQ(asString(c.props.at("text").constant), "Item 1");
    ev.setTime(1.5);
    EXPECT_DOUBLE_EQ(ev.num(c, "x"), 50.0);
    EXPECT_EQ(formatColor(ev.color(ev.project().clips[3], "color")), "#202020");
}

TEST(Script, GeneratorsAndSynths) {
    Project p = Project::makeDefault();
    p.script = R"(
mf.generator("bars", function(g)
  g:clear("#000000")
  for i = 0, 3 do g:rect(i * 10, 0, 5, g.h * g.progress, gray(i / 3)) end
end)
mf.synth("tone", function(time) return sin(time * 2 * pi * 100) * 0.5 end)
)";
    Evaluator ev;
    ev.setProject(p);
    DrawList dl;
    std::string err;
    ASSERT_TRUE(ev.lua().draw("bars", dl, 1, 1, 0.5, 100, 50, &err)) << err;
    ASSERT_EQ(dl.cmds.size(), 5u);
    EXPECT_FLOAT_EQ(dl.cmds[2].d, 25.0f);
    std::vector<float> buf(2 * 480);
    ASSERT_TRUE(ev.lua().synth("tone", 0, 48000, 480, buf.data(), &err)) << err;
    EXPECT_NEAR(buf[2 * 120], 0.5f, 1e-4);  // quarter period of 100 Hz at 48 kHz
}

TEST(Script, ConsoleEvalAndInfiniteLoopBudget) {
    Project p = Project::makeDefault();
    p.script = "answer = 42";
    Evaluator ev;
    ev.setProject(p);
    std::string err;
    EXPECT_DOUBLE_EQ(asNumber(ev.eval("answer + 1", &err)), 43.0);
    EXPECT_TRUE(err.empty());
    ev.eval("os.exit(1)", &err);
    EXPECT_FALSE(err.empty());
    err.clear();
    ev.eval("local n = 0; while true do n = n + 1; if n < 0 then break end end", &err);
    EXPECT_NE(err.find("time budget"), std::string::npos) << err;
}

TEST(Script, UnchangedSignalSkipsDependents) {
    Project p = Project::makeDefault();
    p.script = R"(mf.signal("step", function() return floor(t) end))";
    Id clip = addText(p, "V1", 0, 10);
    edit::setPropExpr(p, clip, "x", "sig.step * 10");
    Evaluator ev;
    ev.setProject(p);
    const Clip& c = *ev.project().findClip(clip);
    ev.setTime(0.1);
    ev.num(c, "x");
    auto node = ev.graph().find(fmt::format("prop:{}.x", clip));
    auto before = ev.graph().node(node).evalCount;
    ev.setTime(0.5);
    ev.num(c, "x");
    EXPECT_EQ(ev.graph().node(node).evalCount, before);
    ev.setTime(1.5);
    EXPECT_DOUBLE_EQ(ev.num(c, "x"), 10.0);
    EXPECT_EQ(ev.graph().node(node).evalCount, before + 1);
}

TEST(Script, SynthBlockErrorsAndTimeoutRemainRecoverable) {
    Project p = Project::makeDefault();
    p.script = "mf.synth('bad', function(t) error('sample failed') end)\n"
               "mf.synth('loop', function(t) while true do end end)\n"
               "mf.synth('good', function(t) return t, -t end)";
    Evaluator ev; ev.setProject(p);
    float samples[8] = {};
    std::string err;
    EXPECT_FALSE(ev.lua().synth("bad", 0, 48000, 4, samples, &err));
    EXPECT_NE(err.find("sample failed"), std::string::npos);
    err.clear();
    EXPECT_FALSE(ev.lua().synth("loop", 0, 48000, 4, samples, &err));
    EXPECT_NE(err.find("time budget exceeded"), std::string::npos);
    err.clear();
    ASSERT_TRUE(ev.lua().synth("good", .25, 48000, 4, samples, &err)) << err;
    EXPECT_FLOAT_EQ(samples[0], .25f);
    EXPECT_FLOAT_EQ(samples[1], -.25f);
}
