#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace mf {

// Fixed worker pool for data-parallel loops (pixel rows, audio blocks).
class ThreadPool {
public:
    explicit ThreadPool(unsigned threads = 0);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Splits [begin, end) into chunks and runs fn(chunkBegin, chunkEnd) on all
    // workers plus the calling thread. Blocks until every chunk is done.
    void parallelFor(size_t begin, size_t end, const std::function<void(size_t, size_t)>& fn, size_t minChunk = 16);

    unsigned size() const { return unsigned(workers_.size()) + 1; }
    static ThreadPool& shared();

private:
    void workerLoop();
    bool runOneChunk();

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    const std::function<void(size_t, size_t)>* job_ = nullptr;
    size_t next_ = 0, end_ = 0, chunk_ = 1, pending_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;
    std::mutex submit_;
};

}  // namespace mf
