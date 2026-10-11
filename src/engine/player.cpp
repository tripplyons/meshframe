#include "engine/player.hpp"

#include <algorithm>

namespace mf {

Player::Player(AudioMixer& mixer, AudioSink* sink, Clock clock) : mixer_(mixer), sink_(sink), clock_(std::move(clock)) {
    if (!clock_) {
        clock_ = [] {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        };
    }
}

double Player::time() const {
    if (!playing_) return paused_;
    if (sink_) return origin_ + double(queuedTotal_ - sink_->queuedFrames()) / sampleRate_;
    return origin_ + (clock_() - wallStart_);
}

void Player::play(Evaluator& ev) {
    if (playing_) return;
    double end = ev.project().duration();
    if (end > 0 && paused_ >= end - 1e-6) paused_ = 0;  // replay from the start
    sampleRate_ = ev.project().settings.sampleRate;
    origin_ = renderPos_ = paused_;
    queuedTotal_ = 0;
    wallStart_ = clock_();
    mixer_.reset();
    playing_ = true;
    if (sink_) {
        sink_->clear();
        fill(ev);
        sink_->setPaused(false);
    }
}

void Player::pause() {
    if (!playing_) return;
    paused_ = time();
    playing_ = false;
    if (sink_) {
        sink_->setPaused(true);
        sink_->clear();
    }
}

void Player::seek(double t) {
    t = std::max(0.0, t);
    paused_ = t;
    if (!playing_) return;
    origin_ = renderPos_ = t;
    queuedTotal_ = 0;
    wallStart_ = clock_();
    mixer_.reset();
    if (sink_) sink_->clear();
}

void Player::fill(Evaluator& ev) {
    int target = int(kLookahead * sampleRate_);
    chunk_.resize(size_t(kChunk) * 2);
    while (sink_->queuedFrames() < target) {
        mixer_.render(ev, renderPos_, kChunk, chunk_.data());
        sink_->queue(chunk_.data(), kChunk);
        queuedTotal_ += kChunk;
        renderPos_ += double(kChunk) / sampleRate_;
    }
}

double Player::update(Evaluator& ev) {
    if (!playing_) return paused_;
    double end = ev.project().duration();
    double t = time();
    if (end > 0 && t >= end) {
        if (loop_) {
            seek(0);
            t = 0;
        } else {
            pause();
            paused_ = end;
            return end;
        }
    }
    if (sink_) fill(ev);
    return t;
}

}  // namespace mf
