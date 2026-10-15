#include "engine/frame_renderer.hpp"
#include "exporter/exporter.hpp"
#include "fixtures.hpp"
#include "media/decoder.hpp"
#include "model/serialize.hpp"
#include "session/commands.hpp"

#include <gtest/gtest.h>

#include <fstream>

using namespace mf;

TEST(PathExport, ImportedAnimatedEffectSurvivesReloadAndMatchesPreview) {
    auto source = fixtures::dir() / "path-export.svg";
    std::ofstream(source) << R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 32"><rect x="26" y="10" width="12" height="12" fill="#ff0000"/></svg>)";
    Session session;
    Commands commands(session);
    auto run = [&](std::vector<std::string> words) {
        return commands.runSync(Commands::requestFromWords(words, fixtures::dir().string()));
    };
    for (const auto& request : std::vector<std::vector<std::string>>{
             {"settings", "width=64", "height=32", "fps=10"},
             {"path.import", source.string(), "V1", "0", "1", "box"},
             {"prop.set", "box", "motionPath", "M -16 0 L 16 0"},
             {"prop.key", "box", "motionProgress", "0", "0"},
             {"prop.key", "box", "motionProgress", "1", "1"},
             {"effect.add", "box", "invert"}}) {
        auto reply = run(request);
        ASSERT_TRUE(reply["ok"]) << reply.dump();
    }
    auto projectFile = (fixtures::dir() / "path-export.mfproj").string();
    saveProject(session.project(), projectFile);
    std::filesystem::remove(source);
    Project project = loadProject(projectFile);
    ExportOptions options;
    options.path = (fixtures::dir() / "path-animation-export.mp4").string();
    options.crf = 0;
    std::string error;
    ASSERT_TRUE(exportProject(project, options, nullptr, &error)) << error;

    VideoDecoder decoder;
    ASSERT_TRUE(decoder.open(options.path, 64, 32, &error)) << error;
    MediaCache cache;
    FrameRenderer renderer(cache);
    Evaluator evaluator;
    evaluator.setProject(project);
    for (double time : {0.0, 0.5, 0.9}) {
        auto preview = renderer.render(evaluator, time, 64, 32);
        auto exported = decoder.frameAt(time);
        ASSERT_TRUE(exported);
        double difference = 0;
        for (size_t i = 0; i < preview->px.size(); i += 4)
            for (size_t channel = 0; channel < 3; ++channel)
                difference += std::abs(int(preview->px[i + channel]) - int(exported->px[i + channel]));
        EXPECT_LT(difference / (64 * 32 * 3), 5) << "at " << time;
        int center = int(16 + 32 * time);
        EXPECT_GT(exported->at(center, 16)[1], 230) << "at " << time;
        EXPECT_GT(exported->at(center, 16)[2], 230) << "at " << time;
    }
    EXPECT_TRUE(evaluator.errors().empty());
}
