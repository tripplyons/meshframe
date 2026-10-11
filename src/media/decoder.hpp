#pragma once

#include "model/project.hpp"
#include "render/image.hpp"

#include <memory>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace mf {

// Reads stream facts. Returns false and sets err when the file has no usable stream.
bool probeMedia(const std::string& path, MediaSource& out, std::string* err);

// Interleaved stereo float samples at a fixed rate, with min/max peaks for drawing.
struct AudioBuffer {
    int sampleRate = 48000;
    std::vector<float> samples;
    static constexpr int kPeakBlock = 256;
    std::vector<float> peaks;  // per block: min, max (mono mix)

    size_t frames() const { return samples.size() / 2; }
    double duration() const { return double(frames()) / sampleRate; }
};

std::shared_ptr<AudioBuffer> decodeAudio(const std::string& path, int sampleRate, std::string* err);

// Sequential-friendly video decoder with seeking. Not thread-safe.
class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Output frames are scaled to fit within maxW x maxH (keeping aspect), never upscaled.
    bool open(const std::string& path, int maxW, int maxH, std::string* err);
    // Returns the frame shown at source time t, or null past the end.
    ImagePtr frameAt(double t);
    // Source time of the frame last returned; used to pick a decoder for a request.
    double position() const { return curPts_; }
    double frameDuration() const { return frameDur_; }
    int width() const { return outW_; }
    int height() const { return outH_; }

private:
    bool decodeNext(AVFrame* into);
    void seek(double t);
    ImagePtr convert(AVFrame* f);

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* cur_ = nullptr;
    AVFrame* next_ = nullptr;
    SwsContext* sws_ = nullptr;
    int stream_ = -1;
    double timeBase_ = 0;
    double startTime_ = 0;
    double frameDur_ = 1.0 / 30;
    double curPts_ = -1e9;
    bool haveCur_ = false, haveNext_ = false, eof_ = false;
    int outW_ = 0, outH_ = 0;
    ImagePtr curImage_;
};

}  // namespace mf
