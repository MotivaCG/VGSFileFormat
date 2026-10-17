// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

namespace previewmemory {
constexpr uint64_t MiB=1024ull*1024,GiB=1024*MiB;
struct Snapshot {
    uint64_t physicalTotal=0,physicalAvailable=0;
    uint64_t commitTotal=0,commitAvailable=0;
    bool valid=false,lowMemory=false;
};
Snapshot systemSnapshot();
using Probe=std::function<Snapshot()>;
struct Allowance {
    uint64_t growth=0,reclaim=0;
    bool paused=false;
};
// Optional retention only. Active frames, decoding and another process's allocations
// cannot be reserved by this policy. All preview caches consult the same instance.
class Guard {
public:
    explicit Guard(Probe probe={});
    Allowance inspect();
    bool canRetain(size_t bytes) {const auto allowed=inspect();return !allowed.paused && uint64_t(bytes)<=allowed.growth;}
private:
    Probe probe_;
    bool paused_=false;
};
}
