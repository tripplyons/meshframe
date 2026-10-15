#include "fixtures.hpp"
#include "model/serialize.hpp"
#include "model/path_edit.hpp"
#include "session/commands.hpp"

#include <gtest/gtest.h>
#include <fstream>

using namespace mf;

namespace {
json command(Commands& commands, std::vector<std::string> words) {
    return commands.runSync(Commands::requestFromWords(words, fixtures::dir().string()));
}
std::string svgFile(const std::string& name, const std::string& svg) {
    auto path = fixtures::dir() / name;
    std::ofstream(path) << svg;
    return path.string();
}
}

TEST(PathCommands, SchemaCreationAnimationAndEffects) {
    Session session;
    Commands commands(session);
    auto schema = command(commands, {"schema", "clips"});
    ASSERT_TRUE(schema["ok"]) << schema.dump();
    bool path = false;
    for (const auto& clip : schema["result"]) if (clip["kind"] == "path") path = true;
    EXPECT_TRUE(path);
    auto add = command(commands, {"clip.add", "path", "V1", "0", "3", "shape", R"(props={"shape":"ellipse","width":100,"height":80})"});
    ASSERT_TRUE(add["ok"]) << add.dump();
    EXPECT_EQ(add["result"]["kind"], "path");
    EXPECT_EQ(add["result"]["props"]["shape"], "ellipse");
    for (const auto& words : std::vector<std::vector<std::string>>{
             {"prop.key", "shape", "trimEnd", "0", "0"}, {"prop.key", "shape", "trimEnd", "3", "1"},
             {"prop.expr", "shape", "motionProgress", "progress"}, {"effect.add", "shape", "blur"}}) {
        auto reply = command(commands, words);
        EXPECT_TRUE(reply["ok"]) << reply.dump();
    }
}

