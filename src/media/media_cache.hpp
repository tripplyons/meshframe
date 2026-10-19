#pragma once

#include "media/decoder.hpp"

#include <condition_variable>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mf {

// Shared access to decoded media: an LRU of video frames, a small pool of
// decoders per file, and fully decoded audio. Thread-safe.
class MediaCache {
public:
    explicit MediaCache(size_t frameBudgetBytes = size_t(1) << 30);
    ~MediaCache();
    MediaCache(const MediaCache&) = delete;
    MediaCache& operator=(const MediaCache&) = delete;

    // Frame shown at source time t, scaled to fit maxW x maxH. Null when the
    // file cannot be decoded or t is past the end.
    ImagePtr frame(const MediaSource& m, double t, int maxW, int maxH);

    // Decoded audio. Starts decoding in the background and returns null until it
    // is ready, unless wait is true.
    std::shared_ptr<const AudioBuffer> audio(const MediaSource& m, int sampleRate, bool wait = false);

    // Last decode error for a path, or empty.
    std::string error(const std::string& path) const;
    size_t frameBytes() const;
    void clear();

private:
    struct FrameKey {
        std::string path;
        int64_t index;
        int w, h;
        auto operator<=>(const FrameKey&) const = default;
    };
    struct DecoderSlot {
        std::unique_ptr<VideoDecoder> dec;
        std::string path;
        int w = 0, h = 0;
        uint64_t lastUse = 0;
        bool busy = false;
    };
    struct AudioEntry {
        std::shared_ptr<const AudioBuffer> buffer;
        bool pending = false;
        bool failed = false;
    };

    void touch(const FrameKey& key);
    void insertFrame(const FrameKey& key, ImagePtr img);
    DecoderSlot* acquireDecoder(const FrameKey& key, int maxW, int maxH, double t, std::string* err);
    void setError(const std::string& path, std::string msg);

    mutable std::mutex mu_;
    std::condition_variable audioCv_;
    size_t budget_;
    size_t bytes_ = 0;
    uint64_t clock_ = 0;
    std::list<FrameKey> lru_;  // front = most recent
    std::map<FrameKey, std::pair<ImagePtr, std::list<FrameKey>::iterator>> frames_;
    std::vector<std::unique_ptr<DecoderSlot>> decoders_;
    std::map<std::pair<std::string, int>, AudioEntry> audio_;
    std::map<std::string, std::string> errors_;
    std::vector<std::thread> workers_;
};

}  // namespace mf
