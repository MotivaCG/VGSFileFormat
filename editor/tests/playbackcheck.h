// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Victor M. Feliz. All rights reserved.
//
// What the editor's and the viewer's playback tests share: a small capture to play, and a
// record of every frame the viewport puts on screen with where the playhead was then.
#pragma once

#include "exportcapture.h"
#include "rangeslider.h"
#include "vgssign.h"
#include "viewport.h"
#include <QElapsedTimer>
#include <QFile>
#include <QOpenGLWidget>
#include <QString>
#include <QStringList>
#include <QtTest>
#include <cmath>
#include <vector>

namespace playbackcheck {

constexpr double Fps = 30;

// `frames` frames at 30 fps, one chunk each, a splat moving along x.
inline QString writeCapture(const QString &path, int frames) {
    vgs::Header h; h.shDegree = 0; h.frameCount = h.durationTicks = uint64_t(frames); h.chunks.resize(size_t(frames));
    for (size_t i = 0; i < h.chunks.size(); ++i) { h.chunks[i].startTick = i; h.chunks[i].intervals = 1; }
    vgs::EncodeOptions options; options.signer = vgs::authoringSigner(); options.shDegree = 0; options.compression = vgs::Compression::None;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return file.errorString();
    try {
        vgs::encodeSequence(h, [&](size_t chunk) {
            vgs::Frame f; f.count = 1; f.active = {1}; f.position = {float(chunk) * 0.01f, 1, 0}; f.rotation = {0, 0, 0, 1};
            f.scale = {.05f, .05f, .05f}; f.colorDc = {.5f, .5f, .5f}; f.opacity = {1};
            return packExportFrame(f, 0);
        }, [&](uint64_t offset, const uint8_t *data, size_t count) {
            if (!file.seek(qint64(offset)) || file.write(reinterpret_cast<const char *>(data), qint64(count)) != qint64(count))
                throw std::runtime_error("Cannot write the test capture.");
        }, options);
    } catch (const std::exception &e) { return QString::fromUtf8(e.what()); }
    return {};
}

struct Shown { int frame; int playhead; quint64 revision; qint64 ms; };

// Every frame the viewport presents, as it is presented.
class Watch {
public:
    Watch(Viewport *viewport, const RangeSlider *slider) : viewport_(viewport), slider_(slider) {
        clock_.start();
        connection_ = QObject::connect(viewport, &QOpenGLWidget::frameSwapped, viewport, [this] {
            // By revision: the worker reuses a frame's storage for later frames.
            const auto frame = viewport_->frame();
            if (!frame || viewport_->frameRevision() == revision_) return;
            revision_ = viewport_->frameRevision();
            shown.push_back({int(std::lround(frame->seconds * Fps)), slider_->playheadValue(), frame->cache.revision, clock_.elapsed()});
        });
    }
    ~Watch() { QObject::disconnect(connection_); }
    void clear() { shown.clear(); }
    qint64 now() const { return clock_.elapsed(); }
    std::vector<Shown> shown;
private:
    Viewport *viewport_;
    const RangeSlider *slider_;
    quint64 revision_ = 0;
    QElapsedTimer clock_;
    QMetaObject::Connection connection_;
};

// Frames from `first` to `last`, looping: how far `a` is behind `b`.
inline int behind(int a, int b, int first, int last) {
    const int length = last - first + 1;
    return ((b - a) % length + length) % length;
}

// Each frame is one the clock has reached - never one before its time - within the range, and
// they follow one another in the clock's order, `step` frames apart `share` of the time. Lateness
// beyond `maxLate` frames, or a jump past two of those, is allowed only for a stall of the
// desktop now and then: 2% of the frames, at least one.
inline void checkOnTime(const std::vector<Shown> &shown, int first, int last, int maxLate, int step, double share) {
    QVERIFY2(shown.size() >= 2, "Nothing was played.");
    const int length = last - first + 1;
    int inStep = 0, stalls = 0;
    QStringList stalled;
    for (size_t i = 0; i < shown.size(); ++i) {
        const auto &s = shown[i];
        QVERIFY2(s.frame >= first && s.frame <= last, qPrintable(QString("Frame %1 is outside %2-%3.").arg(s.frame).arg(first).arg(last)));
        const int late = behind(s.frame, s.playhead, first, last);
        // More than half the range behind is, on a loop, ahead of the playhead.
        QVERIFY2(late <= length / 2, qPrintable(QString("Frame %1 shown before its time, with the playhead at %2.").arg(s.frame).arg(s.playhead)));
        bool stall = late > maxLate;
        if (i) {
            const int advance = behind(shown[i - 1].frame, s.frame, first, last);
            QVERIFY2(advance > 0 && advance <= length / 2, qPrintable(QString("Frame %1 followed frame %2.").arg(s.frame).arg(shown[i - 1].frame)));
            stall = stall || advance > std::max(maxLate, step) * 2 + 2;
            if (advance == step) ++inStep;
        }
        if (stall) {++stalls; stalled << QString("%1/%2@%3").arg(s.frame).arg(s.playhead).arg(s.ms);}
    }
    QVERIFY2(stalls <= std::max<int>(1, int(shown.size()) / 50), qPrintable(QString("%1 frames late: %2").arg(stalls).arg(stalled.join(' '))));
    const double inOrder = double(inStep) / double(shown.size() - 1);
    QVERIFY2(inOrder >= share, qPrintable(QString("Only %1% of the frames were %2 apart.").arg(inOrder * 100, 0, 'f', 1).arg(step)));
}

} // namespace playbackcheck
