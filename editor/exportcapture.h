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
#include <array>

struct ExportResult {
    int frames = 0;
    quint64 kept = 0, removed = 0;
    QStringList notes;
    // Prune low contribution over the source chunks it scored: their records, what its share
    // asked for, what went, and in how many chunks the protection kept more than the share.
    quint64 pruneRecords = 0, pruneAsked = 0, pruneRemoved = 0;
    int pruneChunks = 0, pruneLimited = 0;
};
using ExportProgress = std::function<bool(int, const QString &)>;

// Source attributes are immutable. Crop tests transformed means in world space.
// `pruned`, when given, says which records Prune low contribution keeps (pruning.h).
vgs::Frame bakeExportFrame(const vgs::Frame &, const Project &, int degree,
                          const ExportProgress & = {},double frameRate = 30,
                          const std::vector<uint8_t> *pruned = nullptr);
// `thumbnailJpeg`, when given, becomes the capture's thumbnail (VGS/PGS); without it a .vgs
// source keeps its own. The soundtrack - every active Audio modifier's track mixed at its
// volume, or with none the source's own - is cut to the range as AAC with ffmpeg (audiomix.h),
// starting with it. Without ffmpeg one track travels as delivered, and startTick says where
// the range starts on it.
ExportResult exportCaptureFile(const Project &, const QString &destination,
                               const ExportProgress & = {}, const QByteArray &thumbnailJpeg = {});

// What a .vgs export of the project would weigh, without making it: a few one-second windows
// spread over the range are exported for real, to a temporary folder, and their chunks
// extrapolated to the whole range; the header, metadata, audio and thumbnail are measured.
// A range of four seconds or less is exported whole, and the figure is exact.
struct ExportEstimate {
    double seconds = 0;     // the range's duration
    double bytes = 0;       // the whole file
    double averageMbps = 0; // the capture's data over its duration
    double peakMbps = 0;    // the heaviest chunk sampled, over its own duration
    int windows = 0;        // how many windows were exported
    bool exact = false;     // the whole range was exported
};
ExportEstimate estimateExportSize(const Project &, const ExportProgress & = {}, const QByteArray &thumbnailJpeg = {});

// One edited instant as a 3D Gaussian Splatting .ply (INRIA convention, binary little
// endian): what Export capture bakes at that time - transform, modifiers, colour
// processing - with the export SH degree's bands in f_rest.
ExportResult exportFramePly(const Project &, double seconds, const QString &destination,
                            const ExportProgress & = {});
void writePly(const vgs::Frame &, const QString &destination);

// The export's despill on one frame's colour (DC and SH), in place: also what the viewport
// shows when a Colour modifier despills always.
void despillFrame(vgs::Frame &, const CaptureSettings &);

// Assemble standard encoding-0 attributes from an already baked native frame.
vgs::DecodedChunk packExportFrame(const vgs::Frame &, int degree);
std::array<double,256> exportShTransform(const Transform &);
