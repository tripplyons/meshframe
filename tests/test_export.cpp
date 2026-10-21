#include "exporter/exporter.hpp"
#include "fixtures.hpp"
#include "media/decoder.hpp"
#include "model/edit.hpp"

#include <gtest/gtest.h>

extern "C" {
#include <libavformat/avformat.h>
}

#include <filesystem>
#include <numbers>
#include <chrono>

using namespace mf;

namespace {

Project exportProjectFixture() {
    Project p = Project::makeDefault();
    p.script = R"(mf.synth("tone", function(t) return 0.25 * sin(2 * pi * 440 * t) end))";
    p.settings.width = 64;
    p.settings.height = 32;
    p.settings.fps = 25;
    Clip solid;
    solid.kind = ClipKind::Solid;
    solid.track = p.findTrackByName("V1")->id;
    solid.duration = 1;
    Id s = edit::addClip(p, solid);
    edit::setPropConstant(p, s, "color", Color{1, 0, 0, 1});
    Clip synth;
    synth.kind = ClipKind::Synth;
    synth.track = p.findTrackByName("A1")->id;
    synth.duration = 1;
    Id a = edit::addClip(p, synth);
    edit::setPropConstant(p, a, "synth", std::string("tone"));
    return p;
}

std::string outPath(const char* name) { return (fixtures::dir() / name).string(); }

}  // namespace

TEST(Export, Mp4HasExpectedStreamsDurationAndPixels) {
    Project p = exportProjectFixture();
    ExportOptions o;
    o.path = outPath("export.mp4");
    ExportProgress prog;
    std::string err;
    ASSERT_TRUE(exportProject(p, o, &prog, &err)) << err;
    EXPECT_DOUBLE_EQ(prog.fraction, 1.0);

    MediaSource m;
    ASSERT_TRUE(probeMedia(o.path, m, &err)) << err;
    EXPECT_TRUE(m.hasVideo);
    EXPECT_TRUE(m.hasAudio);
    EXPECT_EQ(m.width, 64);
    EXPECT_EQ(m.height, 32);
    EXPECT_NEAR(m.fps, 25, 1e-6);
    EXPECT_NEAR(m.duration, 1.0, 0.1);
    EXPECT_EQ(m.sampleRate, 48000);

    VideoDecoder dec;
    ASSERT_TRUE(dec.open(o.path, 64, 32, &err)) << err;
    auto frame = dec.frameAt(0.5);
    ASSERT_TRUE(frame);
    const uint8_t* px = frame->at(32, 16);
    EXPECT_NEAR(px[0], 255, 8);
    EXPECT_NEAR(px[1], 0, 8);
    EXPECT_NEAR(px[2], 0, 8);

    auto audio = decodeAudio(o.path, 48000, &err);
    ASSERT_TRUE(audio) << err;
    float peak = 0;
    for (size_t i = 4800; i < audio->samples.size() - 4800; ++i) peak = std::max(peak, std::abs(audio->samples[i]));
    EXPECT_NEAR(peak, 0.25, 0.03);
}

TEST(Export, Mp4HasChaptersFromTitledMarkers) {
    Project p = exportProjectFixture();
    edit::addMarker(p, "intro", 0, "Intro");
    edit::addMarker(p, "plain", 0.3);
    edit::addMarker(p, "middle", 0.5, "The middle");
    ExportOptions o;
    o.path = outPath("export-chapters.mp4");
    o.start = 0.25;  // the export starts inside the first chapter
    std::string err;
    ASSERT_TRUE(exportProject(p, o, nullptr, &err)) << err;

    AVFormatContext* fc = nullptr;
    ASSERT_EQ(avformat_open_input(&fc, o.path.c_str(), nullptr, nullptr), 0);
    ASSERT_EQ(fc->nb_chapters, 2u);
    auto at = [&](unsigned i, int64_t ts) { return double(ts) * av_q2d(fc->chapters[i]->time_base); };
    EXPECT_STREQ(av_dict_get(fc->chapters[0]->metadata, "title", nullptr, 0)->value, "Intro");
    EXPECT_NEAR(at(0, fc->chapters[0]->start), 0, 1e-3);
    EXPECT_STREQ(av_dict_get(fc->chapters[1]->metadata, "title", nullptr, 0)->value, "The middle");
    EXPECT_NEAR(at(1, fc->chapters[1]->start), 0.25, 1e-3);
    avformat_close_input(&fc);

    // The chapter track doesn't get in the way of reading the video back.
    MediaSource m;
    ASSERT_TRUE(probeMedia(o.path, m, &err)) << err;
    EXPECT_TRUE(m.hasVideo);
    EXPECT_TRUE(m.hasAudio);
    VideoDecoder dec;
    ASSERT_TRUE(dec.open(o.path, 64, 32, &err)) << err;
    ASSERT_TRUE(dec.frameAt(0.5));
}

