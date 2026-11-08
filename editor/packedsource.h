// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Victor M. Feliz. All rights reserved.
#pragma once

#include "packedchunk.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace vgs { struct DecodedChunk; }

// A chunk as the logical arrays a .mint holds (vgs::MintLogicalSource), packed as the decoder's
// Packed mode would pack the same arrays read from a .vgs.
std::shared_ptr<PackedChunk> packLogicalChunk(const vgs::DecodedChunk &chunk, size_t index, unsigned shDegree);

// Packs chunks for the GPU on a thread of its own, ahead of the frames that need them, so that
// the caller never waits for one: a frame whose chunk is not packed yet is drawn the floats way.
// At most three chunks are held.
class PackedChunkSource {
public:
    // Runs on the source's thread, which is the only one it is ever called from: the chunk
    // packed, or null when it cannot be. An exception stops the source (failed()).
    using Packer = std::function<PackedChunkPtr(size_t index)>;
    explicit PackedChunkSource(Packer packer);
    ~PackedChunkSource();
    PackedChunkSource(const PackedChunkSource &) = delete;
    PackedChunkSource &operator=(const PackedChunkSource &) = delete;

    // The chunk if it is packed. Null otherwise, and then it is packed before anything else.
    PackedChunkPtr chunk(size_t index);
    // The chunks to have packed next, soonest first. Held chunks neither asked for here nor
    // by the last chunk() call go.
    void want(const std::vector<size_t> &indices);
    bool failed() const;
    // Whether the chunk is packed, without asking for it.
    bool holds(size_t index) const;

private:
    void run();
    void evict(); // under the lock
    Packer packer_;
    mutable std::mutex mutex_;
    std::condition_variable work_;
    std::deque<size_t> queue_;
    std::vector<size_t> wanted_;
    size_t current_ = size_t(-1), packing_ = size_t(-1);
    std::map<size_t, PackedChunkPtr> held_;
    bool stop_ = false, failed_ = false;
    std::thread thread_;
};
