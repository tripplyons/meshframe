#pragma once

#include "audio/audio_sink.hpp"
#include "audio/mixer.hpp"

#include <chrono>
#include <functional>

namespace mf {

// Transport: play, pause, and seek. While playing, audio is rendered a little
// ahead into the sink and the sink's play position is the clock. Without a
// sink, the wall clock drives time and no audio is produced.
class Player {
public:
    using Clock = std::function<double()>;  // seconds; injectable for tests

    Player(AudioMixer& mixer, AudioSink* sink, Clock clock = {});

    void play(Evaluator& ev);
    void pause();
    void seek(double t);
    bool playing() const { return playing_; }
    double time() const;

    // Call once per UI frame: tops up the sink and stops at the project end.
    // Returns the playhead.
    double update(Evaluator& ev);

    void setLoop(bool loop) { loop_ = loop; }
    bool loop() const { return loop_; }
    const AudioLevels& levels() const { return mixer_.levels(); }

    static constexpr double kLookahead = 0.08;  // seconds of audio kept queued
    static constexpr int kChunk = 1024;

private:
    void fill(Evaluator& ev);

    AudioMixer& mixer_;
    AudioSink* sink_;
    Clock clock_;
    bool playing_ = false;
    bool loop_ = false;
    double origin_ = 0;      // timeline time where playback (re)started
    double wallStart_ = 0;   // clock value at start, when there is no sink
    int64_t queuedTotal_ = 0;  // frames pushed since start
    double renderPos_ = 0;   // timeline time of the next frame to render
    double paused_ = 0;      // playhead while paused
    int sampleRate_ = 48000;
    std::vector<float> chunk_;
};

}  // namespace mf
