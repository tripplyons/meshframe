#include "audio/mixer.hpp"
#include "engine/player.hpp"
#include "fixtures.hpp"
#include "media/decoder.hpp"
#include "model/edit.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

using namespace mf;

namespace {

constexpr int kRate = 48000;

Project audioProject(const char* script) {
    Project p = Project::makeDefault();
    p.script = script;
    return p;
}

Id addSynth(Project& p, const char* track, double start, double dur, const char* synth) {
    Clip c;
    c.kind = ClipKind::Synth;
    c.track = p.findTrackByName(track)->id;
    c.start = start;
    c.duration = dur;
    Id id = edit::addClip(p, c);
    edit::setPropConstant(p, id, "synth", std::string(synth));
    return id;
}

struct Mix {
    MediaCache cache;
    AudioMixer mixer{cache};
    Evaluator ev;
    std::vector<float> render(const Project& p, double t0, int frames) {
        ev.setProject(p);
        mixer.reset();
        std::vector<float> out(size_t(frames) * 2);
        mixer.render(ev, t0, frames, out.data(), true);
        return out;
    }
};

float peakOf(const std::vector<float>& s, size_t from = 0) {
    float m = 0;
    for (size_t i = from; i < s.size(); ++i) m = std::max(m, std::abs(s[i]));
    return m;
}

const char* kSynths = R"(
mf.synth("dc", function(t) return 0.5 end)
mf.synth("hi", function(t) return 0.5 * sin(2 * pi * 8000 * t) end)
mf.synth("lo", function(t) return 0.5 * sin(2 * pi * 60 * t) end)
mf.synth("click", function(t) if t < 0.001 then return 1 end return 0 end)
)";

}  // namespace

TEST(Audio, SynthClipsGainPanAndTimeRange) {
    Project p = audioProject(kSynths);
    Id id = addSynth(p, "A1", 0.5, 1.0, "dc");
    Mix m;
    auto out = m.render(p, 0.4, 9600);  // 0.4 .. 0.6
    EXPECT_FLOAT_EQ(out[0], 0.0f);      // before the clip
    EXPECT_FLOAT_EQ(out[2 * 4800 + 0], 0.5f);
    EXPECT_FLOAT_EQ(out[2 * 9000 + 1], 0.5f);
    // Clip start lands on frame 4800 exactly.
    EXPECT_FLOAT_EQ(out[2 * 4799], 0.0f);

    edit::setPropConstant(p, id, "gain", -6.0);
    edit::setPropConstant(p, id, "pan", 1.0);
    out = m.render(p, 1.0, 256);
    EXPECT_FLOAT_EQ(out[0], 0.0f);
    EXPECT_NEAR(out[1], 0.5 * std::pow(10, -6.0 / 20), 1e-5);

    // Muted tracks are silent; two tracks sum.
    addSynth(p, "A2", 0, 2, "dc");
    out = m.render(p, 1.0, 256);
    EXPECT_NEAR(out[0], 0.5, 1e-5);
    edit::setTrack(p, p.findTrackByName("A2")->id, edit::TrackFlags{.muted = true});
    out = m.render(p, 1.0, 256);
    EXPECT_FLOAT_EQ(out[0], 0.0f);
}

TEST(Audio, FiltersShapeFrequencies) {
    Project p = audioProject(kSynths);
    Id hi = addSynth(p, "A1", 0, 2, "hi");
    Id fx = edit::addEffect(p, hi, true, "lowpass");
    edit::setPropConstant(p, fx, "cutoff", 500.0);
    Mix m;
    EXPECT_LT(peakOf(m.render(p, 0, 9600), 4000), 0.01f);

    Project q = audioProject(kSynths);
    Id lo = addSynth(q, "A1", 0, 2, "lo");
    Id hp = edit::addEffect(q, lo, true, "highpass");
    edit::setPropConstant(q, hp, "cutoff", 2000.0);
    EXPECT_LT(peakOf(m.render(q, 0, 9600), 4000), 0.01f);
    edit::setPropConstant(q, hp, "cutoff", 20.0);
    EXPECT_GT(peakOf(m.render(q, 0, 9600), 4000), 0.4f);
}

TEST(Audio, DelayEchoesAndCompressorReducesPeaks) {
    Project p = audioProject(kSynths);
    Id click = addSynth(p, "A1", 0, 2, "click");
    Id d = edit::addEffect(p, click, true, "delay");
    edit::setPropConstant(p, d, "time", 0.1);
    edit::setPropConstant(p, d, "mix", 0.5);
    edit::setPropConstant(p, d, "feedback", 0.0);
    Mix m;
    auto out = m.render(p, 0, 9600);
    EXPECT_NEAR(out[2 * 4800], 0.5f, 1e-5);  // echo of the first sample, 0.1 s later
    EXPECT_NEAR(out[2 * 4700], 0.0f, 1e-5);

    Project q = audioProject(kSynths);
    Id dc = addSynth(q, "A1", 0, 2, "dc");
    Id comp = edit::addEffect(q, dc, true, "compressor");
    edit::setPropConstant(q, comp, "threshold", -20.0);
    edit::setPropConstant(q, comp, "ratio", 10.0);
    out = m.render(q, 0, 48000);
    // 0.5 is -6 dB: 14 dB over, reduced by 12.6 dB once attack settles.
    EXPECT_NEAR(gainToDb(std::abs(out[2 * 47000])), -6.02 - 14 * 0.9, 0.3);
}