TEST(PathCommands, ImportsStylesLayerOrderUndoAndPersistence) {
    auto file = svgFile("path-commands.svg", R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"><rect id="back" x="0" y="0" width="100" height="100" fill="#ff0000"/><path id="front" d="M20 20 L80 20 L50 80 Z" fill="#00ff00" stroke="#0000ff" stroke-width="2" stroke-linecap="square" stroke-linejoin="bevel" stroke-miterlimit="7" fill-rule="evenodd"/></svg>)");
    Session session;
    Commands commands(session);
    // Relative paths resolve against the request's working directory.
    auto reply = command(commands, {"path.import", "path-commands.svg", "V2", "1", "4", "logo"});
    ASSERT_TRUE(reply["ok"]) << reply.dump();
    ASSERT_EQ(reply["result"].size(), 2u);
    EXPECT_EQ(reply["result"][0]["name"], "logo 1");
    EXPECT_EQ(reply["result"][1]["name"], "logo 2");
    auto& clips = session.project().clips;
    ASSERT_EQ(clips.size(), 2u);
    EXPECT_LT(clips[0].id, clips[1].id);
    EXPECT_EQ(asString(clips[0].props.at("strokeLinecap").constant), "butt");
    EXPECT_EQ(asString(clips[0].props.at("strokeLinejoin").constant), "miter");
    EXPECT_DOUBLE_EQ(asNumber(clips[0].props.at("strokeMiterLimit").constant), 4);
    EXPECT_EQ(asColor(clips[0].props.at("fill").constant), (Color{1,0,0,1}));
    EXPECT_EQ(asColor(clips[1].props.at("stroke").constant), (Color{0,0,1,1}));
    EXPECT_EQ(asString(clips[1].props.at("fillRule").constant), "evenodd");
    EXPECT_DOUBLE_EQ(asNumber(clips[1].props.at("strokeWidth").constant), 2);
    EXPECT_EQ(asString(clips[1].props.at("strokeLinecap").constant), "square");
    EXPECT_EQ(asString(clips[1].props.at("strokeLinejoin").constant), "bevel");
    EXPECT_DOUBLE_EQ(asNumber(clips[1].props.at("strokeMiterLimit").constant), 7);
    EXPECT_EQ(session.history().undoLabels().size(), 1u);
    EXPECT_TRUE(session.undo());
    EXPECT_TRUE(session.project().clips.empty());
    EXPECT_TRUE(session.redo());
    auto saved = projectToJson(session.project());
    std::filesystem::remove(file);
    auto restored = projectFromJson(saved);
    ASSERT_EQ(restored.clips.size(), 2u);
    EXPECT_EQ(restored.clips[1].props, session.project().clips[1].props);
}

TEST(PathCommands, FailedImportAndBatchAreAtomic) {
    auto file = svgFile("path-atomic.svg", R"(<svg xmlns="http://www.w3.org/2000/svg"><path d="M0 0 L20 20"/></svg>)");
    Session session;
    Commands commands(session);
    for (const auto& words : std::vector<std::vector<std::string>>{
             {"path.import", file, "A1", "0", "2"}, {"path.import", file, "V1", "0", "-1"},
             {"path.import", "not-found.svg", "V1", "0", "2"}}) {
        auto reply = command(commands, words);
        EXPECT_FALSE(reply["ok"]) << reply.dump();
        EXPECT_TRUE(session.project().clips.empty());
        EXPECT_TRUE(session.history().undoLabels().empty());
    }
    json batch = {{"cmd", "batch"}, {"args", {{"commands", json::array({
        {{"cmd", "path.import"}, {"args", {{"path", file}, {"track", "V1"}, {"start", 0}, {"duration", 2}, {"name", "imported"}}}},
        {{"cmd", "prop.set"}, {"args", {{"target", "imported"}, {"name", "noSuchProperty"}, {"value", 3}}}}
    })}}}};
    auto reply = commands.runSync(batch);
    EXPECT_FALSE(reply["ok"]) << reply.dump();
    EXPECT_TRUE(session.project().clips.empty());
    EXPECT_TRUE(session.history().undoLabels().empty());
    batch["args"]["commands"].erase(1);
    reply = commands.runSync(batch);
    ASSERT_TRUE(reply["ok"]) << reply.dump();
    EXPECT_EQ(session.project().clips.size(), 1u);
    EXPECT_EQ(session.history().undoLabels().size(), 1u);
}

TEST(PathCommands, ImportedGeometryValidationIsAtomic) {
    Project project = Project::makeDefault();
    Id track = project.findTrackByName("V1")->id;
    auto before = projectToJson(project);
    ImportedPath valid;
    valid.d = "M0 0 L10 0 L10 10 Z";
    for (const std::string& invalid : {std::string(), std::string("M1 2 C3")}) {
        ImportedPath bad;
        bad.d = invalid;
        EXPECT_THROW(edit::addImportedPaths(project, {valid, bad}, track, 0, 2), EditError);
        EXPECT_EQ(projectToJson(project), before);
    }
}

TEST(PathCommands, ImportedStrokeStylesInheritFromSvgGroups) {
    auto file = svgFile("path-inherited-stroke.svg", R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 40 40"><g stroke="#ffffff" stroke-width="4" stroke-linecap="square" stroke-linejoin="bevel" stroke-miterlimit="9"><path d="M0 0 L40 40"/></g></svg>)");
    Session session;
    Commands commands(session);
    auto reply = command(commands, {"path.import", file, "V1", "0", "2"});
    ASSERT_TRUE(reply["ok"]) << reply.dump();
    ASSERT_EQ(session.project().clips.size(), 1u);
    const auto& props = session.project().clips[0].props;
    EXPECT_EQ(asString(props.at("strokeLinecap").constant), "square");
    EXPECT_EQ(asString(props.at("strokeLinejoin").constant), "bevel");
    EXPECT_DOUBLE_EQ(asNumber(props.at("strokeMiterLimit").constant), 9);
}
