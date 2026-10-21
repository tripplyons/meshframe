#pragma once

#include "model/project.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mf {

struct ExportOptions {
    std::string path;      // .mp4 writes H.264 + AAC; .wav writes 16-bit PCM audio only
    int width = 0;         // 0 uses the project size
    int height = 0;
    double start = 0;
    double end = -1;       // negative uses the project duration
    int crf = 18;          // x264 CRF; VideoToolbox maps it to quality 116 - 2 * crf
    int audioBitrate = 192000;
};

struct ExportProgress {
    std::atomic<double> fraction{0};
    std::atomic<bool> cancel{false};
};

// Renders and encodes on the calling thread with its own evaluator and media
// cache. On failure or cancel, returns false, sets err, and removes the file.
bool exportProject(const Project& p, const ExportOptions& opts, ExportProgress* progress, std::string* err);

// Runs one export on a background thread.
class ExportJob {
public:
    ~ExportJob();
    // Fails if an export is already running.
    bool start(const Project& p, ExportOptions opts, std::string* err);
    void cancel();
    bool running() const { return running_; }
    double progress() const { return progress_.fraction; }
    // Result of the last finished export: empty while running or on success.
    std::string error() const;
    const ExportOptions& options() const { return opts_; }
    bool finished() const { return finished_; }
    // How many exports have started, so a watcher can tell one result from the next.
    uint64_t started() const { return started_; }
    void wait();

private:
    std::thread thread_;
    ExportProgress progress_;
    ExportOptions opts_;
    std::atomic<bool> running_{false}, finished_{false};
    mutable std::mutex mu_;
    std::string error_;
    uint64_t started_ = 0;
};

}  // namespace mf
