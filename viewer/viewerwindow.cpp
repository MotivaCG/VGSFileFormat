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
#include <QShortcut>
#include <QStyle>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>

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
bool supported(const QString &path) {const auto s=QFileInfo(path).suffix().toLower();return s=="vgs" || s=="pgs";}
}

ViewerWindow::ViewerWindow() : worker_(new CaptureWorker) {
    qRegisterMetaType<FramePtr>();qRegisterMetaType<CaptureInfo>();qRegisterMetaType<Project>();
    setWindowTitle(tr("VGS Viewer %1").arg(VIEWER_VERSION_NAME));setAcceptDrops(true);resize(1280,800);
    auto *center=new QWidget;auto *layout=new QVBoxLayout(center);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    viewport_=new Viewport;viewport_->setViewerMode(true);viewport_->setSplatRendering(true);viewport_->setSplatShDegree(-1);
    layout->addWidget(viewport_,1);
    // The editor's transport and timeline, without trimming: the range is the whole file.
    auto *timeline=new QWidget;timeline->setObjectName("timeline");auto *tl=new QVBoxLayout(timeline);tl->setContentsMargins(18,8,18,12);tl->setSpacing(6);
    auto *row=new QHBoxLayout;
    auto button=[&](QStyle::StandardPixmap icon,const QString &tip,auto action) {
        auto *b=new QPushButton;b->setIcon(transportIcon(style(),icon));b->setToolTip(tip);b->setFixedSize(34,30);connect(b,&QPushButton::clicked,this,action);row->addWidget(b);return b;};
    button(QStyle::SP_MediaSkipBackward,tr("First frame (Home)"),[this] {play(false);setTime(0);});
    button(QStyle::SP_MediaSeekBackward,tr("Previous frame (Left)"),[this] {play(false);setTime(time_s_-1/info_.fps);});
    playButton_=button(QStyle::SP_MediaPlay,tr("Play / pause (Space)"),[this] {play(!playback_.isActive());});
    button(QStyle::SP_MediaSeekForward,tr("Next frame (Right)"),[this] {play(false);setTime(time_s_+1/info_.fps);});
    button(QStyle::SP_MediaSkipForward,tr("Last frame (End)"),[this] {play(false);setTime(last());});
    time_=new QLabel;row->addSpacing(12);row->addWidget(time_);row->addStretch();
    loop_=new QCheckBox(tr("Loop"));loop_->setChecked(true);row->addWidget(loop_);row->addWidget(new QLabel(tr("Speed")));
    speed_=new QDoubleSpinBox;speed_->setRange(0.1,4);speed_->setSingleStep(0.25);speed_->setValue(1);speed_->setSuffix(" ×");row->addWidget(speed_);
    tl->addLayout(row);
    slider_=new RangeSlider;slider_->setObjectName("captureRangeSlider");slider_->setLabelsInSeconds(true);tl->addWidget(slider_);
    layout->addWidget(timeline);setCentralWidget(center);
    connect(slider_,&RangeSlider::playheadChanged,this,[this](int frame) {play(false);setTime(frame/info_.fps);});
    connect(slider_,&RangeSlider::rangeChanged,this,[this](int,int,int) {slider_->setRangeValues(0,std::max(0,info_.frames-1));});
    connect(speed_,&QDoubleSpinBox::valueChanged,this,[this](double) {if (playback_.isActive()) {playStart_=time_s_;clock_.restart();}});
    // Menus: open and recent files; the floor aids; the capture's information.
    auto *file=menuBar()->addMenu(tr("File"));
    file->addAction(tr("Open…"),QKeySequence::Open,this,[this] {
        QSettings settings;const QString path=QFileDialog::getOpenFileName(this,tr("Open capture"),settings.value("Viewer/Directory").toString(),tr("VGS captures (*.vgs *.pgs)"));
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
    key(Qt::Key_Home,[this] {play(false);setTime(0);});
    key(Qt::Key_End,[this] {play(false);setTime(last());});
    audio_=new AudioPreview(this);
    // Under the background button, when the capture has sound: on/off.
    displayControls_=new QWidget;auto *controls=new QVBoxLayout(displayControls_);controls->setContentsMargins(0,0,0,0);
    sound_=new QPushButton;sound_->setToolTip(tr("Sound on/off"));sound_->setFixedHeight(26);controls->addWidget(sound_);
    connect(sound_,&QPushButton::clicked,this,[this] {muted_=!muted_;syncSound();viewport_->setFocus();});
    viewport_->setDisplayControls(displayControls_);syncSound();
    // Playback follows the clock; a frame is decoded whenever the worker is free.
    playback_.setInterval(8);
    connect(&playback_,&QTimer::timeout,this,[this] {
        double t=playStart_+clock_.elapsed()/1000.0*speed_->value();
        if (t>last()) {if (loop_->isChecked()) {t=0;playStart_=0;clock_.restart();} else {setTime(last());play(false);return;}}
        setTime(t);});
    worker_->moveToThread(&thread_);connect(&thread_,&QThread::finished,worker_,&QObject::deleteLater);
    connect(this,&ViewerWindow::openRequested,worker_,&CaptureWorker::open);
    connect(this,&ViewerWindow::decodeRequested,worker_,&CaptureWorker::decode);
    connect(worker_,&CaptureWorker::opened,this,[this](CaptureInfo info,FramePtr frame,quint64 gen) {
        if (gen!=generation_) return;
        info_=info;loaded_=true;decoding_=pending_=false;time_s_=0;
        viewport_->setSplatAntialiasing(info.antialiased);viewport_->fit(info.minimum,info.maximum);viewport_->setFrame(frame);
        slider_->setFrameRate(info.fps);slider_->setFrameRange(0,std::max(0,info.frames-1));slider_->setRangeValues(0,std::max(0,info.frames-1));slider_->resetView();
        if (!info.audio.isEmpty()) audio_->setBytes(info.audio,info.audioSuffix);else audio_->clear();
        syncSound();
        setWindowTitle(tr("%1 — VGS Viewer %2").arg(info.title.isEmpty() ? QFileInfo(info.path).fileName() : info.title,VIEWER_VERSION_NAME));
        syncTime();play(true);});
    connect(worker_,&CaptureWorker::decoded,this,[this](FramePtr frame,quint64 gen) {
        if (gen!=generation_) return;
        decoding_=false;viewport_->setFrame(frame);if (pending_) {pending_=false;requestFrame();}});
    connect(worker_,&CaptureWorker::failed,this,[this](QString,quint64 gen,bool) {
        if (gen!=generation_) return;
        // One message for every reason - not authentic, damaged, unreadable - so it says
        // nothing about which check failed.
        play(false);decoding_=false;QMessageBox::warning(this,tr("VGS Viewer"),tr("This file cannot be opened."));});
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
    if (playing) {if (time_s_>=last()) time_s_=0;playStart_=time_s_;clock_.restart();playback_.start();} else playback_.stop();
    playButton_->setIcon(transportIcon(style(),playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    syncTime();
}
void ViewerWindow::setTime(double seconds) {
    if (!loaded_) return;
    const double t=std::clamp(seconds,0.0,last());
    const bool frameChanged=std::lround(t*info_.fps)!=std::lround(time_s_*info_.fps);
    time_s_=t;syncTime();if (frameChanged) requestFrame();
}
void ViewerWindow::requestFrame() {
    if (!loaded_) return;
    if (decoding_) {pending_=true;return;}
    // No modifiers: the capture as it is, at its full SH.
    Project plain;plain.modifiers.clear();decoding_=true;
    emit decodeRequested(std::round(time_s_*info_.fps)/info_.fps,generation_,true,plain);
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
    add(tr("Duration"),tr("%1 s \u00b7 %2 frames at %3 fps").arg(info_.duration,0,'f',3).arg(info_.frames).arg(info_.fps,0,'f',2));
    add(tr("Size"),tr("%1 MB").arg(QFileInfo(info_.path).size()/1e6,0,'f',1));
    if (!info_.audio.isEmpty()) add(tr("Audio"),info_.audioSuffix.toUpper());
    QMessageBox::information(this,tr("Capture information"),lines.join("\n"));
}
void ViewerWindow::dragEnterEvent(QDragEnterEvent *event) {
    const auto urls=event->mimeData()->urls();if (!urls.isEmpty() && supported(urls.first().toLocalFile())) event->acceptProposedAction();
}
void ViewerWindow::dropEvent(QDropEvent *event) {openFile(event->mimeData()->urls().first().toLocalFile());}
