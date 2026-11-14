// SPDX-License-Identifier: LicenseRef-VGS-Decoder-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of VGS Viewer, supplied under the VGS Decoder licence (decoder/LICENSE.md).

#include "viewerwindow.h"
#include "audiopreview.h"
#include "rangeslider.h"
#include "viewport.h"
#include "viewerversion.h"
#include "vgsdecoder/vgsdecoder.h"
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSettings>
#include <QSignalBlocker>
#include <QShortcut>
#include <QStyle>
#include <QPainter>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScreen>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
// The editor's transport icons: the style's symbols tinted light, grey when disabled.
QIcon transportIcon(QStyle *style,QStyle::StandardPixmap symbol) {
    QIcon icon;
    for (auto mode:{QIcon::Normal,QIcon::Disabled}) {
        QPixmap pixmap=style->standardIcon(symbol).pixmap(24,24);
        QPainter painter(&pixmap);painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(),mode==QIcon::Disabled ? QColor("#777777") : QColor("#eeeeee"));painter.end();
        icon.addPixmap(pixmap,mode);
    }
    return icon;
}
bool supported(const QString &path) {const auto s=QFileInfo(path).suffix().toLower();return s=="vgs" || s=="pgs" || s=="sgs";}
}

ViewerWindow::ViewerWindow() : worker_(new CaptureWorker) {
    qRegisterMetaType<FramePtr>();qRegisterMetaType<CaptureInfo>();qRegisterMetaType<Project>();
    setWindowTitle(tr("VGS Viewer %1").arg(VIEWER_VERSION_NAME));setAcceptDrops(true);resize(1280,800);
    auto *center=new QWidget;auto *layout=new QVBoxLayout(center);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    viewport_=new Viewport;viewport_->setViewerMode(true);viewport_->setSplatRendering(true);viewport_->setSplatShDegree(-1);
    layout->addWidget(viewport_,1);
    // The editor's transport and timeline; the range selects the playback section.
    auto *timeline=new QWidget;timeline->setObjectName("timeline");auto *tl=new QVBoxLayout(timeline);tl->setContentsMargins(18,8,18,12);tl->setSpacing(6);
    auto *row=new QHBoxLayout;
    auto button=[&](QStyle::StandardPixmap icon,const QString &tip,auto action) {
        auto *b=new QPushButton;b->setIcon(transportIcon(style(),icon));b->setToolTip(tip);b->setFixedSize(34,30);connect(b,&QPushButton::clicked,this,action);row->addWidget(b);return b;};
    button(QStyle::SP_MediaSkipBackward,tr("Playback range start (Home)"),[this] {play(false);setTime(slider_->startValue()/info_.fps);});
    button(QStyle::SP_MediaSeekBackward,tr("Previous frame (Left)"),[this] {play(false);setTime(time_s_-1/info_.fps);});
    playButton_=button(QStyle::SP_MediaPlay,tr("Play / pause (Space)"),[this] {play(!playback_.isActive());});playButton_->setObjectName("playPause");
    button(QStyle::SP_MediaSeekForward,tr("Next frame (Right)"),[this] {play(false);setTime(time_s_+1/info_.fps);});
    button(QStyle::SP_MediaSkipForward,tr("Playback range end (End)"),[this] {play(false);setTime(slider_->endValue()/info_.fps);});
    time_=new QLabel;row->addSpacing(12);row->addWidget(time_);row->addStretch();
    loop_=new QCheckBox(tr("Loop"));loop_->setObjectName("playbackLoop");loop_->setChecked(true);row->addWidget(loop_);row->addWidget(new QLabel(tr("Speed")));
    speed_=new QDoubleSpinBox;speed_->setObjectName("playbackSpeed");speed_->setRange(0.1,4);speed_->setSingleStep(0.25);speed_->setValue(1);speed_->setSuffix(" ×");row->addWidget(speed_);
    tl->addLayout(row);
    slider_=new RangeSlider;slider_->setObjectName("captureRangeSlider");slider_->setLabelsInSeconds(true);tl->addWidget(slider_);
    layout->addWidget(timeline);setCentralWidget(center);
    connect(slider_,&RangeSlider::playheadChanged,this,[this](int frame) {play(false);setTime(frame/info_.fps);});
    connect(slider_,&RangeSlider::rangeChanged,this,[this](int,int,int preview) {
        if (!loaded_) return;
        play(false);setTime(preview/info_.fps);
    });
    connect(speed_,&QDoubleSpinBox::valueChanged,this,[this](double) {
        if (playback_.isActive()) {
            // A new clock: held frames were timed by the old one. The frame on screen stays.
            const bool covered=currentFrameShown();ahead_.invalidate();if (covered) shown(shownFrame_,ahead_.epoch(),coveredFrame_);
            playStart_=time_s_;clock_.restart();tickSeconds_=0;
        }
        viewport_->setPlaybackRate(playback_.isActive(),info_.fps*speed_->value());
    });
    // Menus: open and recent files; the floor aids; the capture's information.
    auto *file=menuBar()->addMenu(tr("File"));
    file->addAction(tr("Open…"),QKeySequence::Open,this,[this] {
        QSettings settings;
        const QString last=settings.value("Viewer/LastFile").toString();
        const QString initial=QFileInfo::exists(last) ? last : settings.value("Viewer/Directory").toString();
        const QString path=QFileDialog::getOpenFileName(this,tr("Open capture"),initial,tr("VGS captures (*.vgs *.pgs *.sgs)"));
        if (!path.isEmpty()) openFile(path);});
    recent_=file->addMenu(tr("Open recent"));updateRecent();
    file->addAction(tr("Capture information…"),QKeySequence("Ctrl+I"),this,&ViewerWindow::showInfo);
    file->addSeparator();file->addAction(tr("Exit"),QKeySequence::Quit,this,&QWidget::close);
    auto *view=menuBar()->addMenu(tr("View"));QSettings settings;
    auto toggle=[&](const QString &name,const QString &key,bool fallback,void (Viewport::*set)(bool)) {
        auto *a=view->addAction(name);a->setCheckable(true);const bool on=settings.value(key,fallback).toBool();a->setChecked(on);(viewport_->*set)(on);
        connect(a,&QAction::toggled,this,[this,key,set](bool value) {QSettings().setValue(key,value);(viewport_->*set)(value);});return a;};
    toggle(tr("Grid"),"Display/Grid",true,&Viewport::setGrid)->setShortcut(QKeySequence("Shift+G"));
    toggle(tr("Axes"),"Display/Axes",false,&Viewport::setAxes);
    toggle(tr("Origin and front marker"),"Display/FrontMarker",true,&Viewport::setFrontMarker);
    auto *help=menuBar()->addMenu(tr("Help"));
    help->addAction(tr("About VGS Viewer"),this,[this] {QMessageBox::about(this,tr("About VGS Viewer"),tr("<h2>VGS Viewer</h2><p>Version %1</p><p>Víctor M. Feliz</p><p>The4DScanner · ScanMeNow</p>").arg(VIEWER_VERSION_NAME));});
    help->addAction(tr("About Qt"),qApp,&QApplication::aboutQt);
    auto key=[&](const QKeySequence &sequence,auto action) {auto *s=new QShortcut(sequence,this);connect(s,&QShortcut::activated,this,action);};
    key(Qt::Key_Space,[this] {play(!playback_.isActive());});
    key(Qt::Key_Left,[this] {play(false);setTime(time_s_-1/info_.fps);});
    key(Qt::Key_Right,[this] {play(false);setTime(time_s_+1/info_.fps);});
    key(Qt::Key_Home,[this] {play(false);setTime(slider_->startValue()/info_.fps);});
    key(Qt::Key_End,[this] {play(false);setTime(slider_->endValue()/info_.fps);});
    audio_=new AudioPreview(this);
    // Under the background button, when the capture has sound: on/off.
    displayControls_=new QWidget;auto *controls=new QVBoxLayout(displayControls_);controls->setContentsMargins(0,0,0,0);
    sound_=new QPushButton;sound_->setToolTip(tr("Sound on/off"));sound_->setFixedHeight(26);controls->addWidget(sound_);
    connect(sound_,&QPushButton::clicked,this,[this] {muted_=!muted_;syncSound();viewport_->setFocus();});
    viewport_->setDisplayControls(displayControls_);syncSound();
    // Playback follows the clock; a frame is decoded whenever the worker is free.
    playback_.setInterval(8);
    playback_.setTimerType(Qt::PreciseTimer);
    connect(&playback_,&QTimer::timeout,this,[this] {
        bool finished=false;tickSeconds_=clock_.nsecsElapsed()/1e9;const double t=playbackTime(tickSeconds_,&finished);
        {const QScopedValueRollback<bool> tick(playbackTick_,true);setTime(t);}
        if (finished) {play(false);return;}
        scheduleAhead();});
    // Chunks go to the GPU once and the shader evaluates each frame (CaptureWorker::
    // setPackedPlayback); VGS_VIEWER_FLOATS=1 plays the floats way, as the editor does.
    worker_->setPackedPlayback(!qEnvironmentVariableIsSet("VGS_VIEWER_FLOATS"));
    connect(viewport_,&Viewport::packedRenderingFailed,this,[this] {
        QMetaObject::invokeMethod(worker_,[worker=worker_] {worker->setPackedPlayback(false);});
        requestFrame();});
    worker_->moveToThread(&thread_);connect(&thread_,&QThread::finished,worker_,&QObject::deleteLater);
    connect(this,&ViewerWindow::openRequested,worker_,&CaptureWorker::open);
    connect(this,&ViewerWindow::decodeRequested,worker_,&CaptureWorker::decode);
    connect(worker_,&CaptureWorker::opened,this,[this](CaptureInfo info,FramePtr frame,quint64 gen) {
        if (gen!=generation_) return;
        info_=info;loaded_=true;decoding_=pending_=false;requests_.clear();time_s_=0;
        ahead_.invalidate();shown(0,ahead_.epoch());
        QSettings().setValue("Viewer/LastFile",info.path);
        viewport_->setSplatAntialiasing(info.antialiased);viewport_->fit(info.minimum,info.maximum);viewport_->setFrame(frame);
        {
            QSignalBlocker blocker(slider_);
            slider_->setFrameRate(info.fps);slider_->setFrameRange(0,std::max(0,info.frames-1));
            slider_->setRangeValues(0,std::max(0,info.frames-1));slider_->resetView();
        }
        if (!info.audio.isEmpty()) audio_->setBytes(info.audio,info.audioSuffix);else audio_->clear();
        syncSound();
        setWindowTitle(tr("%1 — VGS Viewer %2").arg(info.title.isEmpty() ? QFileInfo(info.path).fileName() : info.title,VIEWER_VERSION_NAME));
        syncTime();play(false);});
    connect(worker_,&CaptureWorker::decoded,this,[this](FramePtr frame,quint64 gen) {
        // The worker answers in order: this is the oldest request.
        if (gen!=generation_ || requests_.empty()) return;
        const auto answered=requests_.front();requests_.pop_front();decoding_=!requests_.empty();
        if (playback_.isActive()) ahead_.decoded(answered.clock.nsecsElapsed()/1e9);
        if (!answered.ahead) {shown(answered.frame,answered.epoch);viewport_->setFrame(frame);}
        else if (playback_.isActive() && answered.epoch==ahead_.epoch()) {
            // Shown now if the clock has reached it, held if it is still to come. One that comes
            // late is shown only over a frame from before an edit, which it is newer than.
            if (answered.frame==currentFrame() || (!ahead_.store(answered.frame,answered.epoch,frame,upcomingFrames()) && shownEpoch_!=ahead_.epoch())) {
                if (answered.frame==currentFrame()) ahead_.shownAt(tickSeconds_);
                shown(answered.frame,answered.epoch);viewport_->setFrame(frame);
            }
        }
        if (pending_ && !decoding_) {pending_=false;showOrDecodeFrame();}
        scheduleAhead();});
    connect(worker_,&CaptureWorker::failed,this,[this](QString,quint64 gen,bool) {
        if (gen!=generation_) return;
        // One message for every reason - not authentic, damaged, unreadable - so it says
        // nothing about which check failed.
        // That frame cannot be shown: stopping does not ask for it again.
        decoding_=pending_=false;requests_.clear();shown(currentFrame(),ahead_.epoch());
        play(false);QMessageBox::warning(this,tr("VGS Viewer"),tr("This file cannot be opened."));});
    thread_.start();
}
ViewerWindow::~ViewerWindow() {play(false);thread_.quit();thread_.wait();}

