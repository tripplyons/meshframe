#include "media/media_cache.hpp"

#include <cmath>

namespace mf {

namespace {
constexpr int kDecodersPerFile = 2;
constexpr double kForwardReach = 2.0;  // seconds a decoder may run forward instead of seeking
}  // namespace

MediaCache::MediaCache(size_t frameBudgetBytes) : budget_(frameBudgetBytes) {}

MediaCache::~MediaCache() {
    for (auto& w : workers_)
        if (w.joinable()) w.join();
}

void MediaCache::touch(const FrameKey& key) {
    auto it = frames_.find(key);
    lru_.splice(lru_.begin(), lru_, it->second.second);
}

void MediaCache::insertFrame(const FrameKey& key, ImagePtr img) {
    if (frames_.contains(key)) return;
    lru_.push_front(key);
    bytes_ += img->bytes();
    frames_.emplace(key, std::pair{std::move(img), lru_.begin()});
    while (bytes_ > budget_ && lru_.size() > 1) {
        auto old = frames_.find(lru_.back());
        bytes_ -= old->second.first->bytes();
        frames_.erase(old);
        lru_.pop_back();
    }
}

MediaCache::DecoderSlot* MediaCache::acquireDecoder(const std::string& path, int w, int h, double t,
                                                    std::string* err) {
    DecoderSlot* best = nullptr;
    DecoderSlot* oldest = nullptr;
    int count = 0;
    for (auto& s : decoders_) {
        if (s->path != path || s->w != w || s->h != h) continue;
        ++count;
        if (s->busy) continue;
        double pos = s->dec->position();
        bool reachable = t + 1e-6 >= pos && t <= pos + kForwardReach;
        if (reachable && (!best || pos > best->dec->position())) best = s.get();
        if (!oldest || s->lastUse < oldest->lastUse) oldest = s.get();
    }
    if (!best && count >= kDecodersPerFile && oldest) best = oldest;  // will seek
    if (!best) {
        auto slot = std::make_unique<DecoderSlot>();
        slot->dec = std::make_unique<VideoDecoder>();
        slot->path = path;
        slot->w = w;
        slot->h = h;
        if (!slot->dec->open(path, w, h, err)) return nullptr;
        decoders_.push_back(std::move(slot));
        best = decoders_.back().get();
    }
    best->busy = true;
    best->lastUse = ++clock_;
    return best;
}

ImagePtr MediaCache::frame(const MediaSource& m, double t, int maxW, int maxH) {
    if (!m.hasVideo || m.path.empty()) return nullptr;
    double fps = m.fps > 0 ? m.fps : 30;
    bool still = m.kind == MediaKind::Image;
    if (still) t = 0;
    if (t < 0) t = 0;
    FrameKey key{m.path, still ? 0 : int64_t(std::floor(t * fps + 1e-4)), maxW, maxH};
    DecoderSlot* slot = nullptr;
    {
        std::lock_guard lock(mu_);
        if (auto it = frames_.find(key); it != frames_.end()) {
            touch(key);
            return it->second.first;
        }
        if (errors_.contains(m.path)) return nullptr;
        std::string err;
        slot = acquireDecoder(m.path, maxW, maxH, t, &err);
        if (!slot) {
            errors_[m.path] = err;
            return nullptr;
        }
    }
    // Decode outside the lock so other files proceed in parallel.
    ImagePtr img = slot->dec->frameAt(still ? 0 : (key.index + 0.5) / fps);
    std::lock_guard lock(mu_);
    slot->busy = false;
    if (img) insertFrame(key, img);
    return img;
}

std::shared_ptr<const AudioBuffer> MediaCache::audio(const MediaSource& m, int sampleRate, bool wait) {
    if (!m.hasAudio || m.path.empty()) return nullptr;
    std::unique_lock lock(mu_);
    auto key = std::pair{m.path, sampleRate};
    AudioEntry& e = audio_[key];
    if (!e.buffer && !e.pending && !e.failed) {
        e.pending = true;
        workers_.emplace_back([this, key] {
            std::string err;
            auto buf = decodeAudio(key.first, key.second, &err);
            std::lock_guard lock(mu_);
            AudioEntry& entry = audio_[key];
            entry.pending = false;
            if (buf) entry.buffer = std::move(buf);
            else {
                entry.failed = true;
                errors_[key.first] = err;
            }
            audioCv_.notify_all();
        });
    }
    if (wait) audioCv_.wait(lock, [&] { return !audio_[key].pending; });
    return audio_[key].buffer;
}

std::string MediaCache::error(const std::string& path) const {
    std::lock_guard lock(mu_);
    auto it = errors_.find(path);
    return it == errors_.end() ? std::string{} : it->second;
}

size_t MediaCache::frameBytes() const {
    std::lock_guard lock(mu_);
    return bytes_;
}

void MediaCache::clear() {
    std::unique_lock lock(mu_);
    audioCv_.wait(lock, [&] {
        for (auto& [k, e] : audio_)
            if (e.pending) return false;
        return true;
    });
    frames_.clear();
    lru_.clear();
    bytes_ = 0;
    std::erase_if(decoders_, [](auto& s) { return !s->busy; });
    audio_.clear();
    errors_.clear();
}

}  // namespace mf
