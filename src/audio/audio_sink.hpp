#pragma once

#include <memory>
#include <string>

namespace mf {

// Destination for rendered audio: the playback device, or a fake in tests.
class AudioSink {
public:
    virtual ~AudioSink() = default;
    virtual void queue(const float* stereo, int frames) = 0;
    // Frames queued but not yet played.
    virtual int queuedFrames() const = 0;
    virtual void clear() = 0;
    virtual void setPaused(bool paused) = 0;
};

// Opens the default playback device as float stereo. Null when no device is available.
std::unique_ptr<AudioSink> openSdlAudio(int sampleRate, std::string* err);

}  // namespace mf
