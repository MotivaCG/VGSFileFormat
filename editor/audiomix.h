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
#include <QByteArray>
#include <QString>
#include <QVector>
#include <functional>

// One track in an export's soundtrack. `start` is how far into the track the soundtrack
// begins, in seconds; negative, the track comes in that long after it. A looped track
// repeats from its beginning for as long as the soundtrack lasts.
struct AudioInput {
    QString path;
    double start = 0, volume = 1;
    bool loop = false;
};

// The ffmpeg the export uses: VGS_FFMPEG when set, otherwise tools/ffmpeg beside the program.
// Empty when there is none.
QString ffmpegPath();

// The tracks mixed, each at its volume, into exactly `seconds` of AAC in an .m4a (silence
// where none plays). Throws std::runtime_error on failure or when `cancelled` says so.
QByteArray mixAudioAac(const QVector<AudioInput> &inputs, double seconds, const std::function<bool()> &cancelled = {});
