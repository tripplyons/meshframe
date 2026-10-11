#pragma once

#include "audio/dsp.hpp"
#include "engine/evaluator.hpp"
#include "media/media_cache.hpp"

#include <array>
#include <unordered_map>

namespace mf {

struct AudioLevels {
    std::array<float, 2> peak{};  // linear, per channel
    std::array<float, 2> rms{};
};

// Mixes audio tracks, clip effects, transitions, and the master chain into
// interleaved stereo at the project sample rate. Effects keep state between
// calls, so render consecutive ranges and call reset() after a seek.
class AudioMixer {
public:
    static constexpr int kBlock = 256;  // parameters are evaluated once per block

    explicit AudioMixer(MediaCache& cache) : cache_(cache) {}

    // Renders `frames` frames starting at timeline time t0. With wait, blocks
    // until media audio is decoded (export and tests); otherwise unready media is silent.
    void render(Evaluator& ev, double t0, int frames, float* out, bool wait = false);
    void reset();
    const AudioLevels& levels() const { return levels_; }

private:
    struct FxState {
        std::string type;
        std::unique_ptr<AudioEffect> fx;
    };
    void renderBlock(Evaluator& ev, double t0, int frames, float* out, bool wait);
    void runChain(Evaluator& ev, const std::vector<Effect>& chain, float* buf, int frames);
    // Writes a clip's source samples for frames [i0, i1) of the block.
    bool clipSource(Evaluator& ev, const Clip& c, double t0, int i0, int i1, float* buf, bool wait);

    MediaCache& cache_;
    std::unordered_map<Id, FxState> fx_;
    AudioLevels levels_;
    std::vector<float> clipBuf_, trackBuf_;
};

}  // namespace mf
