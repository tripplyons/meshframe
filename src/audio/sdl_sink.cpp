#include "audio/audio_sink.hpp"

#include <SDL3/SDL.h>

namespace mf {

namespace {

class SdlSink : public AudioSink {
public:
    explicit SdlSink(SDL_AudioStream* s) : stream_(s) {}
    ~SdlSink() override { SDL_DestroyAudioStream(stream_); }
    void queue(const float* stereo, int frames) override {
        SDL_PutAudioStreamData(stream_, stereo, frames * int(sizeof(float)) * 2);
    }
    int queuedFrames() const override { return SDL_GetAudioStreamQueued(stream_) / int(sizeof(float) * 2); }
    void clear() override { SDL_ClearAudioStream(stream_); }
    void setPaused(bool paused) override {
        if (paused) SDL_PauseAudioStreamDevice(stream_);
        else SDL_ResumeAudioStreamDevice(stream_);
    }

private:
    SDL_AudioStream* stream_;
};

}  // namespace

std::unique_ptr<AudioSink> openSdlAudio(int sampleRate, std::string* err) {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        if (err) *err = SDL_GetError();
        return nullptr;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, sampleRate};
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s) {
        if (err) *err = SDL_GetError();
        return nullptr;
    }
    return std::make_unique<SdlSink>(s);
}

}  // namespace mf
