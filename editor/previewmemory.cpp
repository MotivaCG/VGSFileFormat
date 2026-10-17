// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
#include "previewmemory.h"
#include <QtGlobal>
#include <algorithm>
#include <limits>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <QFile>
#endif

namespace previewmemory {
Snapshot systemSnapshot() {
    Snapshot out;
#ifdef Q_OS_WIN
    MEMORYSTATUSEX memory={};memory.dwLength=sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) return out;
    out={memory.ullTotalPhys,memory.ullAvailPhys,memory.ullTotalPageFile,memory.ullAvailPageFile,true,false};
    struct Notification {
        HANDLE handle=CreateMemoryResourceNotification(LowMemoryResourceNotification);
        ~Notification() {if (handle) CloseHandle(handle);}
    };
    static Notification notification;
    BOOL low=FALSE;
    if (notification.handle && QueryMemoryResourceNotification(notification.handle,&low)) out.lowMemory=low;
#elif defined(Q_OS_LINUX)
    QFile file(QStringLiteral("/proc/meminfo"));
    if (!file.open(QIODevice::ReadOnly)) return out;
    for (const auto &line:file.readAll().split('\n')) {
        const auto fields=line.simplified().split(' ');if (fields.size()<2) continue;
        if (fields[0]=="MemTotal:") out.physicalTotal=fields[1].toULongLong()*1024;
        if (fields[0]=="MemAvailable:") out.physicalAvailable=fields[1].toULongLong()*1024;
    }
    // Linux overcommit does not expose the Windows commit guarantee. Physical
    // MemAvailable still includes reclaimable file pages, rather than free pages alone.
    out.valid=out.physicalTotal>0;
#endif
    return out;
}
Guard::Guard(Probe probe):probe_(probe ? std::move(probe) : Probe(systemSnapshot)) {}
Allowance Guard::inspect() {
    const auto memory=probe_();
    if (!memory.valid || !memory.physicalTotal) return {0,0,true};
    const uint64_t physicalReserve=std::max(2*GiB,memory.physicalTotal/10);
    const uint64_t commitReserve=std::max(GiB,memory.commitTotal/20);
    const bool commitKnown=memory.commitTotal>0;
    constexpr uint64_t hysteresis=512*MiB;
    const bool low=memory.lowMemory || memory.physicalAvailable<physicalReserve ||
        (commitKnown && memory.commitAvailable<commitReserve);
    if (low) paused_=true;
    if (paused_ && !low && memory.physicalAvailable>=physicalReserve+hysteresis &&
        (!commitKnown || memory.commitAvailable>=commitReserve+hysteresis)) paused_=false;
    if (paused_) {
        const auto deficit=[](uint64_t target,uint64_t available) {return target>available ? target-available : uint64_t(0);};
        const uint64_t reclaim=std::max({deficit(physicalReserve+hysteresis,memory.physicalAvailable),
            commitKnown ? deficit(commitReserve+hysteresis,memory.commitAvailable) : uint64_t(0),
            memory.lowMemory ? 256*MiB : uint64_t(0)});
        return {0,reclaim,true};
    }
    return {std::min(memory.physicalAvailable-physicalReserve,
        commitKnown ? memory.commitAvailable-commitReserve : std::numeric_limits<uint64_t>::max()),0,false};
}
}
