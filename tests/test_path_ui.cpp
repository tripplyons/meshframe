#include "shell_harness.hpp"

#include <gtest/gtest.h>
#include <fstream>

using namespace mf;

TEST(PathUI, ShapeActionUsesSelectedTrackAndPlayhead) {
    ShellHarness harness;
    harness.session.selection.track = harness.session.project().findTrackByName("V2")->id;
    ASSERT_TRUE(harness.run({"seek", "1.5"})["ok"]);
    auto reply = harness.run({"ui.action", "edit.add-path"});
    ASSERT_TRUE(reply["ok"]) << reply.dump();
    ASSERT_EQ(harness.session.project().clips.size(), 1u);
    const auto& clip = harness.session.project().clips[0];
    EXPECT_EQ(clip.kind, ClipKind::Path);
    EXPECT_EQ(clip.track, harness.session.selection.track);
    EXPECT_DOUBLE_EQ(clip.start, 1.5);
    EXPECT_EQ(harness.session.selection.primaryClip(), clip.id);
    harness.frame();
    EXPECT_TRUE(harness.session.undo());
    EXPECT_TRUE(harness.session.project().clips.empty());
}

TEST(PathUI, ImportActionEmbedsPathsAndSelectsThem) {
    ShellHarness harness;
    auto file = fixtures::dir() / "path-ui.svg";
    std::ofstream(file) << R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"><circle cx="50" cy="50" r="40"/><path d="M0 0 L100 100"/></svg>)";
    harness.session.selection.track = harness.session.project().findTrackByName("V1")->id;
    ASSERT_TRUE(harness.run({"seek", "2"})["ok"]);
    bool dialog = false;
    harness.shell->hooks.fileDialog = [&](bool save, const std::string& title, const std::vector<std::string>& extensions,
                                           std::function<void(std::string)> done) {
        dialog = true;
        EXPECT_FALSE(save);
        EXPECT_EQ(title, "Import paths");
        EXPECT_EQ(extensions, (std::vector<std::string>{"svg", "path", "txt"}));
        done(file.string());
    };
    auto reply = harness.run({"ui.action", "file.import-paths"});
    ASSERT_TRUE(reply["ok"]) << reply.dump();
    EXPECT_TRUE(dialog);
    ASSERT_EQ(harness.session.project().clips.size(), 2u);
    EXPECT_EQ(harness.session.selection.clips.size(), 2u);
    for (const auto& clip : harness.session.project().clips) {
        EXPECT_EQ(clip.kind, ClipKind::Path);
        EXPECT_DOUBLE_EQ(clip.start, 2);
        EXPECT_DOUBLE_EQ(clip.duration, 3);
    }
    EXPECT_EQ(harness.session.history().undoLabels().size(), 1u);
    harness.frame();
    EXPECT_TRUE(harness.session.undo());
    EXPECT_TRUE(harness.session.project().clips.empty());
}

TEST(PathUI, CancelAndLockedTrackPreserveProject) {
    ShellHarness harness;
    harness.shell->hooks.fileDialog = [](bool, const std::string&, const std::vector<std::string>&,
                                           std::function<void(std::string)> done) { done(""); };
    EXPECT_TRUE(harness.run({"ui.action", "file.import-paths"})["ok"]);
    EXPECT_TRUE(harness.session.project().clips.empty());
    EXPECT_TRUE(harness.session.history().undoLabels().empty());
    auto file = fixtures::dir() / "path-ui-locked.path";
    std::ofstream(file) << "M0 0 L20 20";
    harness.session.selection.track = harness.session.project().findTrackByName("V1")->id;
    ASSERT_TRUE(harness.run({"track.set", "V1", "locked=true"})["ok"]);
    harness.shell->hooks.fileDialog = [&](bool, const std::string&, const std::vector<std::string>&,
                                           std::function<void(std::string)> done) { done(file.string()); };
    EXPECT_TRUE(harness.run({"ui.action", "file.import-paths"})["ok"]);
    EXPECT_TRUE(harness.session.project().clips.empty());
    EXPECT_EQ(harness.session.history().undoLabels().size(), 1u);
}