TEST(Audio, TransitionsCrossfadeAndMasterChainApplies) {
    Project p = audioProject(kSynths);
    Id a = addSynth(p, "A1", 0, 2, "dc");
    Id b = addSynth(p, "A1", 2, 2, "dc");
    Id tr = edit::addTransition(p, a, b, "crossfade", 1.0);
    edit::setTransitionCurve(p, tr, Curve{{Keyframe{0, 0, Interp::Linear}, Keyframe{1, 1, Interp::Linear}}});
    edit::setPropConstant(p, b, "pan", 1.0);  // B only on the right
    Mix m;
    auto out = m.render(p, 1.75, 256);  // mix 0.25: left = A * 0.75, right = A * 0.75 + B * 0.25
    EXPECT_NEAR(out[0], 0.375, 1e-3);
    EXPECT_NEAR(out[1], 0.5, 1e-3);

    edit::addEffect(p, 0, true, "gain");
    edit::setPropConstant(p, p.masterAudio[0].id, "gain", -6.0206);
    out = m.render(p, 3.0, 256);
    EXPECT_NEAR(out[1], 0.25, 1e-3);
    EXPECT_NEAR(m.mixer.levels().peak[1], 0.25, 1e-3);
    EXPECT_NEAR(m.mixer.levels().peak[0], 0.0, 1e-6);
}

TEST(Audio, MediaClipPlaysDecodedSamplesAtSourceOffset) {
    if (!fixtures::haveFfmpeg()) GTEST_SKIP() << "ffmpeg CLI not installed";
    Project p = audioProject("");
    MediaSource ms;
    std::string err;
    ASSERT_TRUE(probeMedia(fixtures::sineAudio(), ms, &err)) << err;
    Id media = edit::addMedia(p, ms);
    auto ids = edit::addMediaClips(p, media, 1.0);
    ASSERT_EQ(ids.size(), 1u);
    p.findClip(ids[0])->in = 0.5;
    Mix m;
    auto out = m.render(p, 1.25, 4800);
    auto buf = decodeAudio(ms.path, kRate, &err);
    ASSERT_TRUE(buf);
    // Timeline 1.25 is source 0.75.
    for (int i : {0, 100, 4000}) EXPECT_NEAR(out[i * 2], buf->samples[(36000 + i) * 2], 1e-5);
}

namespace {

class FakeSink : public AudioSink {
public:
    std::vector<float> played;
    std::vector<float> queue_;
    bool paused = true;
    void queue(const float* s, int frames) override { queue_.insert(queue_.end(), s, s + frames * 2); }
    int queuedFrames() const override { return int(queue_.size() / 2); }
    void clear() override { queue_.clear(); }
    void setPaused(bool p) override { paused = p; }
    // The device consumes frames.
    void consume(int frames) {
        frames = std::min(frames, queuedFrames());
        played.insert(played.end(), queue_.begin(), queue_.begin() + frames * 2);
        queue_.erase(queue_.begin(), queue_.begin() + frames * 2);
    }
};

}  // namespace

TEST(Audio, PlayerClockFollowsTheDeviceAndStopsAtTheEnd) {
    Project p = audioProject(kSynths);
    addSynth(p, "A1", 0, 1, "dc");
    MediaCache cache;
    AudioMixer mixer(cache);
    Evaluator ev;
    ev.setProject(p);
    FakeSink sink;
    Player player(mixer, &sink);
    player.seek(0.5);
    player.play(ev);
    EXPECT_FALSE(sink.paused);
    EXPECT_GE(sink.queuedFrames(), int(Player::kLookahead * kRate));
    EXPECT_DOUBLE_EQ(player.time(), 0.5);
    // The device plays 512 frames at a time while the UI loop keeps it topped up.
    auto advance = [&](int frames) {
        double t = 0;
        for (int done = 0; done < frames; done += 512) {
            sink.consume(512);
            t = player.update(ev);
        }
        return t;
    };
    EXPECT_NEAR(advance(4608), 0.596, 1e-9);
    EXPECT_FLOAT_EQ(sink.played[0], 0.5f);

    player.pause();
    EXPECT_TRUE(sink.paused);
    EXPECT_NEAR(player.time(), 0.596, 1e-9);
    player.seek(0.9);
    player.play(ev);
    EXPECT_NEAR(advance(9728), 1.0, 1e-9);  // 0.2 s later is past the 1.0 s end
    EXPECT_FALSE(player.playing());
    EXPECT_DOUBLE_EQ(player.time(), 1.0);
}

TEST(Audio, PlayerWithoutDeviceUsesTheClock) {
    Project p = audioProject("");
    addSynth(p, "A1", 0, 5, "dc");
    MediaCache cache;
    AudioMixer mixer(cache);
    Evaluator ev;
    ev.setProject(p);
    double now = 100;
    Player player(mixer, nullptr, [&] { return now; });
    player.play(ev);
    now = 101.5;
    EXPECT_DOUBLE_EQ(player.update(ev), 1.5);
    player.seek(4.0);
    now = 102.0;
    EXPECT_DOUBLE_EQ(player.time(), 4.5);
}