// VideoToolbox stops at 4096 pixels wide, so wider exports fall back to x264.
TEST(Export, Mp4WiderThanTheHardwareEncoderFallsBack) {
    Project p = exportProjectFixture();
    ExportOptions o;
    o.path = outPath("export-wide.mp4");
    o.width = 4608;
    o.height = 32;
    o.end = 0.2;
    std::string err;
    ASSERT_TRUE(exportProject(p, o, nullptr, &err)) << err;

    VideoDecoder dec;
    ASSERT_TRUE(dec.open(o.path, 4608, 32, &err)) << err;
    auto frame = dec.frameAt(0.1);
    ASSERT_TRUE(frame);
    const uint8_t* px = frame->at(4500, 16);
    EXPECT_NEAR(px[0], 255, 8);
    EXPECT_NEAR(px[1], 0, 8);
    EXPECT_NEAR(px[2], 0, 8);
}

TEST(Export, WavHasExactLengthAndSamples) {
    Project p = exportProjectFixture();
    ExportOptions o;
    o.path = outPath("export.wav");
    o.start = 0.25;
    o.end = 0.75;
    std::string err;
    ASSERT_TRUE(exportProject(p, o, nullptr, &err)) << err;
    auto audio = decodeAudio(o.path, 48000, &err);
    ASSERT_TRUE(audio) << err;
    EXPECT_EQ(audio->frames(), 24000u);
    // Sample 100 is t = 0.25 + 100 / 48000.
    double t = 0.25 + 100.0 / 48000;
    EXPECT_NEAR(audio->samples[200], 0.25 * std::sin(2 * std::numbers::pi * 440 * t), 1e-4);
}

TEST(Export, CancelAndBadPathsLeaveNoFile) {
    Project p = exportProjectFixture();
    ExportOptions o;
    o.path = outPath("canceled.mp4");
    std::filesystem::remove(o.path);
    ExportProgress prog;
    prog.cancel = true;
    std::string err;
    EXPECT_FALSE(exportProject(p, o, &prog, &err));
    EXPECT_EQ(err, "export canceled");
    EXPECT_FALSE(std::filesystem::exists(o.path));

    o.path = outPath("bad.mov");
    EXPECT_FALSE(exportProject(p, o, nullptr, &err));
    EXPECT_NE(err.find(".mp4 or .wav"), std::string::npos);
}

TEST(Export, BackgroundJobReportsProgressAndResult) {
    Project p = exportProjectFixture();
    ExportJob job;
    ExportOptions o;
    o.path = outPath("job.wav");
    std::string err;
    ASSERT_TRUE(job.start(p, o, &err)) << err;
    job.wait();
    EXPECT_TRUE(job.finished());
    EXPECT_FALSE(job.running());
    EXPECT_TRUE(job.error().empty()) << job.error();
    EXPECT_DOUBLE_EQ(job.progress(), 1.0);
    EXPECT_TRUE(std::filesystem::exists(o.path));
}

TEST(Export, CancelWhileEncodingJoinsTheWriterAndRemovesOutput) {
    Project p = exportProjectFixture();
    p.settings.width = 1920;
    p.settings.height = 1080;
    for (auto& clip : p.clips) clip.duration = 30;
    ExportOptions options;
    options.path = outPath("cancel-active.mp4");
    ExportJob job;
    std::string error;
    ASSERT_TRUE(job.start(p, options, &error)) << error;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (job.running() && job.progress() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    job.cancel();
    job.wait();
    EXPECT_FALSE(job.running());
    EXPECT_EQ(job.error(), "export canceled");
    EXPECT_FALSE(std::filesystem::exists(options.path));
}
