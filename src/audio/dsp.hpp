#pragma once

#include "core/value.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace mf {

double dbToGain(double db);
double gainToDb(double gain);

// Stateful audio effect processing interleaved stereo float in place.
class AudioEffect {
public:
    virtual ~AudioEffect() = default;
    // Reads parameters once per block, then processes `frames` frames.
    virtual void process(float* stereo, int frames, const ParamFn& param) = 0;
    virtual void reset() {}
};

// Creates the processor for an effect type, or null for an unknown type.
std::unique_ptr<AudioEffect> makeAudioEffect(std::string_view type, int sampleRate);

// Balance pan: -1 is left only, 1 is right only, 0 leaves both at unity.
inline void panGains(double pan, float& l, float& r) {
    l = float(pan > 0 ? 1 - pan : 1);
    r = float(pan < 0 ? 1 + pan : 1);
}

}  // namespace mf
