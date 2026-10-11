#include "core/thread_pool.hpp"

#include <algorithm>

namespace mf {

ThreadPool::ThreadPool(unsigned threads) {
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned i = 1; i < threads; ++i) workers_.emplace_back([this] { workerLoop(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    for (auto& w : workers_) w.join();
}

ThreadPool& ThreadPool::shared() {
    static ThreadPool pool;
    return pool;
}

bool ThreadPool::runOneChunk() {
    size_t b, e;
    const std::function<void(size_t, size_t)>* job;
    {
        std::lock_guard lock(mutex_);
        if (!job_ || next_ >= end_) return false;
        b = next_;
        e = std::min(end_, b + chunk_);
        next_ = e;
        job = job_;
    }
    (*job)(b, e);
    {
        std::lock_guard lock(mutex_);
        pending_ -= (e - b);
        if (pending_ == 0) done_.notify_all();
    }
    return true;
}

void ThreadPool::workerLoop() {
    uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || (job_ && generation_ != seen && next_ < end_); });
            if (stop_) return;
            seen = generation_;
        }
        while (runOneChunk()) {}
    }
}

void ThreadPool::parallelFor(size_t begin, size_t end, const std::function<void(size_t, size_t)>& fn, size_t minChunk) {
    if (end <= begin) return;
    size_t count = end - begin;
    if (workers_.empty() || count <= minChunk) {
        fn(begin, end);
        return;
    }
    std::lock_guard submitLock(submit_);
    {
        std::lock_guard lock(mutex_);
        job_ = &fn;
        next_ = begin;
        end_ = end;
        pending_ = count;
        chunk_ = std::max(minChunk, count / (size() * 4) + 1);
        ++generation_;
    }
    wake_.notify_all();
    while (runOneChunk()) {}
    std::unique_lock lock(mutex_);
    done_.wait(lock, [&] { return pending_ == 0; });
    job_ = nullptr;
}

}  // namespace mf
