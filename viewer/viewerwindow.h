// SPDX-License-Identifier: LicenseRef-VGS-Decoder-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of VGS Viewer, supplied under the VGS Decoder licence (decoder/LICENSE.md).

#pragma once
#include "captureworker.h"
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
    void requestFrame();
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
    CaptureInfo info_;
    quint64 generation_ = 0;
    bool loaded_ = false, decoding_ = false, pending_ = false;
    double time_s_ = 0, playStart_ = 0;
};
