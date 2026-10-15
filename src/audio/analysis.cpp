#include "audio/analysis.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace mf {

namespace {

constexpr double kAbsoluteGate = -70;  // LUFS
constexpr float kFullScale = 0.999f;
constexpr int kTaps = 12;  // true-peak interpolation taps per phase
constexpr int kOversample = 4;

// Windowed-sinc weights for a point `frac` (0..1) between taps kTaps/2-1 and kTaps/2.
const std::array<std::array<float, kTaps>, kOversample - 1>& interpolationTable() {
    static const auto table = [] {
        std::array<std::array<float, kTaps>, kOversample - 1> t{};
        for (int p = 1; p < kOversample; ++p) {
            double frac = double(p) / kOversample;
            constexpr int center = kTaps / 2 - 1;
            constexpr double halfSpan = kTaps / 2.0 + 0.5;
            double sum = 0;
            for (int k = 0; k < kTaps; ++k) {
                double x = (k - center) - frac;
                double sinc = std::abs(x) < 1e-12 ? 1 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
                double window = 0.5 * (1 + std::cos(std::numbers::pi * x / halfSpan));
                t[p - 1][k] = float(sinc * window);
                sum += sinc * window;
            }
            for (auto& w : t[p - 1]) w = float(w / sum);
        }
        return t;
    }();
    return table;
}

double percentile(std::vector<double> v, double q) {
    std::ranges::sort(v);
    double pos = q * double(v.size() - 1);
    size_t i = size_t(pos);
    double f = pos - double(i);
    return i + 1 < v.size() ? v[i] * (1 - f) + v[i + 1] * f : v[i];
}

// Power mean of blocks above a gate, in LUFS; none when no block passes.
std::optional<double> gatedLoudness(const std::vector<double>& energies, double gate) {
    double sum = 0;
    int n = 0;
    for (double e : energies)
        if (lufsOf(e) > gate) sum += e, ++n;
    if (n == 0) return std::nullopt;
    return lufsOf(sum / n);
}

}  // namespace

double lufsOf(double meanSquare) {
    return meanSquare > 0 ? -0.691 + 10 * std::log10(meanSquare) : -std::numeric_limits<double>::infinity();
}

double dbOf(double amplitude) {
    return amplitude > 0 ? 20 * std::log10(amplitude) : -std::numeric_limits<double>::infinity();
}

double AudioAnalyzer::Biquad::run(double x, int ch) {
    double y = b0 * x + z1[ch];
    z1[ch] = b1 * x - a1 * y + z2[ch];
    z2[ch] = b2 * x - a2 * y;
    return y;
}

AudioAnalyzer::AudioAnalyzer(int sampleRate, double start, int64_t frames, AudioAnalysisOptions options)
    : rate_(sampleRate), start_(start), total_(frames), opt_(options), history_(kTaps) {
    out_.start = start;
    out_.duration = double(frames) / sampleRate;
    out_.columns.assign(size_t(std::max(0, opt_.columns)), {std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()});
    subLen_ = std::max(1, int(std::lround(sampleRate * 0.1)));
    winLen_ = std::max(1, int(std::lround(sampleRate * 0.01)));
    stepLen_ = std::max<int64_t>(1, std::llround(sampleRate * opt_.step));

    // K-weighting (ITU-R BS.1770-4) for any sample rate, designed as in
    // libebur128: a high shelf, then a high pass. At 48 kHz these give the
    // coefficients published in the recommendation.
    double fs = sampleRate;
    {
        double f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
        double k = std::tan(std::numbers::pi * f0 / fs);
        double vh = std::pow(10, gain / 20), vb = std::pow(vh, 0.4996667741545416);
        double a0 = 1 + k / q + k * k;
        shelf_.b0 = (vh + vb * k / q + k * k) / a0;
        shelf_.b1 = 2 * (k * k - vh) / a0;
        shelf_.b2 = (vh - vb * k / q + k * k) / a0;
        shelf_.a1 = 2 * (k * k - 1) / a0;
        shelf_.a2 = (1 - k / q + k * k) / a0;
    }
    {
        double f0 = 38.13547087602444, q = 0.5003270373238773;
        double k = std::tan(std::numbers::pi * f0 / fs);
        double a0 = 1 + k / q + k * k;
        highpass_.b0 = 1;
        highpass_.b1 = -2;
        highpass_.b2 = 1;
        highpass_.a1 = 2 * (k * k - 1) / a0;
        highpass_.a2 = (1 - k / q + k * k) / a0;
    }
}

