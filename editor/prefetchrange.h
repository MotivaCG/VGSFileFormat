// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Victor M. Feliz. All rights reserved.
#pragma once

#include <QtGlobal>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

// Reads a range of a mapped file into memory in large sequential requests, returning once it
// is there. Read on demand instead, page by page as it is touched, a chunk of tens of megabytes
// takes seconds from a hard disk. The file's pages are shared by its mappings and by ordinary
// reads, so a read of the range afterwards is served from memory too.
inline void prefetchMapped(const void *data, size_t size) {
    if (!data || !size) return;
#ifdef Q_OS_WIN
    WIN32_MEMORY_RANGE_ENTRY range{const_cast<void *>(data), SIZE_T(size)};
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
#else
    const quintptr page = quintptr(sysconf(_SC_PAGESIZE));
    const quintptr from = quintptr(data) / page * page, to = quintptr(data) + size;
    posix_madvise(reinterpret_cast<void *>(from), size_t(to - from), POSIX_MADV_WILLNEED);
#endif
}

namespace prefetchdetail {
// One thread, so ranges are read one after another rather than making a disk seek between them.
class Reader {
public:
    static Reader &instance() { static Reader reader; return reader; }
    void add(const void *owner, const void *data, size_t size) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // A long wait means the disk is behind: what was asked for first is likely played
            // past already.
            while (jobs_.size() >= 6) jobs_.pop_front();
            jobs_.push_back({owner, data, size});
        }
        ready_.notify_all();
    }
    // Drops what `owner` asked for and waits for a read of it under way.
    void forget(const void *owner) {
        std::unique_lock<std::mutex> lock(mutex_);
        for (auto it = jobs_.begin(); it != jobs_.end();) it = it->owner == owner ? jobs_.erase(it) : it + 1;
        done_.wait(lock, [&] { return running_ != owner; });
    }
    ~Reader() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; jobs_.clear(); }
        ready_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
private:
    struct Job { const void *owner; const void *data; size_t size; };
    Reader() : thread_([this] { run(); }) {}
    void run() {
        for (;;) {
            Job job{};
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
                if (stop_) return;
                job = jobs_.front(); jobs_.pop_front(); running_ = job.owner;
            }
            prefetchMapped(job.data, job.size);
            { std::lock_guard<std::mutex> lock(mutex_); running_ = nullptr; }
            done_.notify_all();
        }
    }
    std::mutex mutex_;
    std::condition_variable ready_, done_;
    std::deque<Job> jobs_;
    const void *running_ = nullptr;
    bool stop_ = false;
    std::thread thread_;
};
}

// The same, on a thread of its own: returns at once. For a range wanted later, such as the chunk
// after the one playing, so reading it does not hold up the caller. The mapping must stay until
// prefetchForget(owner) has returned.
inline void prefetchMappedLater(const void *owner, const void *data, size_t size) {
    if (!data || !size) return;
    prefetchdetail::Reader::instance().add(owner, data, size);
}
// Before `owner` unmaps what it asked to have read: its reads are dropped, or waited for.
inline void prefetchForget(const void *owner) { prefetchdetail::Reader::instance().forget(owner); }
