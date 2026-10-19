#include "fixtures.hpp"
#include "media/decoder.hpp"
#include "media/media_cache.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace mf;

namespace {

#define REQUIRE_FFMPEG() \
    if (!fixtures::haveFfmpeg()) GTEST_SKIP() << "ffmpeg CLI not installed"

// Recovers the frame index from the counting video's gray level.
int frameIndexOf(const Image& img) {
    const uint8_t* p = img.at(img.w / 2, img.h / 2);
    double y = 16 + p[1] * 219.0 / 255.0;  // full-range RGB back to limited-range luma
    return int(std::lround((y - 20) / 7));
}

}  // namespace

TEST(Media, ProbesVideoAudioAndImage) {
    REQUIRE_FFMPEG();
    MediaSource v, a, i;
    std::string err;
    ASSERT_TRUE(probeMedia(fixtures::countingVideo(), v, &err)) << err;
    EXPECT_EQ(v.kind, MediaKind::Video);
    EXPECT_EQ(v.width, 64);
    EXPECT_EQ(v.height, 48);
    EXPECT_NEAR(v.fps, 10, 1e-6);
    EXPECT_NEAR(v.duration, 3, 0.15);
    EXPECT_FALSE(v.hasAudio);
    EXPECT_EQ(v.name, "counting.mp4");

    ASSERT_TRUE(probeMedia(fixtures::sineAudio(), a, &err)) << err;
    EXPECT_EQ(a.kind, MediaKind::Audio);
    EXPECT_EQ(a.sampleRate, 44100);
    EXPECT_NEAR(a.duration, 2, 0.05);

    ASSERT_TRUE(probeMedia(fixtures::redImage(), i, &err)) << err;
    EXPECT_EQ(i.kind, MediaKind::Image);
    EXPECT_EQ(i.width, 32);

    MediaSource bad;
    EXPECT_FALSE(probeMedia("/nonexistent/file.mp4", bad, &err));
    EXPECT_NE(err.find("cannot open"), std::string::npos);
}

TEST(Media, VideoDecoderReturnsExactFramesSequentiallyAndAfterSeeks) {
    REQUIRE_FFMPEG();
    VideoDecoder dec;
    std::string err;
    ASSERT_TRUE(dec.open(fixtures::countingVideo(), 1920, 1080, &err)) << err;
    EXPECT_EQ(dec.width(), 64);  // never upscaled
    for (int n = 0; n < 30; ++n) {
        auto img = dec.frameAt(n / 10.0 + 0.05);
        ASSERT_TRUE(img) << n;
        EXPECT_EQ(frameIndexOf(*img), n);
    }
    // Backward and long forward jumps seek.
    for (int n : {25, 3, 17, 0, 29, 12}) {
        auto img = dec.frameAt(n / 10.0 + 0.01);
        ASSERT_TRUE(img) << n;
        EXPECT_EQ(frameIndexOf(*img), n);
    }
    EXPECT_FALSE(dec.frameAt(10.0));
}

TEST(Media, VideoDecoderScalesToFit) {
    REQUIRE_FFMPEG();
    VideoDecoder dec;
    std::string err;
    ASSERT_TRUE(dec.open(fixtures::countingVideo(), 32, 32, &err)) << err;
    EXPECT_EQ(dec.width(), 32);
    EXPECT_EQ(dec.height(), 24);
    auto img = dec.frameAt(0);
    ASSERT_TRUE(img);
    EXPECT_EQ(img->w, 32);
    EXPECT_EQ(img->h, 24);
}

TEST(Media, VideoDecoderStopsAtEndAfterRepeatedReadsAndSeeks) {
    REQUIRE_FFMPEG();
    VideoDecoder dec;
    std::string err;
    ASSERT_TRUE(dec.open(fixtures::countingVideo(), 64, 48, &err)) << err;
    auto last = dec.frameAt(2.95);
    ASSERT_TRUE(last);
    EXPECT_EQ(frameIndexOf(*last), 29);
    for (double t : {3.0, 3.01, 3.2, 10.0}) EXPECT_FALSE(dec.frameAt(t)) << t;

    // An out-of-range read must not prevent later reads within the file.
    auto first = dec.frameAt(0.05);
    ASSERT_TRUE(first);
    EXPECT_EQ(frameIndexOf(*first), 0);
    EXPECT_FALSE(dec.frameAt(10));
    last = dec.frameAt(2.99999);
    ASSERT_TRUE(last);
    EXPECT_EQ(frameIndexOf(*last), 29);
    EXPECT_FALSE(dec.frameAt(3.0));
    EXPECT_FALSE(dec.frameAt(3.01));

    VideoDecoder cold;
    ASSERT_TRUE(cold.open(fixtures::countingVideo(), 64, 48, &err)) << err;
    EXPECT_FALSE(cold.frameAt(3.0));
}