void AudioAnalyzer::push(const float* stereo, int frames) {
    const auto& table = interpolationTable();
    for (int i = 0; i < frames; ++i, ++done_) {
        float l = stereo[2 * i], r = stereo[2 * i + 1];

        double kl = highpass_.run(shelf_.run(l, 0), 0), kr = highpass_.run(shelf_.run(r, 1), 1);
        double k = kl * kl + kr * kr;
        subSum_ += k;
        stepSum_ += k;

        float al = std::abs(l), ar = std::abs(r), peak = std::max(al, ar);
        out_.samplePeak = std::max(out_.samplePeak, double(peak));
        stepPeak_ = std::max(stepPeak_, double(peak));
        double sq = double(l) * l + double(r) * r;
        sumSquares_ += sq;
        winSum_ += sq / 2;

        // True peak: interpolate between the middle taps of the history.
        history_[historyPos_] = {l, r};
        historyPos_ = (historyPos_ + 1) % kTaps;
        double tp = peak;
        for (const auto& weights : table)
            for (int ch = 0; ch < 2; ++ch) {
                double y = 0;
                for (int t = 0; t < kTaps; ++t) y += weights[t] * history_[(historyPos_ + t) % kTaps][ch];
                tp = std::max(tp, std::abs(y));
            }
        out_.truePeak = std::max(out_.truePeak, tp);

        // Clipping: runs of full-scale samples, merged across gaps under 10 ms.
        int clipped = (al >= kFullScale) + (ar >= kFullScale);
        if (clipped) {
            if (clipEnd_ >= 0 && done_ - clipEnd_ <= winLen_) {
                out_.clipping.back().end = time(done_ + 1);
                out_.clipping.back().samples += clipped;
            } else {
                out_.clipping.push_back({time(done_), time(done_ + 1), clipped});
            }
            clipEnd_ = done_;
        }

        if (!out_.columns.empty() && total_ > 0) {
            size_t col = std::min(out_.columns.size() - 1, size_t(double(done_) * double(out_.columns.size()) / double(total_)));
            float m = (l + r) / 2;
            out_.columns[col][0] = std::min(out_.columns[col][0], m);
            out_.columns[col][1] = std::max(out_.columns[col][1], m);
        }

        if (++subFill_ == subLen_) endSubBlock();
        if (++winFill_ == winLen_) endWindow();
        if (++stepFill_ == stepLen_) endStep();
    }
}

void AudioAnalyzer::endSubBlock() {
    subBlocks_.push_back(subSum_ / subFill_);
    subSum_ = 0;
    subFill_ = 0;
}

void AudioAnalyzer::endWindow() {
    int64_t first = done_ + 1 - winFill_;
    bool silent = winSum_ / winFill_ < std::pow(10, opt_.silenceDb / 10);
    if (silent && silentSince_ < 0) silentSince_ = first;
    if (!silent && silentSince_ >= 0) {
        if (double(first - silentSince_) / rate_ >= opt_.minSilence) out_.silence.push_back({time(silentSince_), time(first)});
        silentSince_ = -1;
    }
    winSum_ = 0;
    winFill_ = 0;
}

void AudioAnalyzer::endStep() {
    double ms = stepSum_ / stepFill_;
    double l = lufsOf(ms);
    out_.steps.push_back({time(done_ + 1 - stepFill_), l > kAbsoluteGate ? std::optional(l) : std::nullopt, stepPeak_});
    stepSum_ = stepPeak_ = 0;
    stepFill_ = 0;
}

AudioAnalysis AudioAnalyzer::finish() {
    if (winFill_ > 0) endWindow();
    if (silentSince_ >= 0 && double(done_ - silentSince_) / rate_ >= opt_.minSilence)
        out_.silence.push_back({time(silentSince_), time(done_)});
    silentSince_ = -1;
    if (stepFill_ > 0) endStep();
    // A partial sub-block would skew the 400 ms blocks, so it is dropped.

    // Integrated loudness: 400 ms blocks every 100 ms, absolute then relative gate.
    std::vector<double> blocks;
    for (size_t i = 0; i + 4 <= subBlocks_.size(); ++i)
        blocks.push_back((subBlocks_[i] + subBlocks_[i + 1] + subBlocks_[i + 2] + subBlocks_[i + 3]) / 4);
    if (auto ungated = gatedLoudness(blocks, kAbsoluteGate)) out_.integrated = gatedLoudness(blocks, std::max(kAbsoluteGate, *ungated - 10));

    // Loudness range: 3 s blocks every 100 ms, gated, 10th to 95th percentile.
    std::vector<double> shortTerm;
    for (size_t i = 0; i + 30 <= subBlocks_.size(); ++i) {
        double sum = 0;
        for (size_t j = i; j < i + 30; ++j) sum += subBlocks_[j];
        shortTerm.push_back(sum / 30);
    }
    if (auto ungated = gatedLoudness(shortTerm, kAbsoluteGate)) {
        std::vector<double> kept;
        for (double e : shortTerm)
            if (double l = lufsOf(e); l > kAbsoluteGate && l > *ungated - 20) kept.push_back(l);
        if (!kept.empty()) out_.range = percentile(kept, 0.95) - percentile(kept, 0.10);
    }

    out_.rms = done_ > 0 ? std::sqrt(sumSquares_ / (2.0 * double(done_))) : 0;
    for (auto& c : out_.columns)
        if (c[0] > c[1]) c = {0, 0};
    return std::move(out_);
}

}  // namespace mf
