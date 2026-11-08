// SPDX-License-Identifier: LicenseRef-VGS-Decoder-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of VGS Viewer, supplied under the VGS Decoder licence (decoder/LICENSE.md).

#pragma once
#include "captureworker.h"
#include "playbackahead.h"
#include <QElapsedTimer>
#include <QMainWindow>
#include <QThread>
#include <QTimer>

class AudioPreview;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QMenu;
class QPushButton;
class RangeSlider;
class Viewport;

// Opens a .vgs or .pgs and plays it: the editor's viewport and timeline, nothing that edits.
class ViewerWindow : public QMainWindow {
    Q_OBJECT
public:
    ViewerWindow();
    ~ViewerWindow() override;
    void openFile(const QString &path);
signals:
    void openRequested(const QString &path, quint64 generation, bool sh);
    void decodeRequested(double time, quint64 generation, bool sh, Project project);
protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    void play(bool playing);
    void setTime(double seconds);
    // Shows the frame at the playhead. From the playback clock it only moves on; from anywhere
    // else frames decoded ahead are dropped.
    void requestFrame();
    // Playback decodes ahead of the clock (PlaybackAhead), one request with the worker at a time.
    double playbackTime(double elapsed, bool *finished = nullptr) const;
    int currentFrame() const;
    int frameAt(double seconds) const;
    std::vector<PlaybackAhead::Upcoming> upcomingFrames() const;
    double displaySeconds() const;
    bool currentFrameShown() const;
    void showOrDecodeFrame();
    void issueDecode(int frame, bool ahead);
    void scheduleAhead();
    void syncTime();
    void updateRecent();
    void showInfo();
    void syncSound();
    double last() const { return info_.frames > 0 ? (info_.frames - 1) / info_.fps : 0; }

    Viewport *viewport_;
    RangeSlider *slider_;
    QLabel *time_;
    QPushButton *playButton_, *sound_;
    QWidget *displayControls_;
    bool muted_ = false;
    QCheckBox *loop_;
    QDoubleSpinBox *speed_;
    QMenu *recent_;
    AudioPreview *audio_;
    CaptureWorker *worker_;
    QThread thread_;
    QTimer playback_;
    QElapsedTimer clock_;
    // When the playback clock last ticked, in seconds of playback: what the playhead shows. A
    // held frame is due by that, not by the clock, which may be a frame further on.
    double tickSeconds_ = 0;
    CaptureInfo info_;
    quint64 generation_ = 0;
    bool loaded_ = false, decoding_ = false, pending_ = false;
    PlaybackAhead ahead_;
    bool playbackTick_ = false;
    // The frame on screen; and the clock's frame it stands for, which is another when the
    // clock's tick fell past a frame decoded for it. Both of the epoch shownEpoch_.
    int shownFrame_ = -1, coveredFrame_ = -1;
    quint64 shownEpoch_ = 0;
    void shown(int frame, quint64 epoch, int covered = -1) {shownFrame_=frame;coveredFrame_=covered<0 ? frame : covered;shownEpoch_=epoch;}
    // Requests with the worker, oldest first; it answers them in order.
    struct Request { int frame; quint64 epoch; bool ahead; QElapsedTimer clock; };
    std::deque<Request> requests_;
    double time_s_ = 0, playStart_ = 0;
};
