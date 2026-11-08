// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Victor M. Feliz. All rights reserved.

#include "packedsource.h"
#include "vgscodec.h"
#include "vgsdecoder/vgsdecoder.h"
#include <algorithm>

std::shared_ptr<PackedChunk> packLogicalChunk(const vgs::DecodedChunk &chunk, size_t index, unsigned shDegree) {
    // What the decoder's describePacked() makes of a chunk it has decoded: views of its pages.
    std::vector<vgsdec::GroupData> groups;
    std::vector<vgsdec::Buffer> buffers;
    groups.reserve(chunk.groups.size());
    buffers.reserve(chunk.pages.size());
    uint64_t samples = 0, total = 0;
    for (const auto &group : chunk.groups) {
        vgsdec::GroupData out;
        out.type = group.type; out.flags = group.flags;
        out.splats = group.splats; out.intervals = group.intervals;
        for (size_t k = 0; k < 6; ++k) out.counts[k] = group.counts[k];
        out.positionMin = group.positionMin; out.positionMax = group.positionMax;
        out.trajectoryMin = group.trajectoryMin; out.trajectoryMax = group.trajectoryMax;
        samples = std::max(samples, group.intervals + 1);
        groups.push_back(out);
    }
    for (const auto &page : chunk.pages) {
        vgsdec::Buffer buffer;
        buffer.attribute = page.descriptor.attribute; buffer.group = page.descriptor.group;
        buffer.layer = page.descriptor.layer; buffer.firstRow = page.descriptor.firstRow;
        buffer.rows = page.descriptor.spec.rows; buffer.totalRows = page.descriptor.totalRows;
        buffer.kind = page.descriptor.spec.kind; buffer.width = page.descriptor.spec.width;
        buffer.data = page.bytes.data(); buffer.size = page.bytes.size();
        total += page.bytes.size();
        buffers.push_back(buffer);
    }
    vgsdec::ChunkData data;
    data.chunkIndex = index; data.sampleCount = samples;
    data.groups = groups.data(); data.groupCount = groups.size();
    data.buffers = buffers.data(); data.bufferCount = buffers.size();
    data.totalBytes = total;
    return packChunk(data, shDegree);
}

PackedChunkSource::PackedChunkSource(Packer packer) : packer_(std::move(packer)), thread_([this] { run(); }) {}

PackedChunkSource::~PackedChunkSource() {
    { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
    work_.notify_all();
    thread_.join();
}

PackedChunkPtr PackedChunkSource::chunk(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = index;
    if (const auto it = held_.find(index); it != held_.end()) return it->second;
    if (!failed_ && packing_ != index) {
        queue_.erase(std::remove(queue_.begin(), queue_.end(), index), queue_.end());
        queue_.push_front(index);
        work_.notify_all();
    }
    return nullptr;
}

void PackedChunkSource::want(const std::vector<size_t> &indices) {
    std::lock_guard<std::mutex> lock(mutex_);
    wanted_ = indices;
    // What the last chunk() asked for stays first.
    const bool current = std::find(queue_.begin(), queue_.end(), current_) != queue_.end();
    queue_.clear();
    if (current) queue_.push_back(current_);
    for (size_t index : indices)
        if (!held_.count(index) && index != packing_ && std::find(queue_.begin(), queue_.end(), index) == queue_.end())
            queue_.push_back(index);
    evict();
    if (!queue_.empty()) work_.notify_all();
}

bool PackedChunkSource::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
}

void PackedChunkSource::evict() {
    for (auto it = held_.begin(); it != held_.end() && held_.size() > 3;) {
        const bool keep = it->first == current_ || std::find(wanted_.begin(), wanted_.end(), it->first) != wanted_.end();
        it = keep ? std::next(it) : held_.erase(it);
    }
    // Still over: the chunks wanted latest go first.
    while (held_.size() > 3) {
        auto victim = held_.end();
        size_t rank = 0;
        for (auto it = held_.begin(); it != held_.end(); ++it) {
            if (it->first == current_) continue;
            const auto at = std::find(wanted_.begin(), wanted_.end(), it->first);
            const size_t r = size_t(at - wanted_.begin());
            if (victim == held_.end() || r >= rank) { victim = it; rank = r; }
        }
        if (victim == held_.end()) break;
        held_.erase(victim);
    }
}

void PackedChunkSource::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        work_.wait(lock, [&] { return stop_ || (!failed_ && !queue_.empty()); });
        if (stop_) return;
        const size_t index = queue_.front();
        queue_.pop_front();
        if (held_.count(index)) continue;
        packing_ = index;
        lock.unlock();
        PackedChunkPtr packed;
        bool failure = false;
        try { packed = packer_(index); } catch (...) { failure = true; }
        lock.lock();
        packing_ = size_t(-1);
        if (failure || !packed) { failed_ = true; queue_.clear(); continue; }
        held_[index] = std::move(packed);
        evict();
    }
}
