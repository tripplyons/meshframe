#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace mf {

struct TimeRange {
    double start = 0, end = 0;
};

struct ClipRange {
    double start = 0, end = 0;
    int64_t samples = 0;  // clipped samples, both channels
};

struct LoudnessStep {
    double time = 0;
    std::optional<double> lufs;  // none when silent
    double peak = 0;             // linear sample peak
};

struct AudioAnalysis {
    double start = 0, duration = 0;
    std::optional<double> integrated;  // LUFS, gated per ITU-R BS.1770-4
    std::optional<double> range;       // LU, per EBU Tech 3342
    double truePeak = 0;               // linear, 4x oversampled
    double samplePeak = 0;             // linear
    double rms = 0;                    // linear, both channels
    std::vector<TimeRange> silence;
    std::vector<ClipRange> clipping;
    std::vector<LoudnessStep> steps;
    // Per waveform column: min and max of the mono mix.
    std::vector<std::array<float, 2>> columns;
};

struct AudioAnalysisOptions {
    double step = 0.5;               // seconds per LoudnessStep
    double silenceDb = -50;          // dBFS RMS
    double minSilence = 0.3;         // seconds
    int columns = 0;                 // waveform columns, 0 for none
};

// Measures interleaved stereo pushed in order. `start` is the time of the
// first frame and `frames` the total count, used to place waveform columns.
class AudioAnalyzer {
public:
    AudioAnalyzer(int sampleRate, double start, int64_t frames, AudioAnalysisOptions options);
    void push(const float* stereo, int frames);
    AudioAnalysis finish();

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        std::array<double, 2> z1{}, z2{};
        double run(double x, int ch);
    };
    void endSubBlock();
    void endWindow();
    void endStep();
    double time(int64_t frame) const { return start_ + double(frame) / rate_; }

    int rate_;
    double start_;
    int64_t total_;
    AudioAnalysisOptions opt_;
    AudioAnalysis out_;
    Biquad shelf_, highpass_;
    int64_t done_ = 0;

    // 100 ms K-weighted sub-blocks: mean square summed over channels.
    int subLen_;
    int subFill_ = 0;
    double subSum_ = 0;
    std::vector<double> subBlocks_;

    // True peak: the last taps of each channel.
    std::vector<std::array<float, 2>> history_;
    size_t historyPos_ = 0;

    double sumSquares_ = 0;

    // 10 ms windows for silence.
    int winLen_;
    int winFill_ = 0;
    double winSum_ = 0;
    int64_t silentSince_ = -1;

    int64_t clipEnd_ = -1;

    int64_t stepLen_;
    int64_t stepFill_ = 0;
    double stepSum_ = 0, stepPeak_ = 0;
};

// Loudness in LUFS of a K-weighted mean square summed over channels.
double lufsOf(double meanSquare);
// Decibels of a linear amplitude; -inf for 0.
double dbOf(double amplitude);

}  // namespace mf