TEST(Media, CacheDoesNotRepeatTheLastFramePastEnd) {
    REQUIRE_FFMPEG();
    MediaSource video;
    std::string err;
    ASSERT_TRUE(probeMedia(fixtures::countingVideo(), video, &err)) << err;
    MediaCache cache;
    ASSERT_TRUE(cache.frame(video, 2.95, 64, 48));
    for (double t : {3.0, 3.05, 3.2, 10.0}) EXPECT_FALSE(cache.frame(video, t, 64, 48)) << t;
    auto first = cache.frame(video, 0.05, 64, 48);
    ASSERT_TRUE(first);
    EXPECT_EQ(frameIndexOf(*first), 0);
}

TEST(Media, DecodesAudioToStereoAtTargetRateWithPeaks) {
    REQUIRE_FFMPEG();
    std::string err;
    auto buf = decodeAudio(fixtures::sineAudio(), 48000, &err);
    ASSERT_TRUE(buf) << err;
    EXPECT_EQ(buf->sampleRate, 48000);
    EXPECT_NEAR(buf->duration(), 2, 0.01);
    float peak = 0;
    for (float s : buf->samples) peak = std::max(peak, std::abs(s));
    EXPECT_NEAR(peak, 0.125, 0.01);  // lavfi sine amplitude is 1/8
    EXPECT_FLOAT_EQ(buf->samples[2000], buf->samples[2001]);  // mono duplicated to both channels
    ASSERT_EQ(buf->peaks.size(), (buf->frames() + 255) / 256 * 2);
    EXPECT_NEAR(buf->peaks[201], 0.125, 0.01);
}

TEST(Media, CacheReusesFramesAndDecodesImagesAndAudio) {
    REQUIRE_FFMPEG();
    MediaCache cache;
    MediaSource v;
    std::string err;
    ASSERT_TRUE(probeMedia(fixtures::countingVideo(), v, &err)) << err;
    auto f1 = cache.frame(v, 1.23, 64, 48);
    ASSERT_TRUE(f1);
    EXPECT_EQ(frameIndexOf(*f1), 12);
    EXPECT_EQ(cache.frame(v, 1.27, 64, 48), f1);  // same frame index, cached
    EXPECT_EQ(frameIndexOf(*cache.frame(v, 0.4, 64, 48)), 4);
    EXPECT_GT(cache.frameBytes(), 0u);

    MediaSource img;
    ASSERT_TRUE(probeMedia(fixtures::redImage(), img, &err)) << err;
    auto red = cache.frame(img, 7.0, 100, 100);
    ASSERT_TRUE(red);
    EXPECT_GE(red->at(0, 0)[0], 250);
    EXPECT_LE(red->at(0, 0)[1], 5);
    EXPECT_EQ(red->at(0, 0)[3], 255);

    MediaSource av;
    ASSERT_TRUE(probeMedia(fixtures::videoWithAudio(), av, &err)) << err;
    EXPECT_TRUE(av.hasVideo && av.hasAudio);
    auto audio = cache.audio(av, 48000, true);
    ASSERT_TRUE(audio);
    EXPECT_NEAR(audio->duration(), 1, 0.06);

    MediaSource missing{.path = "/nonexistent.mp4", .hasVideo = true};
    EXPECT_FALSE(cache.frame(missing, 0, 64, 64));
    EXPECT_FALSE(cache.error("/nonexistent.mp4").empty());
}

TEST(Media, CacheEvictsOverBudget) {
    REQUIRE_FFMPEG();
    MediaCache cache(64 * 48 * 4 * 3);
    MediaSource v;
    std::string err;
    ASSERT_TRUE(probeMedia(fixtures::countingVideo(), v, &err)) << err;
    for (int n = 0; n < 10; ++n) ASSERT_TRUE(cache.frame(v, n / 10.0, 64, 48));
    EXPECT_LE(cache.frameBytes(), size_t(64 * 48 * 4 * 3));
}

TEST(Media, CacheSharesEquivalentOutputSizesWithoutRoundingTwice) {
    REQUIRE_FFMPEG();
    MediaSource media;
    std::string error;
    ASSERT_TRUE(probeMedia(fixtures::countingVideo(), media, &error));
    MediaCache cache;
    auto original = cache.frame(media, .5, 64, 48);
    ASSERT_TRUE(original);
    EXPECT_EQ(cache.frame(media, .5, 4096, 4096), original);
    EXPECT_EQ(cache.frame(media, .5, 64, 300), original);
    auto small = cache.frame(media, .5, 32, 100);
    ASSERT_TRUE(small);
    EXPECT_EQ(cache.frame(media, .5, 200, 24), small);
    EXPECT_EQ(cache.frameBytes(), original->bytes() + small->bytes());
    // Even rounding is performed once, not again using the rounded result.
    EXPECT_EQ(VideoDecoder::outputSize(641, 359, 500, 500), (std::pair{500, 280}));
}