void ViewerWindow::openFile(const QString &path) {
    if (!supported(path)) {QMessageBox::warning(this,tr("VGS Viewer"),tr("This file cannot be opened."));return;}
    play(false);loaded_=false;audio_->clear();
    QSettings settings;settings.setValue("Viewer/Directory",QFileInfo(path).absolutePath());
    QStringList recent=settings.value("Viewer/Recent").toStringList();recent.removeAll(path);recent.prepend(path);settings.setValue("Viewer/Recent",recent.mid(0,10));updateRecent();
    emit openRequested(QFileInfo(path).absoluteFilePath(),++generation_,true);
}
void ViewerWindow::updateRecent() {
    recent_->clear();const auto recent=QSettings().value("Viewer/Recent").toStringList();
    for (const auto &path:recent) recent_->addAction(QFileInfo(path).fileName(),this,[this,path] {openFile(path);})->setToolTip(path);
    recent_->setEnabled(!recent.isEmpty());
}
void ViewerWindow::play(bool playing) {
    if (playing && !loaded_) return;
    if (playing) {
        const double rangeStart=slider_->startValue()/info_.fps;
        const double rangeEnd=slider_->endValue()/info_.fps;
        if (time_s_<rangeStart || time_s_>=rangeEnd) setTime(rangeStart);
        playStart_=time_s_;clock_.restart();tickSeconds_=0;playback_.start();
    } else {
        // Frames decoded ahead go. The frame on screen stays; when the clock had reached one
        // still being decoded, it is asked for again.
        // What is on screen, not the frame it stood in for.
        const bool wasPlaying=playback_.isActive(),onScreen=shownFrame_==currentFrame() && shownEpoch_==ahead_.epoch();
        playback_.stop();ahead_.invalidate();
        if (onScreen) shown(shownFrame_,ahead_.epoch());
        else if (wasPlaying) showOrDecodeFrame();
    }
    viewport_->setPlaybackRate(playing,info_.fps*speed_->value());
    playButton_->setIcon(transportIcon(style(),playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    syncTime();
}
void ViewerWindow::setTime(double seconds) {
    if (!loaded_) return;
    const double t=std::clamp(seconds,slider_->startValue()/info_.fps,slider_->endValue()/info_.fps);
    const bool frameChanged=std::lround(t*info_.fps)!=std::lround(time_s_*info_.fps);
    time_s_=t;
    if (frameChanged) {syncTime();requestFrame();}
    else if (!playback_.isActive()) syncTime();
}
void ViewerWindow::requestFrame() {
    if (!playbackTick_) ahead_.invalidate();
    showOrDecodeFrame();
}
double ViewerWindow::playbackTime(double elapsed,bool *finished) const {
    if (finished) *finished=false;
    double t=playStart_+elapsed*speed_->value();
    const double rangeStart=slider_->startValue()/info_.fps;
    const double rangeEnd=slider_->endValue()/info_.fps;
    if (t>rangeEnd) {
        if (loop_->isChecked() && rangeEnd>rangeStart) t=rangeStart+std::fmod(t-rangeStart,rangeEnd-rangeStart+1.0/info_.fps);
        else {if (finished) *finished=true;return rangeEnd;}
    }
    return t;
}
int ViewerWindow::currentFrame() const {return int(std::lround(time_s_*info_.fps));}
// The frame setTime() shows for that time.
int ViewerWindow::frameAt(double seconds) const {
    return int(std::lround(std::clamp(seconds,slider_->startValue()/info_.fps,slider_->endValue()/info_.fps)*info_.fps));
}
std::vector<PlaybackAhead::Upcoming> ViewerWindow::upcomingFrames() const {
    const double now=clock_.nsecsElapsed()/1e9;
    if (!playback_.isActive() || !(info_.fps>0) || !(speed_->value()>0)) return {{currentFrame(),now}};
    const double frameSeconds=1.0/(info_.fps*speed_->value());
    return PlaybackAhead::upcoming(currentFrame(),now,frameSeconds,[this](double elapsed)->std::optional<int> {
        bool finished=false;const double t=playbackTime(elapsed,&finished);
        if (finished) return std::nullopt;
        return frameAt(t);
    },ahead_.lookahead(frameSeconds,PlaybackAhead::spacing(frameSeconds,displaySeconds())));
}
// One refresh of the screen the viewport is on.
double ViewerWindow::displaySeconds() const {
    const QScreen *screen=viewport_->screen();
    return screen && screen->refreshRate()>1 ? 1.0/screen->refreshRate() : 1.0/60;
}
bool ViewerWindow::currentFrameShown() const {return coveredFrame_==currentFrame() && shownEpoch_==ahead_.epoch();}
void ViewerWindow::showOrDecodeFrame() {
    if (!loaded_) return;
    const int frame=currentFrame();
    if (playback_.isActive()) {
        // Playing, what to decode is the scheduler's: the frame the clock will be at when it is
        // decoded, which is this one only when decoding keeps up.
        if (currentFrameShown()) return;
        int taken=frame;
        if (auto held=ahead_.take(frame,tickSeconds_,&taken)) {shown(taken,ahead_.epoch(),frame);viewport_->setFrame(std::move(held));return;}
        scheduleAhead();return;
    }
    if (decoding_) {pending_=true;return;}
    issueDecode(frame,false);
}
// While playing: the next frames the clock will show, up to two requests with the worker.
void ViewerWindow::scheduleAhead() {
    if (!playback_.isActive() || !loaded_) return;
    // A request left from before playing is the scheduler's now.
    pending_=false;
    const double frameSeconds=info_.fps>0 && speed_->value()>0 ? 1.0/(info_.fps*speed_->value()) : 0;
    const double spacing=PlaybackAhead::spacing(frameSeconds,displaySeconds());
    while (int(requests_.size())<PlaybackAhead::MaxRequests) {
        std::vector<int> decoding;for (const auto &r:requests_) if (r.epoch==ahead_.epoch()) decoding.push_back(r.frame);
        const auto next=ahead_.next(upcomingFrames(),clock_.nsecsElapsed()/1e9,currentFrameShown(),decoding,spacing);
        if (!next) break;
        issueDecode(*next,true);
    }
}
void ViewerWindow::issueDecode(int frame,bool ahead) {
    // No modifiers: the capture as it is, at its full SH. The range and loop let the worker
    // read the right chunk ahead at a loop's wrap.
    Project plain;plain.modifiers.clear();
    plain.previewPlaying=playback_.isActive();plain.loop=loop_->isChecked();
    plain.in=slider_->startValue()/info_.fps;plain.out=slider_->endValue()/info_.fps;
    Request request{frame,ahead_.epoch(),ahead,{}};request.clock.start();
    requests_.push_back(request);decoding_=true;
    emit decodeRequested(frame/info_.fps,generation_,true,plain);
}
void ViewerWindow::syncTime() {
    const int frame=int(std::lround(time_s_*info_.fps));
    {QSignalBlocker blocker(slider_);slider_->setPlayheadValue(frame);}
    time_->setText(tr("%1 s of %2 s  ·  frame %3").arg(time_s_,0,'f',3).arg(last(),0,'f',3).arg(frame));
    audio_->follow(time_s_+info_.startSeconds,playback_.isActive(),speed_->value());
}
void ViewerWindow::syncSound() {
    const bool sound=loaded_ && !info_.audio.isEmpty();
    sound_->setVisible(sound);displayControls_->setMaximumHeight(sound ? QWIDGETSIZE_MAX : 0);
    viewport_->setDisplayControls(displayControls_);
    sound_->setIcon(transportIcon(style(),muted_ ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
    audio_->setVolume(muted_ ? 0 : 1);
}
void ViewerWindow::showInfo() {
    if (!loaded_) return;
    QStringList lines;
    auto add=[&](const QString &label,const QString &value) {if (!value.trimmed().isEmpty()) lines << label+": "+value.trimmed();};
    try {
        const auto m=vgsdec::Capture::openFile(info_.path.toStdString()).metadata();
        auto text=[](const std::string &value) {return QString::fromStdString(value);};
        add(tr("Title"),text(m.title));add(tr("Author"),text(m.author));add(tr("Project"),text(m.projectName));add(tr("Take"),text(m.takeName));
        add(tr("Capture studio"),text(m.captureStudio));add(tr("Copyright"),text(m.copyright));add(tr("Catalog ID"),text(m.id));
        add(tr("Software"),(text(m.softwareName)+" "+text(m.softwareVersion)).trimmed());
        QStringList tags;for (const auto &tag:m.tags) tags << text(tag);add(tr("Tags"),tags.join(", "));
    } catch (const std::exception &) {}
    if (!lines.isEmpty()) lines << QString();
    add(tr("File"),QFileInfo(info_.path).fileName());
    if (info_.shDegree>=0) add(tr("SH degree"),tr("SH%1").arg(info_.shDegree));
    add(tr("Duration"),tr("%1 s \u00b7 %2 frames at %3 fps").arg(info_.duration,0,'f',3).arg(info_.frames).arg(info_.fps,0,'f',2));
    add(tr("Size"),tr("%1 MB").arg(QFileInfo(info_.path).size()/1e6,0,'f',1));
    if (!info_.audio.isEmpty()) add(tr("Audio"),info_.audioSuffix.toUpper());
    QMessageBox::information(this,tr("Capture information"),lines.join("\n"));
}
void ViewerWindow::dragEnterEvent(QDragEnterEvent *event) {
    const auto urls=event->mimeData()->urls();if (!urls.isEmpty() && supported(urls.first().toLocalFile())) event->acceptProposedAction();
}
void ViewerWindow::dropEvent(QDropEvent *event) {openFile(event->mimeData()->urls().first().toLocalFile());}
