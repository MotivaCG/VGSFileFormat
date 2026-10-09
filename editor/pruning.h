// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#pragma once
#include "project.h"
#include "vgsframe.h"
#include <functional>
#include <vector>

// Prune low contribution. Each chunk is rendered from around the capture - a ring from
// afar, a ring from close, from above and from below - at a few instants, the way the
// viewport draws splats, and every record is credited with its blending weight over the
// pixels it touches. That is its contribution: what the image loses without it. Hidden,
// faint and tiny splats score near nothing.
//
// The decision is per chunk and per record, never per frame, so a splat is kept or
// removed for its whole life in the chunk and the preview shows what the export writes.

// The instants of a chunk spanning [start, end) that score it.
std::vector<double> pruneSampleTimes(double start, double end);

// Each record's contribution, as the area it covers at full weight in a 1080p view, averaged
// over the views and the instants it is alive at. Records alive at none of the instants get
// -1: nothing was measured, so nothing may be removed. Every sample holds the same records.
std::vector<float> contributionScores(const std::vector<vgs::Frame> &samples, bool antialiased,
                                      const std::function<bool()> &cancelled = {});

// What one filter did to one chunk: how many records it had, how many the share asked for,
// and how many went. Fewer went than were asked when the protection kept the rest.
struct PruneStats {
    size_t records = 0, asked = 0, removed = 0;
    bool limited() const { return removed < asked; }
};

// What stays: for each filter, up to its percent of the chunk's records, lowest scores first,
// among those scoring below its protection threshold. `stats`, when given, gets one entry
// per filter.
std::vector<uint8_t> pruneKeep(const std::vector<float> &scores, const QVector<PruneFilter> &filters,
                               std::vector<PruneStats> *stats = nullptr);

// Both steps for one chunk, reading its frames through `frameAt` (seconds).
std::vector<uint8_t> pruneChunk(double start, double end, const std::function<vgs::Frame(double)> &frameAt,
                                bool antialiased, const QVector<PruneFilter> &filters,
                                const std::function<bool()> &cancelled = {}, std::vector<PruneStats> *stats = nullptr);
