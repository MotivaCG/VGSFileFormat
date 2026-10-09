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
#include <QObject>
#include <QString>

class QAudioOutput;
class QBuffer;
class QMediaPlayer;

// The Audio modifier's track, played along with the timeline. It follows where the timeline
// is rather than keeping its own clock: while playing it starts, keeps in step and corrects
// drift; paused it only moves to the spot. Before the track starts and after it ends it is
// silent.
class AudioPreview : public QObject {
public:
    explicit AudioPreview(QObject *parent = nullptr);
    ~AudioPreview() override;
    // What to play: a file, or a track's bytes with the suffix its format goes by. Setting the
    // one already set changes nothing.
    void setFile(const QString &path);
    void setBytes(const QByteArray &bytes, const QString &suffix);
    void clear();
    bool hasTrack() const { return !key_.isEmpty(); }
    // `seconds` into the track, playing or not, at the timeline's speed.
    void follow(double seconds, bool playing, double rate);
    bool isPlaying() const;
private:
    QMediaPlayer *player_;
    QAudioOutput *output_;
    QBuffer *buffer_ = nullptr;
    QByteArray bytes_;
    QString key_;
};
