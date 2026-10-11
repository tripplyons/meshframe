#include "audio/dsp.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mf {

double dbToGain(double db) { return db <= -96 ? 0.0 : std::pow(10.0, db / 20); }
double gainToDb(double gain) { return gain <= 1e-5 ? -100.0 : 20 * std::log10(gain); }

namespace {

double num(const ParamFn& p, std::string_view name, double fallback) { return asNumber(p(name), fallback); }

class GainFx : public AudioEffect {
public:
    void process(float* s, int frames, const ParamFn& p) override {
        float g = float(dbToGain(num(p, "gain", 0)));
        for (int i = 0; i < frames * 2; ++i) s[i] *= g;
    }
};

class PanFx : public AudioEffect {
public:
    void process(float* s, int frames, const ParamFn& p) override {
        float l, r;
        panGains(std::clamp(num(p, "pan", 0), -1.0, 1.0), l, r);
        for (int i = 0; i < frames; ++i) {
            s[i * 2] *= l;
            s[i * 2 + 1] *= r;
        }
    }
};

// RBJ cookbook biquad, transposed direct form II, one state per channel.
class BiquadFx : public AudioEffect {
public:
    BiquadFx(bool highpass, int sr) : highpass_(highpass), sr_(sr) {}
    void process(float* s, int frames, const ParamFn& p) override {
        double cutoff = std::clamp(num(p, "cutoff", highpass_ ? 200 : 2000), 10.0, sr_ * 0.45);
        double q = std::max(0.05, num(p, "q", 0.707));
        if (cutoff != cutoff_ || q != q_) design(cutoff, q);
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < 2; ++c) {
                double x = s[i * 2 + c];
                double y = b0_ * x + z1_[c];
                z1_[c] = b1_ * x - a1_ * y + z2_[c];
                z2_[c] = b2_ * x - a2_ * y;
                s[i * 2 + c] = float(y);
            }
    }
    void reset() override { z1_[0] = z1_[1] = z2_[0] = z2_[1] = 0; }

private:
    void design(double f, double q) {
        cutoff_ = f;
        q_ = q;
        double w = 2 * std::numbers::pi * f / sr_, cw = std::cos(w), alpha = std::sin(w) / (2 * q);
        double a0 = 1 + alpha;
        if (highpass_) {
            b0_ = (1 + cw) / 2 / a0;
            b1_ = -(1 + cw) / a0;
        } else {
            b0_ = (1 - cw) / 2 / a0;
            b1_ = (1 - cw) / a0;
        }
        b2_ = b0_;
        a1_ = -2 * cw / a0;
        a2_ = (1 - alpha) / a0;
    }
    bool highpass_;
    double sr_;
    double cutoff_ = -1, q_ = -1;
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    double z1_[2] = {}, z2_[2] = {};
};

class DelayFx : public AudioEffect {
public:
    explicit DelayFx(int sr) : sr_(sr), buf_(size_t(sr) * 2 * 2 + 2, 0.0f) {}
    void process(float* s, int frames, const ParamFn& p) override {
        int len = int(buf_.size() / 2);
        int d = std::clamp(int(num(p, "time", 0.25) * sr_), 1, len - 1);
        float fb = float(std::clamp(num(p, "feedback", 0.35), 0.0, 0.95));
        float mix = float(std::clamp(num(p, "mix", 0.3), 0.0, 1.0));
        for (int i = 0; i < frames; ++i) {
            int rd = (pos_ - d + len) % len;
            for (int c = 0; c < 2; ++c) {
                float dry = s[i * 2 + c], wet = buf_[rd * 2 + c];
                buf_[pos_ * 2 + c] = dry + wet * fb;
                s[i * 2 + c] = dry + (wet - dry) * mix;
            }
            pos_ = (pos_ + 1) % len;
        }
    }
    void reset() override {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        pos_ = 0;
    }

private:
    int sr_;
    std::vector<float> buf_;
    int pos_ = 0;
};

// Stereo-linked peak compressor with smoothed gain reduction.
class CompressorFx : public AudioEffect {
public:
    explicit CompressorFx(int sr) : sr_(sr) {}
    void process(float* s, int frames, const ParamFn& p) override {
        double threshold = num(p, "threshold", -18), ratio = std::max(1.0, num(p, "ratio", 4));
        double attack = std::exp(-1.0 / (std::max(1e-4, num(p, "attack", 0.01)) * sr_));
        double release = std::exp(-1.0 / (std::max(1e-3, num(p, "release", 0.15)) * sr_));
        double makeup = dbToGain(num(p, "makeup", 0));
        for (int i = 0; i < frames; ++i) {
            double level = gainToDb(std::max(std::abs(s[i * 2]), std::abs(s[i * 2 + 1])));
            double over = level - threshold;
            double target = over > 0 ? over * (1 - 1 / ratio) : 0;  // dB of reduction
            double coeff = target > reductionDb_ ? attack : release;
            reductionDb_ = target + (reductionDb_ - target) * coeff;
            float g = float(dbToGain(-reductionDb_) * makeup);
            s[i * 2] *= g;
            s[i * 2 + 1] *= g;
        }
    }
    void reset() override { reductionDb_ = 0; }

private:
    double sr_;
    double reductionDb_ = 0;
};

}  // namespace

std::unique_ptr<AudioEffect> makeAudioEffect(std::string_view type, int sr) {
    if (type == "gain") return std::make_unique<GainFx>();
    if (type == "pan") return std::make_unique<PanFx>();
    if (type == "lowpass") return std::make_unique<BiquadFx>(false, sr);
    if (type == "highpass") return std::make_unique<BiquadFx>(true, sr);
    if (type == "delay") return std::make_unique<DelayFx>(sr);
    if (type == "compressor") return std::make_unique<CompressorFx>(sr);
    return nullptr;
}

}  // namespace mf
