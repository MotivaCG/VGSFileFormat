#include "mainwindow.h"
#include "displayscaling.h"
#include "viewport.h"
#include "viewcube.h"
#include "modifierpanel.h"
#include "animationpanel.h"
#include "capturesettingsdialog.h"
#include "exportcapture.h"
#include <QProgressDialog>
#include <atomic>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QButtonGroup>
#include <QComboBox>
#include <QCursor>
#include <QDesktopServices>
#include <QCloseEvent>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QHeaderView>
#include <QTableWidget>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QImage>
#include <QInputDialog>
#include <QLineEdit>
#include <QJsonDocument>
#include <QIcon>
#include <QLabel>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QAbstractItemView>
#include <QHelpEvent>
#include <QToolTip>
#include <QPushButton>
#include <QPixmap>
#include <QPainter>
#include <QHash>
#include <QSettings>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include "rangeslider.h"
#include <QSpinBox>
#include <functional>
#include <QSlider>
#include <QStatusBar>
#include <QStyle>
#include <QStyleOptionButton>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

static QIcon transportIcon(QStyle *style,QStyle::StandardPixmap symbol,const QColor &colour = QColor("#eeeeee")) {
    QIcon icon;
    for (auto mode : {QIcon::Normal,QIcon::Disabled}) {
        QPixmap pixmap = style->standardIcon(symbol).pixmap(24,24);
        QPainter painter(&pixmap); painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(),mode==QIcon::Disabled ? QColor("#777777") : colour); painter.end();
        icon.addPixmap(pixmap,mode);
    }
    return icon;
}
// A system theme icon (the same set as the modifier toolbar), tinted to the editor's greys.
static QIcon themeIcon(QIcon::ThemeIcon name,const QIcon &fallback) {
    const QIcon source=QIcon::fromTheme(name,fallback);QIcon icon;
    for (auto mode:{QIcon::Normal,QIcon::Disabled}) {
        QPixmap pixmap=source.pixmap(20,20);QPainter tint(&pixmap);tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(pixmap.rect(),mode==QIcon::Disabled ? QColor("#666666") : QColor("#e6e6e6"));tint.end();icon.addPixmap(pixmap,mode);
    }
    return icon;
}
static QIcon editorButtonIcon(const QString &resource,bool checkedOnly,const QString &offResource={}) {
    // State variants are generated at runtime; the supplied PNGs remain untouched.
    static QHash<QString,QIcon> cache;
    const QString key = resource+(checkedOnly ? ":toggle:" : ":selection:")+offResource;
    if (cache.contains(key)) return cache.value(key);
    const QPixmap original(resource),off(offResource.isEmpty() ? resource : offResource);
    auto tinted = [&](const QPixmap &source,const QColor &colour) {
        QPixmap result = source; QPainter painter(&result);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn); painter.fillRect(result.rect(),colour);
        painter.end(); return result;
    };
    const QPixmap inactive=tinted(off,QColor("#888888")),disabledOn=tinted(original,QColor("#606060")),disabledOff=tinted(off,QColor("#606060"));
    QIcon icon;
    for (auto mode : {QIcon::Normal,QIcon::Active,QIcon::Selected}) {
        icon.addPixmap(original,mode,QIcon::On);
        icon.addPixmap(checkedOnly ? inactive : off,mode,QIcon::Off);
    }
    icon.addPixmap(disabledOn,QIcon::Disabled,QIcon::On); icon.addPixmap(disabledOff,QIcon::Disabled,QIcon::Off);
    cache.insert(key,icon); return icon;
}

static QIcon timelineClockIcon() {
    // Qt has no StandardPixmap clock; use its icon theme with a vector fallback.
    QPixmap fallback(48,48);fallback.fill(Qt::transparent);
    QPainter painter(&fallback);painter.setRenderHint(QPainter::Antialiasing);painter.setPen(QPen(Qt::white,3,Qt::SolidLine,Qt::RoundCap));
    painter.drawEllipse(QPointF(24,24),18,18);painter.drawLine(QPointF(24,24),QPointF(24,13));painter.drawLine(QPointF(24,24),QPointF(33,28));painter.end();
    const auto source=QIcon::fromTheme("clock",QIcon::fromTheme("preferences-system-time",QIcon(fallback)));
    QIcon result;
    for (auto mode:{QIcon::Normal,QIcon::Active,QIcon::Selected,QIcon::Disabled}) for (auto state:{QIcon::Off,QIcon::On}) {
        auto pixmap=source.pixmap(24,24);QPainter tint(&pixmap);tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(pixmap.rect(),mode==QIcon::Disabled ? QColor("#606060") : state==QIcon::On ? QColor("#eeeeee") : QColor("#888888"));tint.end();result.addPixmap(pixmap,mode,state);
    }
    return result;
}

// Drag horizontally on an axis label to scrub its spin box (Shift fine, Ctrl coarse, Esc cancels).
// A click without dragging focuses the field for typing. The cursor is hidden and warped back
// to the press point so the drag never runs into a screen edge.
// Qt also fires a "1" shortcut from the numpad's 1 when that key finds no shortcut of its
// own. Numpad digits are the viewport's views, so they go to the focused widget as keys.
// A combo that reads "Label: value" while closed, like the Background button and the Point
// size field beside it; the open list shows the plain choices. An item may carry a shorter
// closed form in ClosedTextRole ("2" rather than "SH2").
class PrefixedComboBox : public QComboBox {
public:
    static constexpr int ClosedTextRole = Qt::UserRole+1;
    explicit PrefixedComboBox(const QString &prefix,QWidget *parent=nullptr) : QComboBox(parent),prefix_(prefix) {
        connect(this,&QComboBox::currentIndexChanged,this,[this] {setAccessibleName(closedText());});
    }
    QString closedText() const {
        const auto closed = currentData(ClosedTextRole).toString();
        return prefix_+": "+(closed.isEmpty() ? currentText() : closed);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QStylePainter painter(this);QStyleOptionComboBox option;initStyleOption(&option);option.currentText=closedText();
        painter.drawComplexControl(QStyle::CC_ComboBox,option);painter.drawControl(QStyle::CE_ComboBoxLabel,option);
    }
private:
    QString prefix_;
};
class NumpadDigitsAreNotShortcuts : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *watched,QEvent *event) override {
        if (event->type()==QEvent::ShortcutOverride) {
            const auto *key=static_cast<QKeyEvent *>(event);
            if ((key->modifiers() & Qt::KeypadModifier) && key->key()>=Qt::Key_0 && key->key()<=Qt::Key_9) {event->accept();return true;}
        }
        return QObject::eventFilter(watched,event);
    }
};
class SpinScrubber final : public QObject {
public:
    static void attach(QWidget *label,QDoubleSpinBox *spin,double pixelStep) {
        new SpinScrubber(label,spin,[spin] {return spin->value();},[spin](double v) {spin->setValue(v);},pixelStep);
    }
    // Whole numbers: the drag accumulates fractions and the field takes the nearest one.
    static void attach(QWidget *label,QSpinBox *spin,double pixelStep) {
        new SpinScrubber(label,spin,[spin] {return double(spin->value());},[spin](double v) {spin->setValue(int(std::lround(v)));},pixelStep);
    }
    // Any field, through functions that read and write its value in the units to scrub in.
    static void attach(QWidget *label,QAbstractSpinBox *spin,std::function<double()> get,std::function<void(double)> set,double pixelStep) {
        new SpinScrubber(label,spin,std::move(get),std::move(set),pixelStep);
    }
    // The label of a field laid out by a QFormLayout, if it has one.
    template<class Spin> static void attachFormLabel(Spin *spin,double pixelStep) {
        auto *form=spin->parentWidget() ? qobject_cast<QFormLayout *>(spin->parentWidget()->layout()) : nullptr;
        auto *label=form ? form->labelForField(spin) : nullptr;if (!label) return;
        attach(label,spin,pixelStep);
        spin->setToolTip(spin->toolTip()+(spin->toolTip().isEmpty() ? QString() : QStringLiteral("\n"))+tr("Drag the label to scrub: Shift fine, Ctrl coarse, Esc cancels."));
    }
protected:
    bool eventFilter(QObject *watched,QEvent *event) override {
        auto *label=static_cast<QWidget *>(watched);
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *e=static_cast<QMouseEvent *>(event);
            if (e->button()!=Qt::LeftButton || !spin_->isEnabled()) return false;
            pressed_=true;dragging_=false;accumulated_=0;startValue_=get_();
            pressPos_=lastPos_=e->globalPosition().toPoint();label->grabKeyboard();
            return true;
        }
        case QEvent::MouseMove: {
            if (!pressed_) return false;
            auto *e=static_cast<QMouseEvent *>(event);const QPoint pos=e->globalPosition().toPoint();
            if (!dragging_) {
                if (std::abs(pos.x()-pressPos_.x())<3) return true;
                dragging_=true;label->setCursor(Qt::BlankCursor);
            }
            const double factor=e->modifiers()&Qt::ShiftModifier ? 0.1 : e->modifiers()&Qt::ControlModifier ? 10.0 : 1.0;
            accumulated_+=(pos.x()-lastPos_.x())*pixelStep_*factor;
            set_(startValue_+accumulated_);
            QCursor::setPos(pressPos_);lastPos_=pressPos_;
            return true;
        }
        case QEvent::MouseButtonRelease: {
            if (!pressed_ || static_cast<QMouseEvent *>(event)->button()!=Qt::LeftButton) return false;
            const bool clicked=!dragging_;finish(label);
            if (clicked) {spin_->setFocus(Qt::MouseFocusReason);spin_->selectAll();}
            return true;
        }
        case QEvent::ShortcutOverride:
            // Keep the window's Esc shortcut from swallowing the cancel key mid-drag.
            if (pressed_ && static_cast<QKeyEvent *>(event)->key()==Qt::Key_Escape) {event->accept();return true;}
            return false;
        case QEvent::KeyPress:
            if (pressed_ && static_cast<QKeyEvent *>(event)->key()==Qt::Key_Escape) {set_(startValue_);finish(label);return true;}
            return false;
        default: return false;
        }
    }
private:
    SpinScrubber(QWidget *label,QAbstractSpinBox *spin,std::function<double()> get,std::function<void(double)> set,double pixelStep)
        : QObject(label),spin_(spin),get_(std::move(get)),set_(std::move(set)),pixelStep_(pixelStep) {
        label->setCursor(Qt::SizeHorCursor);label->installEventFilter(this);
    }
    void finish(QWidget *label) {
        pressed_=dragging_=false;label->releaseKeyboard();label->setCursor(Qt::SizeHorCursor);
    }
    QAbstractSpinBox *spin_;
    std::function<double()> get_;
    std::function<void(double)> set_;
    double pixelStep_,startValue_=0,accumulated_=0;
    QPoint pressPos_,lastPos_;
    bool pressed_=false,dragging_=false;
};

class CenteredPlaybackLayout final : public QHBoxLayout {
public:
    void setGeometry(const QRect &rect) override {
        QHBoxLayout::setGeometry(rect);
        if (count()!=5) return;
        // Keep transport centred in the viewport when both side groups fit.
        // At smaller widths it stays in the available gap without overlapping.
        auto *middle=itemAt(2);auto position=middle->geometry();
        const int first=itemAt(0)->geometry().right()+spacing()+1,last=itemAt(4)->geometry().left()-spacing()-position.width();
        position.moveLeft(std::clamp(rect.x()+(rect.width()-position.width())/2,first,std::max(first,last)));middle->setGeometry(position);
    }
};

MainWindow::MainWindow(QWidget *parent,const QString &presetDirectory) : QMainWindow(parent), presetStore_(presetDirectory), worker_(new CaptureWorker) {
    qRegisterMetaType<FramePtr>(); qRegisterMetaType<CaptureInfo>();
    qRegisterMetaType<Project>();
    project_ = defaultProject();
    buildUi();
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(this, &MainWindow::openRequested, worker_, &CaptureWorker::open);
    connect(this, &MainWindow::decodeRequested, worker_, &CaptureWorker::decode);
    connect(worker_, &CaptureWorker::opened, this, [this](CaptureInfo info, FramePtr frame, quint64 gen) {
        if (gen != openingGeneration_) return;
        loading_ = false; loaded_ = true; decoding_ = pendingDecode_ = false;
        generation_ = gen; info_ = info;
        viewport_->setGhost(false); slider_->resetView(); // a newly opened capture is seen whole
        viewport_->setSplatAntialiasing(info.antialiased);
        processingState_={};
        project_ = pendingProject_.value_or(defaultProject()); project_.asset = info.path;
        if (project_.captureSettings.title.isEmpty()) project_.captureSettings.title = info.title;
        projectPath_ = pendingProjectPath_;
        history_.remember(projectPath_.isEmpty() ? info.path : projectPath_); updateRecentMenu();
        const double last = double(info.frames-1)/info.fps;
        if (!pendingProject_) project_.out = last;
        project_.in = std::clamp(project_.in, 0.0, last);
        project_.out = std::clamp(project_.out, project_.in, last);
        project_.time = std::clamp(project_.time, project_.in, project_.out);
        // Keep all transport controls on the capture's actual frame boundaries.
        project_.in = std::round(project_.in*info.fps)/info.fps;
        project_.out = std::round(project_.out*info.fps)/info.fps;
        project_.time = std::round(project_.time*info.fps)/info.fps;
        viewport_->setTransformMode(TransformMode::None);
        viewport_->setCropEditing(false); viewport_->setCrop(project_.crop());
        viewport_->setTransform(project_.transform);
        for (int g=0; g<3; ++g) viewport_->setCoordinateSpace(TransformMode(g+1),project_.spaces[g]);
        if (pendingProject_) viewport_->setCamera(project_.camera);
        else viewport_->fit(info.minimum, info.maximum);
        project_.camera = viewport_->camera();
        if (!pendingProject_) {fitCrop();viewport_->setCropEditing(false);viewport_->setTransformMode(TransformMode::None);}
        setWindowModified(!pendingProject_.has_value());
        pendingProject_.reset(); refreshPresets(); syncUi(); title();
        viewport_->setFrame(frame);
        if (project_.time > 0) requestFrame();
        else receiveFrame(frame);
        statusBar()->showMessage(tr("Capture ready: %1").arg(info.title), 5000);
    });
    connect(worker_, &CaptureWorker::decoded, this, [this](FramePtr frame, quint64 gen) {
        if (gen != generation_) return;
        decoding_ = false; receiveFrame(frame);
        if (pendingDecode_ && !loading_) { pendingDecode_ = false; requestFrame(); }
    });
    connect(worker_, &CaptureWorker::failed, this, [this](QString message, quint64 gen, bool opening) {
        if (opening) {
            if (gen != openingGeneration_) return;
            loading_ = false; pendingProject_.reset(); pendingProjectPath_.clear();
        } else {
            if (gen != generation_) return;
            decoding_ = pendingDecode_ = false;
        }
        play(false); syncUi(); showError(message);
    });
    connect(viewport_, &Viewport::renderFailed, this, [this](const QString &error) {
        statusBar()->showMessage(error);
        if (!smokeOutput_.isEmpty()) { qCritical("OpenGL: %s", qPrintable(error)); qApp->exit(2); }
    });
    connect(viewport_, &Viewport::cameraChanged, this, [this] {
        project_.camera = viewport_->camera(); if (loaded_ && !loading_) dirty();
    });
    connect(viewport_, &Viewport::transformModeChanged, this, &MainWindow::syncTransformButtons);
    connect(viewport_, &Viewport::transformEdited, this, [this](const Transform &transform) {
        const int frame=int(std::round(project_.time*info_.fps));
        if (project_.modifier() && project_.modifier()->type==ModifierType::AnimateTransform) project_.setAnimatedPose(frame,transform);
        else project_.transform=Transform::fromMatrix(transform.matrix()*project_.animationMatrix(frame).inverted());
        syncUi();dirty();
    });
    connect(viewport_,&Viewport::cropEdited,this,[this](const CropVolume &crop) {
        if (!project_.modifier() || project_.modifier()->type!=ModifierType::Crop) return;
        const bool enabled=project_.crop().enabled;project_.crop() = crop;project_.crop().enabled=enabled;keyCrop();syncUi();dirty();
    });
    connect(viewport_,&Viewport::frameRequested,this,&MainWindow::fitCurrentTarget);
    playback_.setInterval(16); playback_.setTimerType(Qt::PreciseTimer);
    connect(&playback_, &QTimer::timeout, this, [this] {
        double t = playStart_ + double(clock_.elapsed())/1000.0 * project_.speed;
        if (t > project_.out) {
            if (project_.loop && project_.out > project_.in)
                t = project_.in + std::fmod(t-project_.in, project_.out-project_.in+1.0/info_.fps);
            else { setTime(project_.out, false); play(false); return; }
        }
        setTime(t, false);
    });
    thread_.start(); syncUi(); title();
    restoreGeometry(settings_.value("geometry").toByteArray());
    restoreState(settings_.value("windowState").toByteArray());
    installCompactControls(this,settings_.value("Interface/CompactDensity",true).toBool());
}
MainWindow::~MainWindow() {
    playback_.stop(); thread_.quit(); thread_.wait();
}

void MainWindow::buildUi() {
    resize(1400, 900); setAcceptDrops(true);
    auto *file = menuBar()->addMenu(tr("File"));
    auto *newAction = file->addAction(tr("New project"), QKeySequence::New, this, &MainWindow::newProject);
    auto *openAction = file->addAction(tr("Open capture…"), QKeySequence::Open, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Open capture"), history_.openPath("Capture"), tr("Captures (*.vgs *.pgs *.mint)"));
        if (!path.isEmpty()) openPath(path);
    });
    auto *openProject = file->addAction(tr("Open project…"), QKeySequence("Ctrl+Shift+O"), this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Open project"), history_.openPath("Project"), tr("VGS project (*.vgsproj)"));
        if (!path.isEmpty()) openPath(path);
    });
    recentMenu_ = file->addMenu(tr("Open recent")); recentMenu_->setToolTipsVisible(true);
    connect(recentMenu_, &QMenu::aboutToShow, this, &MainWindow::updateRecentMenu);
    file->addSeparator();
    saveAction_ = file->addAction(tr("Save project"), QKeySequence::Save, this, [this] { save(); });
    saveAsAction_ = file->addAction(tr("Save project as…"), QKeySequence::SaveAs, this, [this] { save(true); });
    file->addSeparator(); auto *exitAction = file->addAction(tr("Exit"), QKeySequence::Quit, this, &QWidget::close);
    // Everything that writes something other than the project.
    auto *exportMenu = menuBar()->addMenu(tr("Export")); exportMenu->setToolTipsVisible(true);
    exportAction_ = exportMenu->addAction(tr("Export capture\u2026"), QKeySequence("Ctrl+E"), this, &MainWindow::exportCapture);
    exportAction_->setToolTip(tr("Export the selected Start/End range to VGS, PGS or MINT, baking capture transforms and active modifiers (Ctrl+E). MINT omits capture metadata."));
    plyAction_ = exportMenu->addAction(tr("Export current frame as PLY\u2026"), QKeySequence("Ctrl+Alt+E"), this, &MainWindow::exportFrame);
    plyAction_->setToolTip(tr("Write the frame on screen as a 3D Gaussian Splatting .ply, edited as Export capture would write it: transform, modifiers, colour processing and SH degree (Ctrl+Alt+E)."));
    imageAction_ = exportMenu->addAction(tr("Export viewport image\u2026"), QKeySequence("Ctrl+Shift+E"), this, &MainWindow::exportImage);
    imageAction_->setToolTip(tr("Save the viewport as it looks now as a PNG (Ctrl+Shift+E)."));
    newAction->setIcon(themeIcon(QIcon::ThemeIcon::DocumentNew,style()->standardIcon(QStyle::SP_FileIcon)));
    openAction->setIcon(themeIcon(QIcon::ThemeIcon::DocumentOpen,style()->standardIcon(QStyle::SP_DirOpenIcon)));
    saveAction_->setIcon(themeIcon(QIcon::ThemeIcon::DocumentSave,style()->standardIcon(QStyle::SP_DialogSaveButton)));
    // Every entry carries an icon from the same set, so the labels line up.
    openProject->setIcon(themeIcon(QIcon::ThemeIcon::FolderOpen,style()->standardIcon(QStyle::SP_DirIcon)));
    recentMenu_->setIcon(themeIcon(QIcon::ThemeIcon::DocumentOpenRecent,style()->standardIcon(QStyle::SP_FileDialogDetailedView)));
    saveAsAction_->setIcon(themeIcon(QIcon::ThemeIcon::DocumentSaveAs,style()->standardIcon(QStyle::SP_DialogSaveButton)));
    exitAction->setIcon(themeIcon(QIcon::ThemeIcon::ApplicationExit,style()->standardIcon(QStyle::SP_DialogCloseButton)));
    exportAction_->setIcon(themeIcon(QIcon::ThemeIcon::CameraVideo,style()->standardIcon(QStyle::SP_MediaPlay)));
    plyAction_->setIcon(themeIcon(QIcon::ThemeIcon::DocumentSend,style()->standardIcon(QStyle::SP_FileIcon)));
    imageAction_->setIcon(themeIcon(QIcon::ThemeIcon::CameraPhoto,style()->standardIcon(QStyle::SP_DesktopIcon)));
    updateRecentMenu();
    newAction->setToolTip(tr("Create an empty project (Ctrl+N)."));
    openAction->setToolTip(tr("Open a VGS, PGS or MINT capture (Ctrl+O)."));
    openProject->setToolTip(tr("Restore a saved editor project (Ctrl+Shift+O)."));
    saveAction_->setToolTip(tr("Save this project and its crop settings (Ctrl+S)."));
    auto *center = new QWidget; auto *layout = new QVBoxLayout(center);
    layout->setContentsMargins(0,0,0,0); layout->setSpacing(0);
    viewport_ = new Viewport; layout->addWidget(viewport_, 1);
    timeline_ = new QWidget; timeline_->setObjectName("timeline");
    auto *tl = new QVBoxLayout(timeline_); tl->setContentsMargins(18,8,18,12);tl->setSpacing(6);
    slider_ = new RangeSlider; slider_->setObjectName("captureRangeSlider");
    slider_->setToolTip(tr("Drag the upper marker to set Start, the lower marker to set End, or the white playhead to seek. The selected range is exported.\n"
        "Wheel: zoom · Shift+wheel or middle drag: pan · Double-click Capture: whole capture."));
    auto *controls = new CenteredPlaybackLayout;controls->setSpacing(8);
    auto *frameControls=new QWidget;frameControls->setObjectName("timelineFrameControls");frameControls->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);
    frameControls->setStyleSheet("QDoubleSpinBox { padding: 3px; min-height: 20px; font-size: 9pt; }");
    auto *frames=new QHBoxLayout(frameControls);frames->setContentsMargins(0,0,0,0);frames->setSpacing(5);
    auto field=[&](const QString &name) {auto *spin=new QDoubleSpinBox;spin->setObjectName(name);spin->setKeyboardTracking(false);spin->setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);spin->setDecimals(0);spin->setRange(0,0);frames->addWidget(spin);return spin;};
    inFrame_=field("timelineIn");frameSpin_=field("timelineFrame");outFrame_=field("timelineOut");
    timelineSecondsButton_=new QToolButton;timelineSecondsButton_->setObjectName("timelineSeconds");timelineSecondsButton_->setCheckable(true);timelineSecondsButton_->setChecked(settings_.value("Playback/SecondsDisplay",false).toBool());
    timelineSecondsButton_->setIcon(timelineClockIcon());timelineSecondsButton_->setIconSize({22,22});timelineSecondsButton_->setFixedSize(30,30);timelineSecondsButton_->setAccessibleName(tr("Timeline units"));
    timelineSecondsButton_->setProperty("neutralToggle",true);frames->insertWidget(0,timelineSecondsButton_);
    controls->addWidget(frameControls);controls->addStretch();
    auto *playbackControls=new QWidget;playbackControls->setObjectName("timelinePlaybackControls");playbackControls->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);
    auto *transport=new QHBoxLayout(playbackControls);transport->setContentsMargins(0,0,0,0);transport->setSpacing(5);
    auto button = [&](QStyle::StandardPixmap icon, const QString &tip, auto fn) {
        auto *b = new QPushButton; b->setIcon(transportIcon(style(),icon)); b->setToolTip(tip); b->setFixedSize(34,30);
        transport->addWidget(b); connect(b, &QPushButton::clicked, this, fn); return b;
    };
    button(QStyle::SP_MediaSkipBackward, tr("Go to the playback range start (Ctrl+Home)."), [this] { play(false); setTime(project_.in, true); });
    button(QStyle::SP_MediaSeekBackward, tr("Pause and step back one frame (Left Arrow)."), [this] { play(false); setTime(project_.time-1.0/info_.fps, true); });
    playButton_ = button(QStyle::SP_MediaPlay, tr("Play / pause (Space)"), [this] { play(!playback_.isActive()); });
    button(QStyle::SP_MediaSeekForward, tr("Pause and step forward one frame (Right Arrow)."), [this] { play(false); setTime(project_.time+1.0/info_.fps, true); });
    button(QStyle::SP_MediaSkipForward, tr("Go to the playback range end (Ctrl+End)."), [this] { play(false); setTime(project_.out, true); });
    controls->addWidget(playbackControls);controls->addStretch();
    auto *speedControls=new QWidget;speedControls->setObjectName("timelineSpeedControls");speedControls->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);
    auto *speedLayout=new QHBoxLayout(speedControls);speedLayout->setContentsMargins(0,0,0,0);speedLayout->setSpacing(5);
    loop_ = new QCheckBox(tr("Loop"));speedLayout->addWidget(loop_);speedLayout->addWidget(new QLabel(tr("Speed")));
    speed_ = new QDoubleSpinBox; speed_->setRange(0.1,4); speed_->setSingleStep(0.25); speed_->setSuffix(" ×"); speedLayout->addWidget(speed_);
    controls->addWidget(speedControls);tl->addLayout(controls);tl->addWidget(slider_);
    loop_->setToolTip(tr("Toggle looping within the playback range (L)."));
    modifierPanel_=new ModifierPanel;tl->addWidget(modifierPanel_);
    // The timeline's track runs exactly over the modifier bars, so a frame sits at the same x
    // in both; the space to its left heads the names column below. Queued: both settle first.
    connect(modifierPanel_,&ModifierPanel::trackMoved,this,&MainWindow::alignTimeline,Qt::QueuedConnection);
    modifierPanel_->setMinimumNameWidth(slider_->fontMetrics().horizontalAdvance(tr("Capture")));
    // One zoom and pan for the timeline and the bars: the timeline owns it.
    connect(slider_,&RangeSlider::viewChanged,modifierPanel_,&ModifierPanel::setView);
    connect(modifierPanel_,&ModifierPanel::zoomRequested,this,[this](double factor,int globalX) {slider_->zoomAt(factor,slider_->mapFromGlobal(QPoint(globalX,0)).x());});
    connect(modifierPanel_,&ModifierPanel::panRequested,slider_,&RangeSlider::panByPixels);
    connect(modifierPanel_,&ModifierPanel::resetViewRequested,slider_,&RangeSlider::resetView);
    connect(modifierPanel_,&ModifierPanel::selectionChanged,this,[this] {
        const auto modifier=modifierPanel_->project().selectedModifier;
        viewport_->setTransformMode(TransformMode::None);
        viewport_->setCropEditing(false);project_.selectedModifier=modifier;
        viewport_->setCrop(project_.crop());
        syncUi();dirty();revealModifierProperties();
    });
    connect(modifierPanel_,&ModifierPanel::stackChanged,this,[this] {
        const auto modifiers=modifierPanel_->project().modifiers;const auto modifier=modifierPanel_->project().selectedModifier;
        const bool targetChanged=project_.selectedModifier!=modifier;
        const bool keepEditing=viewport_->cropEditing() && project_.selectedModifier==modifier;
        if (project_.selectedModifier!=modifier) viewport_->setTransformMode(TransformMode::None);
        viewport_->setCropEditing(false);project_.modifiers=modifiers;project_.selectedModifier=modifier;
        if (project_.modifier() && project_.modifier()->type==ModifierType::AnimateTransform && !project_.modifier()->enabled) viewport_->setTransformMode(TransformMode::None);
        viewport_->setCrop(project_.crop());if (keepEditing && project_.modifier() && project_.modifier()->type==ModifierType::Crop) viewport_->setCropEditing(true);syncUi();dirty();
        if (targetChanged) revealModifierProperties();
    });
    connect(modifierPanel_,&ModifierPanel::cropAdded,this,&MainWindow::fitCrop);
    connect(modifierPanel_,&ModifierPanel::seekFrame,this,[this](int frame) {play(false);setTime(frame/info_.fps,true);});
    layout->addWidget(timeline_); setCentralWidget(center);
    auto *dock = new QDockWidget(tr("Tools"), this); dock->setObjectName("toolsDock");
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    tools_ = new QWidget; auto *side = new QVBoxLayout(tools_); side->setContentsMargins(16,16,16,16);
    auto *toolsHeading = new QLabel(tr("CAPTURE TOOLS")); toolsHeading->setObjectName("sectionTitle"); side->addWidget(toolsHeading);
    presetBox_ = new QGroupBox(tr("Presets")); auto *presetLayout = new QVBoxLayout(presetBox_);
    auto *savedPresetRow = new QHBoxLayout;
    presetCombo_ = new QComboBox; presetCombo_->setObjectName("savedPresetCombo"); presetCombo_->setMinimumContentsLength(18);
    presetCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    presetCombo_->setToolTip(tr("Choose a saved preset to restore capture transforms, modifiers and playback settings."));
    savedPresetRow->addWidget(presetCombo_,1);
    presetFolderButton_ = new QToolButton; presetFolderButton_->setIcon(transportIcon(style(),QStyle::SP_DirOpenIcon,Qt::white));
    presetFolderButton_->setFixedSize(34,34); presetFolderButton_->setAccessibleName(tr("Open presets folder"));
    presetFolderButton_->setToolTip(tr("Open the presets folder in the system file manager (Ctrl+Alt+P).")); savedPresetRow->addWidget(presetFolderButton_);
    presetLayout->addLayout(savedPresetRow);
    savePresetButton_ = new QPushButton(tr("Save preset…")); savePresetButton_->setObjectName("saveEditorPreset");
    savePresetButton_->setToolTip(tr("Save capture transforms, modifiers and playback settings (Ctrl+Shift+P). Viewport display controls stay independent. Metadata templates are stored as separate .presetmetadata files."));
    presetLayout->addWidget(savePresetButton_);
    connect(savePresetButton_,&QPushButton::clicked,this,&MainWindow::savePreset);
    connect(presetFolderButton_,&QToolButton::clicked,this,&MainWindow::openPresetFolder);
    connect(presetCombo_,&QComboBox::activated,this,[this](int) { loadSelectedPreset(); });
    refreshPresets();
    assetLabel_ = new QLabel(tr("No capture")); assetLabel_->setWordWrap(true); assetLabel_->setObjectName("assetTitle"); side->addWidget(assetLabel_);
    assetLabel_->setMinimumWidth(0);assetLabel_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    metadata_ = new QLabel; metadata_->setWordWrap(true); metadata_->setTextInteractionFlags(Qt::TextSelectableByMouse); side->addWidget(metadata_);
    captureSettingsButton_ = new QPushButton(tr("Metadata and processing…")); captureSettingsButton_->setObjectName("captureSettingsButton");
    captureSettingsButton_->setToolTip(tr("Prepare metadata and processing options, including despill, before exporting (Ctrl+M).")); side->addWidget(captureSettingsButton_);
    connect(captureSettingsButton_,&QPushButton::clicked,this,&MainWindow::editCaptureSettings);
    side->addWidget(presetBox_);
    const QString groups[] = {tr("Position"), tr("Rotation"), tr("Scale")};
    const QString axes[] = {"X", "Y", "Z"};
    const QString shortcuts[] = {"G", "R", "S"};
    const QString modeIcons[] = {":/icons/move.png",":/icons/rotate.png",":/icons/scale.png"};
    const QString accessibleModes[] = {tr("Move"),tr("Rotate"),tr("Scale")};
    const QString modeNames[] = {tr("Click to toggle Move. G activates Move; repeat G to switch Global/Local. Esc exits all modes."),tr("Click to toggle Rotate. R activates Rotate; repeat R to switch Global/Local. Esc exits all modes."),tr("Click to toggle Scale. S activates Scale; repeat S to switch Global/Local. Crop scaling stays anchored at its base. Esc exits all modes.")};
    transformModes_ = new QButtonGroup(this); transformModes_->setExclusive(true);
    auto *transformBox=new QGroupBox(tr("Transform · Capture"));transformBox_=transformBox;transformBox->setObjectName("transformProperties");transformBox->setProperty("transformGroup",true);
    transformBox->setStyleSheet("QDoubleSpinBox {padding: 3px; min-height: 18px; font-size: 9pt;}");
    auto *transformLayout=new QGridLayout(transformBox);transformLayout->setHorizontalSpacing(5);transformLayout->setVerticalSpacing(6);
    const QColor axisColours[]={{240,60,90},{85,185,105},{67,147,214}};
    for (int g = 0; g < 3; ++g) {
        transformLayout->addWidget(new QLabel(groups[g]),g,0);
        auto *modeButton = new QToolButton; modeButtons_[g] = modeButton;
        modeButton->setText(shortcuts[g]); modeButton->setCheckable(true); modeButton->setFixedSize(34,34);
        modeButton->setIcon(editorButtonIcon(modeIcons[g],true)); modeButton->setIconSize(QSize(26,26));
        modeButton->setToolButtonStyle(Qt::ToolButtonIconOnly); modeButton->setAccessibleName(accessibleModes[g]);
        modeButton->setToolTip(modeNames[g]); modeButton->setObjectName(QString("transformMode_%1").arg(g));
        transformModes_->addButton(modeButton,g); transformLayout->addWidget(modeButton,g,1);
        connect(modeButton,&QToolButton::clicked,this,[this,g] { toggleTransformMode(TransformMode(g+1)); viewport_->setFocus(); });
        auto *space = new QToolButton; spaceButtons_[g] = space; space->setText(tr("Global"));
        space->setObjectName(QString("coordinateSpace_%1").arg(g)); space->setFixedSize(34,34);
        space->setToolButtonStyle(Qt::ToolButtonIconOnly); space->setIconSize(QSize(26,26));
        space->setIcon(editorButtonIcon(":/icons/global.png",false)); transformLayout->addWidget(space,g,2);
        connect(space,&QToolButton::clicked,this,[this,g] { toggleCoordinateSpace(g); viewport_->setFocus(); });
        for (int axis = 0; axis < 3; ++axis) {
            auto *spin = new QDoubleSpinBox; transform_[g][axis] = spin; spin->setDecimals(g == 1 ? 2 : 4);
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            spin->setRange(g == 2 ? 0.0001 : (g == 1 ? -36000 : -1e6), g == 2 ? 10000 : (g == 1 ? 36000 : 1e6));
            spin->setSingleStep(g == 1 ? 1 : 0.01);
            // Avoid a wide minimum size driven by the largest representable number.
            spin->setMinimumWidth(0); spin->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
            spin->setObjectName(QString("transform_%1_%2").arg(g).arg(axis));
            spin->setToolTip(groups[g] + " " + axes[axis] + (g==1 ? tr(" (degrees)") : QString()));
            auto *label = new QLabel(axes[axis]); label->setBuddy(spin);
            label->setObjectName(QString("transformAxis_%1_%2").arg(g).arg(axis));label->setStyleSheet(QString("color: %1; font-weight: 600;").arg(axisColours[axis].name()));
            SpinScrubber::attach(label,spin,g==1 ? 0.5 : 0.005);
            spin->setToolTip(spin->toolTip()+tr("\nDrag the axis label to scrub: Shift fine, Ctrl coarse, Esc cancels."));
            transformLayout->addWidget(label,g,3+axis*2);transformLayout->addWidget(spin,g,4+axis*2);transformLayout->setColumnStretch(4+axis*2,1);
            connect(spin, &QDoubleSpinBox::valueChanged, this, [this, g, axis](double v) {
                if (syncing_) return;
                viewport_->setDisplayedComponent(g,axis,float(v));
            });
        }
    }
    side->addWidget(transformBox);
    auto *reset = new QPushButton(tr("Reset transform")); transformLayout->addWidget(reset,3,0,1,transformLayout->columnCount());
    resetTransformButton_ = reset;
    reset->setToolTip(tr("Reset the current target's position, rotation and scale (Alt+Home)."));
    connect(reset, &QPushButton::clicked, this, &MainWindow::resetTransform);
    side->addStretch(1);
    parametersHeading_=new QLabel(tr("PARAMETERS"));parametersHeading_->setObjectName("sectionTitle");side->addWidget(parametersHeading_);
    auto *cropBox = new QGroupBox(tr("Crop modifier")); cropProperties_=cropBox; auto *cropForm = new QFormLayout(cropBox); cropForm_ = cropForm;
    cropBox->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Maximum);
    cropShapeCombo_ = new QComboBox; cropShapeCombo_->addItem(tr("Cylinder"),int(CropShape::Cylinder)); cropShapeCombo_->addItem(tr("Box"),int(CropShape::Box));
    cropShapeCombo_->setObjectName("cropShape"); cropShapeCombo_->setToolTip(tr("Choose Cylinder or Box. Both share the same base pivot and transform; their dimensions are retained separately."));
    connect(cropShapeCombo_,&QComboBox::activated,this,[this](int) {
        if (syncing_ || !loaded_ || loading_ || !project_.modifier() || project_.modifier()->type!=ModifierType::Crop) return;
        project_.crop().shape = CropShape(cropShapeCombo_->currentData().toInt()); viewport_->setCrop(project_.crop()); syncUi(); dirty(); viewport_->setFocus();
    });
    cropEditButton_ = new QPushButton(tr("Edit")); cropEditButton_->setCheckable(true);
    cropEditButton_->setObjectName("editCropVolume");
    cropEditButton_->setToolTip(tr("Toggle crop editing (Tab / C). On: show the wire volume and edit it with G/R/S. Repeat the active mode key to switch Global/Local. Off: apply the crop preview."));
    t4dsPresetButton_ = new QPushButton(tr("T4DS Preset")); smnPresetButton_ = new QPushButton(tr("SMN Preset"));
    t4dsPresetButton_->setToolTip(tr("Reset the cylinder at (0, 0, 0), height 2.5 m, radius 1.5 m (Ctrl+Alt+1)."));
    smnPresetButton_->setToolTip(tr("Reset the cylinder at (0, 0, 0), height 2.5 m, radius 1 m (Ctrl+Alt+2)."));
    auto *presets = new QWidget; auto *presetRow = new QHBoxLayout(presets); presetRow->setContentsMargins(0,0,0,0);
    const int presetMinimumWidth = std::max(t4dsPresetButton_->minimumSizeHint().width(),smnPresetButton_->minimumSizeHint().width());
    const int presetHeight = std::max(t4dsPresetButton_->sizeHint().height(),smnPresetButton_->sizeHint().height());
    for (auto *button : {cropEditButton_,t4dsPresetButton_,smnPresetButton_}) {
        button->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
        button->setMinimumWidth(presetMinimumWidth); button->setFixedHeight(presetHeight); presetRow->addWidget(button,1);
    }
    cropForm->addRow(presets);
    cropForm->addRow(tr("Shape"),cropShapeCombo_);
    // Static or Animated, switchable at any time: each keeps its own pose and the keys survive.
    cropAnimationCombo_=new QComboBox;cropAnimationCombo_->setObjectName("cropAnimation");cropAnimationCombo_->addItem(tr("Static"),0);cropAnimationCombo_->addItem(tr("Animated"),1);
    cropAnimationCombo_->setToolTip(tr("Static: one pose and size for the whole capture. Animated: position, rotation, scale and size follow keys on the timeline, and editing the crop sets a key at the current frame. Switching keeps both the static pose and the keys. Shape, mode and the editing preview are never animated."));
    cropForm->addRow(tr("Animation"),cropAnimationCombo_);
    connect(cropAnimationCombo_,&QComboBox::activated,this,[this](int) {
        if (syncing_ || !project_.modifier() || project_.modifier()->type!=ModifierType::Crop) return;
        setCropAnimated(cropAnimationCombo_->currentData().toInt()==1);viewport_->setFocus();
    });
    cropModeCombo_=new QComboBox;cropModeCombo_->setObjectName("cropMode");cropModeCombo_->addItem(tr("Keep inside"),0);cropModeCombo_->addItem(tr("Remove inside"),1);
    cropModeCombo_->setToolTip(tr("Keep preserves what is inside; Remove deletes it. Where they overlap, Remove wins. With only Remove crops, everything outside them is kept."));
    cropForm->addRow(tr("Mode"),cropModeCombo_);
    cropPreviewCombo_=new QComboBox;cropPreviewCombo_->setObjectName("cropEditPreview");cropPreviewCombo_->addItem(tr("Colour code"),1);cropPreviewCombo_->addItem(tr("Hide removed"),0);
    cropPreviewCombo_->setToolTip(tr("While this crop is edited: Colour code lightens what the crops keep and shows what they delete in red; Hide removed hides what they delete. Editing only: the normal view and the export are unaffected."));
    cropForm->addRow(tr("While editing"),cropPreviewCombo_);
    connect(cropModeCombo_,&QComboBox::activated,this,[this](int) {
        if (syncing_ || !project_.modifier() || project_.modifier()->type!=ModifierType::Crop) return;
        project_.crop().remove=cropModeCombo_->currentData().toInt()==1;viewport_->setCrop(project_.crop());syncModifiers();dirty();
    });
    connect(cropPreviewCombo_,&QComboBox::activated,this,[this](int) {
        if (syncing_ || !project_.modifier() || project_.modifier()->type!=ModifierType::Crop) return;
        project_.crop().showRemovedInRed=cropPreviewCombo_->currentData().toInt()==1;viewport_->setCrop(project_.crop());syncModifiers();dirty();
    });
    connect(t4dsPresetButton_,&QPushButton::clicked,this,[this] { applyCropPreset(1.5f); });
    connect(smnPresetButton_,&QPushButton::clicked,this,[this] { applyCropPreset(1.0f); });
    cropRadius_ = new QDoubleSpinBox; cropRadiusZ_ = new QDoubleSpinBox; cropHeight_ = new QDoubleSpinBox; cropWidth_ = new QDoubleSpinBox; cropDepth_ = new QDoubleSpinBox;
    for (auto *spin : {cropRadius_,cropRadiusZ_,cropHeight_,cropWidth_,cropDepth_}) { spin->setRange(0.0001,1e6); spin->setDecimals(4); spin->setSingleStep(0.05); spin->setSuffix(" m"); }
    cropRadius_->setObjectName("cropRadiusX"); cropRadiusZ_->setObjectName("cropRadiusZ");
    cropForm->addRow(tr("Radius X"),cropRadius_); cropForm->addRow(tr("Radius Z"),cropRadiusZ_); cropForm->addRow(tr("Height"),cropHeight_);
    cropForm->addRow(tr("Width"),cropWidth_); cropForm->addRow(tr("Depth"),cropDepth_);
    // An animated crop's keys; hidden while it is static, which keeps them.
    cropKeys_=new QWidget;cropKeys_->setObjectName("cropKeys");auto *keysLayout=new QVBoxLayout(cropKeys_);keysLayout->setContentsMargins(0,0,0,0);
    auto *keyButtons=new QHBoxLayout;auto *setCropKey=new QPushButton(tr("Set key"));setCropKey->setObjectName("setCropKey");
    setCropKey->setToolTip(tr("Add or update a key at the current frame with the crop as it is now. Editing the crop also sets one."));
    cropRemoveKey_=new QPushButton(tr("Remove key"));cropRemoveKey_->setObjectName("removeCropKey");
    keyButtons->addWidget(setCropKey);keyButtons->addWidget(cropRemoveKey_);keysLayout->addLayout(keyButtons);
    cropKeyTable_=new QTableWidget;cropKeyTable_->setObjectName("cropKeyTable");cropKeyTable_->setColumnCount(4);cropKeyTable_->setHorizontalHeaderLabels({tr("Frame"),tr("Position"),tr("Rotation"),tr("Size")});
    cropKeyTable_->setSelectionBehavior(QAbstractItemView::SelectRows);cropKeyTable_->setSelectionMode(QAbstractItemView::SingleSelection);cropKeyTable_->setAlternatingRowColors(true);
    cropKeyTable_->verticalHeader()->hide();cropKeyTable_->verticalHeader()->setDefaultSectionSize(28);
    {auto palette=cropKeyTable_->palette();palette.setColor(QPalette::Highlight,QColor(73,73,73));palette.setColor(QPalette::HighlightedText,Qt::white);cropKeyTable_->setPalette(palette);}
    cropKeyTable_->setStyleSheet("QTableWidget::item:selected { background: #494949; color: #ffffff; }");
    cropKeyTable_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);for (int col=1;col<4;++col) cropKeyTable_->horizontalHeader()->setSectionResizeMode(col,QHeaderView::Stretch);
    cropKeyTable_->setToolTip(tr("Click a key to go to its frame. Double-click a frame to move the key. Edit the crop at a frame to change or add its key."));
    keysLayout->addWidget(cropKeyTable_);cropForm->addRow(cropKeys_);
    connect(setCropKey,&QPushButton::clicked,this,[this] {
        auto *m=project_.modifier();if (!m || m->type!=ModifierType::Crop || !m->cropAnimation.animated) return;keyCrop();syncUi();dirty();
    });
    connect(cropRemoveKey_,&QPushButton::clicked,this,[this] {
        auto *m=project_.modifier();if (!m || m->type!=ModifierType::Crop || m->cropAnimation.keys.size()<2) return;
        const int row=cropKeyTable_->currentRow();const auto &keys=m->cropAnimation.keys;
        m->cropAnimation.removeKey(row>=0 && row<keys.size() ? keys[row].frame : currentFrame());syncUi();dirty();
    });
    connect(cropKeyTable_,&QTableWidget::cellClicked,this,[this](int row,int) {
        const auto *m=project_.modifier();if (syncing_ || !m || m->type!=ModifierType::Crop || row>=m->cropAnimation.keys.size()) return;
        play(false);setTime(m->cropAnimation.keys[row].frame/info_.fps,true);
    });
    connect(cropKeyTable_,&QTableWidget::itemChanged,this,[this](QTableWidgetItem *item) {
        auto *m=project_.modifier();if (syncing_ || item->column()!=0 || !m || m->type!=ModifierType::Crop || item->row()>=m->cropAnimation.keys.size()) return;
        auto &keys=m->cropAnimation.keys;bool ok;const int frame=item->text().toInt(&ok);
        bool valid=ok && frame>=0 && frame<=std::max(0,info_.frames-1);
        for (int i=0;i<keys.size();++i) if (i!=item->row() && keys[i].frame==frame) valid=false;
        if (valid) {keys[item->row()].frame=frame;std::sort(keys.begin(),keys.end(),[](const auto &a,const auto &b) {return a.frame<b.frame;});}
        syncUi();if (valid) dirty();
    });
    auto *cropButtons = new QWidget; auto *cropRow = new QHBoxLayout(cropButtons); cropRow->setContentsMargins(0,0,0,0);
    cropFitButton_ = new QPushButton(tr("Fit capture")); cropClearButton_ = new QPushButton(tr("Disable crop"));
    cropFitButton_->setToolTip(tr("Fit the crop volume to the capture bounds and enter Move mode (Ctrl+F)."));
    cropClearButton_->setToolTip(tr("Disable this crop modifier without deleting its settings (Ctrl+Shift+C)."));
    cropRow->addWidget(cropFitButton_); cropRow->addWidget(cropClearButton_); cropForm->addRow(cropButtons);
    cropStatus_ = new QLabel; cropStatus_->setWordWrap(true); cropForm->addRow(cropStatus_); side->addWidget(cropBox);
    connect(cropEditButton_,&QPushButton::clicked,this,[this](bool checked) { editCrop(checked); viewport_->setFocus(); });
    connect(cropFitButton_,&QPushButton::clicked,this,&MainWindow::fitCrop);
    connect(cropClearButton_,&QPushButton::clicked,this,&MainWindow::clearCrop);
    connect(cropRadius_,&QDoubleSpinBox::valueChanged,this,[this](double value) {
        if (syncing_) return; project_.crop().radius = float(value); keyCrop(); viewport_->setCrop(project_.crop()); syncModifiers(); dirty();
    });
    connect(cropRadiusZ_,&QDoubleSpinBox::valueChanged,this,[this](double value) {
        if (syncing_) return; project_.crop().radiusZ = float(value); keyCrop(); viewport_->setCrop(project_.crop()); syncModifiers(); dirty();
    });
    connect(cropHeight_,&QDoubleSpinBox::valueChanged,this,[this](double value) {
        if (syncing_) return; project_.crop().height = float(value); keyCrop(); viewport_->setCrop(project_.crop()); syncModifiers(); dirty();
    });
    connect(cropWidth_,&QDoubleSpinBox::valueChanged,this,[this](double value) { if (!syncing_) { project_.crop().width = float(value); keyCrop(); viewport_->setCrop(project_.crop()); syncModifiers(); dirty(); } });
    connect(cropDepth_,&QDoubleSpinBox::valueChanged,this,[this](double value) { if (!syncing_) { project_.crop().depth = float(value); keyCrop(); viewport_->setCrop(project_.crop()); syncModifiers(); dirty(); } });
    greenProperties_=new QGroupBox(tr("Remove green points"));greenProperties_->setObjectName("greenModifierProperties");auto *greenForm=new QFormLayout(greenProperties_);
    greenProperties_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Maximum);
    greenSaturation_=new QDoubleSpinBox;greenSaturation_->setObjectName("greenMinimumSaturation");greenSaturation_->setRange(0,100);greenSaturation_->setDecimals(1);greenSaturation_->setSuffix(" %");greenSaturation_->setValue(50);
    greenSaturation_->setToolTip(tr("Minimum HSV saturation of source base RGB. Camera and SH shading do not affect the classification."));
    greenHue_=new QDoubleSpinBox;greenHue_->setObjectName("greenHueTolerance");greenHue_->setRange(0,180);greenHue_->setDecimals(1);greenHue_->setSuffix(QString(QChar(0x00b0)));greenHue_->setValue(45);
    greenHue_->setToolTip(tr("Maximum circular hue distance from pure green (120 degrees). Smaller values target a narrower green range."));
    greenForm->addRow(tr("Minimum saturation"),greenSaturation_);greenForm->addRow(tr("Hue distance"),greenHue_);
    greenLinearRgb_=new QCheckBox(tr("Linear RGB"));greenLinearRgb_->setObjectName("greenLinearRgb");greenLinearRgb_->setChecked(true);
    greenLinearRgb_->setToolTip(tr("Convert source base RGB from sRGB to linear RGB before HSV matching. Saved older presets keep their original colour space."));greenForm->addRow(greenLinearRgb_);
    auto *greenNote=new QLabel(tr("Removes matching source RGB points over the full timeline, after the union of enabled crops."));greenNote->setWordWrap(true);greenForm->addRow(greenNote);side->addWidget(greenProperties_);
    auto greenChanged=[this] {
        if (syncing_ || !project_.modifier() || project_.modifier()->type!=ModifierType::RemoveGreen) return;
        project_.modifier()->green.minimumSaturation=float(greenSaturation_->value()/100);project_.modifier()->green.hueTolerance=float(greenHue_->value());project_.modifier()->green.linearRgb=greenLinearRgb_->isChecked();syncModifiers();dirty();
    };
    connect(greenSaturation_,&QDoubleSpinBox::valueChanged,this,[greenChanged](double) {greenChanged();});connect(greenHue_,&QDoubleSpinBox::valueChanged,this,[greenChanged](double) {greenChanged();});
    connect(greenLinearRgb_,&QCheckBox::toggled,this,[greenChanged](bool) {greenChanged();});
    animationProperties_=new AnimationPanel;side->addWidget(animationProperties_);
    connect(animationProperties_,&AnimationPanel::setKeyRequested,this,[this] {auto *m=project_.modifier();if (!m || m->type!=ModifierType::AnimateTransform) return;const int frame=int(std::round(project_.time*info_.fps));m->animation.setKey(frame,m->animation.evaluate(frame));syncUi();dirty();});
    connect(animationProperties_,&AnimationPanel::animationChanged,this,[this](const TransformAnimation &animation) {auto *m=project_.modifier();if (!m || m->type!=ModifierType::AnimateTransform) return;m->animation=animation;syncUi();dirty();});
    connect(animationProperties_,&AnimationPanel::seekFrame,this,[this](int frame) {play(false);setTime(frame/info_.fps,true);});
    isolationProperties_=new QGroupBox(tr("Purge Isolated"));isolationProperties_->setObjectName("isolationModifierProperties");auto *isolationForm=new QFormLayout(isolationProperties_);
    isolationProperties_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Maximum);
    isolationNeighbour_=new QSpinBox;isolationNeighbour_->setObjectName("isolationNeighbour");isolationNeighbour_->setRange(1,256);isolationNeighbour_->setValue(4);
    isolationPercent_=new QDoubleSpinBox;isolationPercent_->setObjectName("isolationMedianPercent");isolationPercent_->setRange(0,1000000);isolationPercent_->setDecimals(1);isolationPercent_->setSuffix(" %");isolationPercent_->setValue(700);
    isolationForm->addRow(tr("Nth neighbour"),isolationNeighbour_);isolationForm->addRow(tr("Distance / median"),isolationPercent_);
    auto *isolationNote=new QLabel(tr("Removes points whose distance to neighbour N exceeds this percentage of the frame's median Nth-neighbour distance, after crop and colour filtering. 100% = median; 700% = 7 times median."));isolationNote->setWordWrap(true);isolationForm->addRow(isolationNote);side->addWidget(isolationProperties_);
    walkProperties_=new QGroupBox(tr("Walk"));walkProperties_->setObjectName("walkModifierProperties");auto *walkForm=new QFormLayout(walkProperties_);
    walkProperties_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum);
    walkSpeed_=new QDoubleSpinBox;walkSpeed_->setObjectName("walkSpeed");walkSpeed_->setRange(0,100);walkSpeed_->setDecimals(2);walkSpeed_->setSingleStep(.1);walkSpeed_->setSuffix(" m/s");walkSpeed_->setValue(1);
    walkSpeed_->setToolTip(tr("How fast the capture advances along +Z, from the start of the export range."));
    // The speed is stored in m/s; this only chooses how it is shown, and is kept with the
    // modifier so projects and presets reopen showing the same unit.
    walkUnits_=new QToolButton;walkUnits_->setObjectName("walkUnits");walkUnits_->setCheckable(true);walkUnits_->setProperty("neutralToggle",true);
    {
        const QIcon source=QIcon::fromTheme(QIcon::ThemeIcon::MediaPlaylistRepeat,style()->standardIcon(QStyle::SP_BrowserReload));QIcon icon;
        for (auto mode:{QIcon::Normal,QIcon::Disabled}) {
            QPixmap pixmap=source.pixmap(20,20);QPainter tint(&pixmap);tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tint.fillRect(pixmap.rect(),mode==QIcon::Disabled ? QColor("#666666") : QColor("#e6e6e6"));tint.end();icon.addPixmap(pixmap,mode);
        }
        walkUnits_->setIcon(icon);
    }
    walkUnits_->setIconSize({18,18});walkUnits_->setFixedSize(30,30);walkUnits_->setAccessibleName(tr("Speed units"));
    walkUnits_->setToolTip(tr("Show the speed in km/h instead of m/s. It is always stored in m/s."));
    auto *walkRow=new QWidget;auto *walkRowLayout=new QHBoxLayout(walkRow);walkRowLayout->setContentsMargins(0,0,0,0);walkRowLayout->setSpacing(4);
    walkRowLayout->addWidget(walkSpeed_,1);walkRowLayout->addWidget(walkUnits_);walkForm->addRow(tr("Speed"),walkRow);
    auto *walkNote=new QLabel(tr("Preview: the capture stays and the floor slides back under it, with a finer grid; at the right speed a planted foot stays on the grid. Export does not move the capture: VGS/PGS mark it as walking at this speed along +Z in the header, for players to carry it. Rotate the capture to choose the direction."));walkNote->setWordWrap(true);walkForm->addRow(walkNote);side->addWidget(walkProperties_);
    // Bake anti-aliasing: prepares a capture trained with anti-aliasing for renderers that
    // do not compensate, for a viewing distance and screen.
    bakeProperties_=new QGroupBox(tr("Bake anti-aliasing"));bakeProperties_->setObjectName("bakeModifierProperties");auto *bakeForm=new QFormLayout(bakeProperties_);
    bakeProperties_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum);
    bakeDistance_=new QDoubleSpinBox;bakeDistance_->setObjectName("bakeDistance");bakeDistance_->setRange(.01,1000);bakeDistance_->setDecimals(2);bakeDistance_->setSingleStep(.1);bakeDistance_->setSuffix(" m");bakeDistance_->setValue(2.5);
    bakeDistance_->setToolTip(tr("How far the capture is usually seen from. Closer than this it looks slightly softer; much farther away some aliasing comes back, as in any plain capture."));
    bakeScreenHeight_=new QSpinBox;bakeScreenHeight_->setObjectName("bakeScreenHeight");bakeScreenHeight_->setRange(16,16384);bakeScreenHeight_->setSingleStep(120);bakeScreenHeight_->setSuffix(" px");bakeScreenHeight_->setValue(1080);
    bakeScreenHeight_->setToolTip(tr("Vertical resolution of the screen it is seen on, at a 45 degree vertical field of view."));
    bakeForm->addRow(tr("Viewing distance"),bakeDistance_);bakeForm->addRow(tr("Screen height"),bakeScreenHeight_);
    bakeSizeLabel_=new QLabel;bakeSizeLabel_->setObjectName("bakeSize");bakeForm->addRow(tr("Minimum size"),bakeSizeLabel_);
    auto *bakeNote=new QLabel(tr("For captures trained with anti-aliasing (every Gracia .mint). Each splat grows to at least about a pixel at that distance and its opacity falls by as much, so needle splats fade instead of drawing as solid lines in renderers that do not compensate. Exports drop the anti-aliasing hint; .vgs/.pgs keep the per-splat opacity factor in an optional attribute."));
    bakeNote->setWordWrap(true);bakeForm->addRow(bakeNote);side->addWidget(bakeProperties_);
    SpinScrubber::attachFormLabel(bakeDistance_,0.01);
    auto bakeChanged=[this] {
        if (syncing_ || !project_.modifier() || project_.modifier()->type!=ModifierType::BakeAntialiasing) return;
        project_.modifier()->bakeDistance=bakeDistance_->value();project_.modifier()->bakeScreenHeight=bakeScreenHeight_->value();
        bakeSizeLabel_->setText(tr("%1 mm").arg(project_.modifier()->bakeSize()*1000,0,'f',2));syncModifiers();dirty();
    };
    connect(bakeDistance_,&QDoubleSpinBox::valueChanged,this,[bakeChanged](double) {bakeChanged();});
    connect(bakeScreenHeight_,&QSpinBox::valueChanged,this,[bakeChanged](int) {bakeChanged();});
    connect(walkUnits_,&QToolButton::toggled,this,[this](bool kmh) {
        auto *m=project_.modifier();if (syncing_ || !m || m->type!=ModifierType::Walk) return;
        m->walkKmh=kmh;showWalkSpeed(*m);syncModifiers();dirty();
    });
    connect(walkSpeed_,&QDoubleSpinBox::valueChanged,this,[this](double value) {
        if (syncing_ || !project_.modifier() || project_.modifier()->type!=ModifierType::Walk) return;
        project_.modifier()->walkSpeed=project_.modifier()->walkKmh ? value/3.6 : value;viewport_->setFloorScroll(project_.walkSpeed()>0,project_.walkDistance(project_.time));syncModifiers();dirty();
    });
    // Modifier parameters scrub from their labels like the transform fields: per pixel, a
    // step that suits the unit (metres, percent, degrees, whole neighbours, m/s).
    for (auto *spin:{cropRadius_,cropRadiusZ_,cropHeight_,cropWidth_,cropDepth_}) SpinScrubber::attachFormLabel(spin,0.005);
    SpinScrubber::attachFormLabel(greenSaturation_,0.25);SpinScrubber::attachFormLabel(greenHue_,0.25);
    SpinScrubber::attachFormLabel(isolationNeighbour_,0.05);SpinScrubber::attachFormLabel(isolationPercent_,1.0);
    // The speed's label belongs to its row (field and unit button), and it scrubs in m/s
    // whichever unit is shown, so a pixel moves the walk by the same amount either way.
    if (auto *label=walkForm->labelForField(walkRow)) {
        SpinScrubber::attach(label,walkSpeed_,[this] {return walkUnits_->isChecked() ? walkSpeed_->value()/3.6 : walkSpeed_->value();},
                             [this](double v) {walkSpeed_->setValue(walkUnits_->isChecked() ? v*3.6 : v);},0.01);
        walkSpeed_->setToolTip(walkSpeed_->toolTip()+"\n"+tr("Drag the label to scrub: Shift fine, Ctrl coarse, Esc cancels."));
    }
    isolationNeighbour_->setToolTip(tr("Nearest-neighbour rank, excluding the point itself. Frames with too few surviving points are preserved."));isolationPercent_->setToolTip(tr("Maximum Nth-neighbour distance as a percentage of the frame median. Lower values remove more points."));
    auto isolationChanged=[this] {if (syncing_ || !project_.modifier() || project_.modifier()->type!=ModifierType::PurgeIsolated) return;project_.modifier()->isolation={isolationNeighbour_->value(),isolationPercent_->value()};syncModifiers();dirty();};
    connect(isolationNeighbour_,&QSpinBox::valueChanged,this,[isolationChanged](int) {isolationChanged();});connect(isolationPercent_,&QDoubleSpinBox::valueChanged,this,[isolationChanged](double) {isolationChanged();});
    displayControls_=new QWidget;displayControls_->setObjectName("viewportDisplayControls");
    // QSS heights exclude the 1 px border, so the field matches the painted Background button exactly.
    displayControls_->setStyleSheet(QString("QWidget#viewportDisplayControls QLabel { color: #dddddd; font-size: 9pt; }"
        "QWidget#viewportDisplayControls QDoubleSpinBox { padding: 0 16px 0 7px; min-height: %1px; max-height: %1px; font-size: 9pt; border-radius: 4px; }"
        "QWidget#viewportDisplayControls QComboBox { padding: 0 7px; min-height: %1px; max-height: %1px; font-size: 9pt; border-radius: 4px; }").arg(ViewCube::rowHeight-2));
    auto *displayLayout=new QVBoxLayout(displayControls_);displayLayout->setContentsMargins(0,0,0,0);displayLayout->setSpacing(4);
    pointSize_ = new QDoubleSpinBox;pointSize_->setObjectName("displayPointSize");pointSize_->setDecimals(1);pointSize_->setRange(1,12); pointSize_->setSingleStep(0.5); pointSize_->setPrefix(tr("Point size  "));pointSize_->setSuffix(" px");pointSize_->setValue(5);
    pointSize_->setToolTip(tr("Opaque point diameter in viewport pixels. Default: 5 px."));
    pointSize_->setMinimumWidth(0);pointSize_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    // Points or Gaussian splats; a display preference kept in the settings, not in projects.
    renderStyle_=new PrefixedComboBox(tr("Render"));renderStyle_->setObjectName("displayRenderStyle");
    renderStyle_->addItem(tr("3D points"));renderStyle_->addItem(tr("Gaussian"));
    renderStyle_->setToolTip(tr("Draw each record as an opaque point (1), or as its Gaussian (2): sized, oriented and blended back to front as a splat viewer shows it. Display only; exports are unaffected."));
    renderStyle_->setMinimumWidth(0);renderStyle_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    displayLayout->addWidget(renderStyle_);
    displayLayout->addWidget(pointSize_); // same edges and height as the painted Background button
    // Splats take the point size's place with the spherical-harmonic bands they evaluate.
    splatSh_=new PrefixedComboBox(tr("SH"));splatSh_->setObjectName("displaySplatSh");
    for (int degree=0;degree<=3;++degree) {
        splatSh_->addItem(degree ? tr("SH%1").arg(degree) : tr("SH0 (base colour)"),degree);
        splatSh_->setItemData(degree,degree ? QString::number(degree) : tr("0 (base colour)"),PrefixedComboBox::ClosedTextRole);
    }
    splatSh_->addItem(tr("All"),-1);
    splatSh_->setToolTip(tr("Spherical-harmonic bands evaluated for the splats' view-dependent colour. All uses every band the capture has. Display only; the export settings decide what is written."));
    splatSh_->setMinimumWidth(0);splatSh_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    displayLayout->addWidget(splatSh_);
    auto showRenderStyle=[this] {const bool splats=renderStyle_->currentIndex()==1;pointSize_->setVisible(!splats);splatSh_->setVisible(splats);viewport_->setSplatRendering(splats);};
    {
        QSignalBlocker a(renderStyle_),b(splatSh_);
        renderStyle_->setCurrentIndex(settings_.value("Display/Splats",true).toBool() ? 1 : 0);
        splatSh_->setCurrentIndex(std::clamp(settings_.value("Display/SplatSh",4).toInt(),0,4));
        viewport_->setSplatShDegree(splatSh_->currentData().toInt()); showRenderStyle();
        for (auto *combo:{renderStyle_,splatSh_}) combo->setAccessibleName(static_cast<PrefixedComboBox *>(combo)->closedText());
    }
    connect(renderStyle_,&QComboBox::currentIndexChanged,this,[this,showRenderStyle](int index) {settings_.setValue("Display/Splats",index==1);showRenderStyle();viewport_->setFocus();});
    connect(splatSh_,&QComboBox::currentIndexChanged,this,[this](int index) {settings_.setValue("Display/SplatSh",index);viewport_->setSplatShDegree(splatSh_->currentData().toInt());viewport_->setFocus();});
    ghostButton_=new QToolButton;ghostButton_->setObjectName("ghostComparison");ghostButton_->setCheckable(true);ghostButton_->setChecked(false);ghostButton_->setIcon(editorButtonIcon(":/icons/ghost.png",true,":/icons/ghost_off.png"));ghostButton_->setIconSize({24,24});ghostButton_->setFixedSize(30,30);ghostButton_->setAccessibleName(tr("Ghost comparison"));
    ghostButton_->setProperty("neutralToggle",true);
    ghostButton_->setToolTip(tr("Freeze currently visible points as a faint white ghost with a soft outline. Timeline and transform changes leave the copy fixed. Switch off to remove it."));displayLayout->addWidget(ghostButton_,0,Qt::AlignHCenter);
    ghostOpacitySlider_=new QSlider(Qt::Horizontal);ghostOpacitySlider_->setObjectName("ghostOpacity");ghostOpacitySlider_->setRange(0,100);ghostOpacitySlider_->setValue(15);ghostOpacitySlider_->setEnabled(false);
    ghostOpacitySlider_->setAccessibleName(tr("Ghost opacity"));ghostOpacitySlider_->setToolTip(tr("Adjust the frozen ghost's opacity."));ghostOpacitySlider_->setMinimumHeight(16);
    ghostOpacitySlider_->setStyleSheet("QSlider::groove:horizontal {height: 4px; background: #3b3b3b; border: 1px solid #626262; border-radius: 2px;}"
        "QSlider::sub-page:horizontal {background: #aaaaaa; border-radius: 2px;} QSlider::handle:horizontal {width: 9px; margin: -4px 0; background: #eeeeee; border: 1px solid #999999; border-radius: 3px;}"
        "QSlider::sub-page:horizontal:disabled {background: #555555;} QSlider::handle:horizontal:disabled {background: #606060; border-color: #777777;}");displayLayout->addWidget(ghostOpacitySlider_);
    connect(ghostOpacitySlider_,&QSlider::valueChanged,this,[this](int value) {viewport_->setGhostOpacity(value/100.0f);});
    connect(ghostButton_,&QToolButton::toggled,this,[this](bool checked) {const bool active=viewport_->setGhost(checked);QSignalBlocker blocker(ghostButton_);ghostButton_->setChecked(active);ghostOpacitySlider_->setEnabled(active && loaded_ && !loading_);if (checked && !active) statusBar()->showMessage(tr("No visible points to freeze."),5000);viewport_->setFocus();});
    connect(viewport_,&Viewport::ghostChanged,this,[this](bool active) {QSignalBlocker blocker(ghostButton_);ghostButton_->setChecked(active);ghostOpacitySlider_->setEnabled(active && loaded_ && !loading_);});
    viewport_->setDisplayControls(displayControls_);
    auto *note = new QLabel(tr("Projects save the capture reference, transform and view.")); note->setWordWrap(true); side->addWidget(note);
    auto *toolsScroll = new QScrollArea; toolsScroll->setWidgetResizable(true); toolsScroll->setFrameShape(QFrame::NoFrame);
    toolsScroll->setObjectName("toolsScrollArea");
    toolsScroll->setWidget(tools_);
    dock->setWidget(toolsScroll); dock->setMinimumWidth(450); addDockWidget(Qt::RightDockWidgetArea, dock);
    auto *viewMenu = menuBar()->addMenu(tr("View")); viewMenu->addAction(dock->toggleViewAction());
    auto *density=viewMenu->addAction(tr("Compact controls on smaller screens"));density->setObjectName("compactInterfaceDensity");density->setCheckable(true);density->setChecked(settings_.value("Interface/CompactDensity",true).toBool());
    density->setToolTip(tr("Reduce control sizes and spacing on smaller screens while preserving readable text and native Windows DPI. Changes apply at the next launch."));
    connect(density,&QAction::toggled,this,[this](bool compact) {settings_.setValue("Interface/CompactDensity",compact);statusBar()->showMessage(tr("Interface density preference saved. Restart the editor to apply it."),7000);});
    gridAction_ = viewMenu->addAction(tr("Grid and axes")); gridAction_->setObjectName("gridAndAxes"); gridAction_->setCheckable(true);
    gridAction_->setShortcut(QKeySequence("Shift+G")); gridAction_->setToolTip(tr("Toggle the world grid and reference axes (Shift+G)."));
    connect(gridAction_, &QAction::toggled, this, [this](bool value) { if (!syncing_) { project_.grid = value; settings_.setValue("Display/Grid",value); viewport_->setGrid(value); dirty(); } });
    auto *frameAction = viewMenu->addAction(tr("Focus visible"),this,&MainWindow::fitCurrentTarget);
    frameAction->setShortcuts({QKeySequence(Qt::KeypadModifier|Qt::Key_Delete),QKeySequence(Qt::KeypadModifier|Qt::Key_Period),QKeySequence(Qt::KeypadModifier|Qt::Key_Comma),QKeySequence("F")});
    frameAction->setToolTip(tr("Focus visible capture and ghost points (Numpad decimal / Numpad Del / F)."));
    auto *standardViews = viewMenu->addMenu(tr("Standard views"));
    const QString viewNames[] = {tr("Perspective"),tr("Front (Numpad 1)"),tr("Back (Ctrl+Numpad 1)"),tr("Left (Ctrl+Numpad 3)"),tr("Right (Numpad 3)"),tr("Top (Numpad 7)"),tr("Bottom (Ctrl+Numpad 7)")};
    for (int i=0; i<7; ++i) standardViews->addAction(viewNames[i],this,[this,i] { viewport_->setViewPreset(ViewPreset(i)); viewport_->setFocus(); });
    auto *helpMenu = menuBar()->addMenu(tr("Help"));
    helpMenu->addAction(tr("About VGS Editor"), this, [this] {
        QMessageBox dialog(this);
        dialog.setWindowTitle(tr("About VGS Editor"));
        dialog.setIconPixmap(QPixmap(":/icons/logo.png").scaled(64,64,Qt::KeepAspectRatio,Qt::SmoothTransformation));
        dialog.setTextFormat(Qt::RichText);
        dialog.setText(tr("<h2>VGS Editor</h2><p>4D Gaussian capture editor</p>"
                          "<p>Víctor M. Feliz</p>"
                          "<p>The4DScanner · ScanMeNow</p>"));
        dialog.exec();
    });
    helpMenu->addAction(tr("About Qt"), qApp, &QApplication::aboutQt);
    connect(slider_, &RangeSlider::playheadChanged, this, [this](int f) { if (!syncing_) { play(false); setTime(f/info_.fps, true); } });
    connect(slider_, &RangeSlider::rangeChanged, this, [this](int first,int last,int preview) {
        if (syncing_ || !loaded_) return; play(false); project_.in=first/info_.fps; project_.out=last/info_.fps;
        setTime(preview/info_.fps,true); syncUi(); dirty();
    });
    connect(timelineSecondsButton_,&QToolButton::toggled,this,[this](bool seconds) {if (syncing_) return;settings_.setValue("Playback/SecondsDisplay",seconds);syncUi();});
    connect(frameSpin_, &QDoubleSpinBox::valueChanged, this, [this](double value) { if (!syncing_) { play(false); setTime(timelineFrame(value)/info_.fps, true);syncUi(); } });
    connect(inFrame_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (syncing_) return; play(false); project_.in = timelineFrame(value)/info_.fps; project_.out = std::max(project_.out, project_.in);
        setTime(std::max(project_.time, project_.in), true); syncUi(); dirty();
    });
    connect(outFrame_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (syncing_) return; play(false); project_.out = timelineFrame(value)/info_.fps; project_.in = std::min(project_.in, project_.out);
        setTime(std::min(project_.time, project_.out), true); syncUi(); dirty();
    });
    connect(loop_, &QCheckBox::toggled, this, [this](bool value) { if (!syncing_) { project_.loop = value; settings_.setValue("Playback/Loop",value); dirty(); } });
    connect(pointSize_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (!syncing_) { project_.pointSize = value; settings_.setValue("Display/PointSize",value); viewport_->setPointSize(float(value)); dirty(); }
    });
    connect(speed_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (syncing_) return; const bool playing = playback_.isActive(); play(false); project_.speed = value;
        settings_.setValue("Playback/Speed",value); dirty(); if (playing) play(true);
    });
    auto *space = new QShortcut(QKeySequence(Qt::Key_Space), this); connect(space, &QShortcut::activated, this, [this] { play(!playback_.isActive()); });
    auto *prev = new QShortcut(QKeySequence(Qt::Key_Left), this); connect(prev, &QShortcut::activated, this, [this] { if (loaded_) { play(false); setTime(project_.time-1.0/info_.fps,true); } });
    auto *next = new QShortcut(QKeySequence(Qt::Key_Right), this); connect(next, &QShortcut::activated, this, [this] { if (loaded_) { play(false); setTime(project_.time+1.0/info_.fps,true); } });
    for (int g=0; g<3; ++g) {
        auto *shortcut = new QShortcut(QKeySequence(shortcuts[g]),this);
        shortcut->setAutoRepeat(false);
        connect(shortcut,&QShortcut::activated,this,[this,g] { activateTransformShortcut(TransformMode(g+1)); });
    }
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape),this);
    connect(escape,&QShortcut::activated,this,[this] { viewport_->setTransformMode(TransformMode::None); });
    auto shortcut = [&](const QString &key,auto action) { auto *s = new QShortcut(QKeySequence(key),this); s->setAutoRepeat(false); connect(s,&QShortcut::activated,this,action); };
    shortcut("Ctrl+Home",[this] { play(false); setTime(project_.in,true); });
    shortcut("Ctrl+End",[this] { play(false); setTime(project_.out,true); });
    shortcut("Alt+Home",[this] { resetTransform(); });
    shortcut("Tab",[this] { editCrop(!viewport_->cropEditing()); viewport_->setFocus(); });
    // 1 and 2 on the main keyboard switch 3D points / Gaussian; the numpad keeps its views.
    shortcut("1",[this] { renderStyle_->setCurrentIndex(0); });
    shortcut("2",[this] { renderStyle_->setCurrentIndex(1); });
    qApp->installEventFilter(new NumpadDigitsAreNotShortcuts(this));
    shortcut("C",[this] { editCrop(!viewport_->cropEditing()); });
    shortcut("Ctrl+F",[this] { fitCrop(); });
    shortcut("Ctrl+Shift+C",[this] { clearCrop(); });
    shortcut("Ctrl+Alt+1",[this] { applyCropPreset(1.5f); });
    shortcut("Ctrl+Alt+2",[this] { applyCropPreset(1.0f); });
    shortcut("Ctrl+Shift+P",[this] { savePreset(); });
    shortcut("Ctrl+Alt+P",[this] { openPresetFolder(); });
    shortcut("Ctrl+M",[this] { editCaptureSettings(); });
    shortcut("L",[this] { if (loaded_ && !loading_) loop_->toggle(); });
    for (int g=0; g<3; ++g) shortcut(QString("F%1").arg(g+6),[this,g] { toggleCoordinateSpace(g); });
    statusBar()->showMessage(tr("Open a capture to begin."));
}

void MainWindow::syncModifiers() {
    viewport_->setModifiers(project_.modifiersAtFrame(currentFrame()));modifierPanel_->setProject(project_);
    const bool purge=!CompiledModifiers(project_).isolations.isEmpty();
    QJsonObject state;if (purge) state={{"modifiers",project_.modifierJson()},{"transform",project_.json({})["transform"]},{"cropEditing",viewport_->cropEditing()}};
    if (state!=processingState_) {processingState_=state;if (purge) requestFrame();}
}
// The Walk speed in the unit its modifier shows: range, suffix and value, without
// writing anything back.
void MainWindow::showWalkSpeed(const Modifier &m) {
    const bool kmh=m.walkKmh,was=syncing_;syncing_=true;
    QSignalBlocker blocker(walkUnits_);walkUnits_->setChecked(kmh);
    walkSpeed_->setRange(0,kmh ? 360 : 100);walkSpeed_->setSuffix(kmh ? " km/h" : " m/s");walkSpeed_->setValue(kmh ? m.walkSpeed*3.6 : m.walkSpeed);
    syncing_=was;
}
void MainWindow::revealModifierProperties() {
    QTimer::singleShot(0,this,[this] {
        const auto *m=project_.modifier();auto *scroll=findChild<QScrollArea *>("toolsScrollArea");if (!m || !scroll) return;
        QWidget *panel=m->type==ModifierType::AnimateTransform ? static_cast<QWidget *>(animationProperties_) : m->type==ModifierType::PurgeIsolated ? isolationProperties_ : m->type==ModifierType::Walk ? walkProperties_ : m->type==ModifierType::BakeAntialiasing ? bakeProperties_ : m->type==ModifierType::Crop ? cropProperties_ : greenProperties_;
        scroll->ensureWidgetVisible(panel,0,12);
    });
}
void MainWindow::alignTimeline() {
    const auto [left,width]=modifierPanel_->trackSpan();
    const int start=left-slider_->mapToGlobal(QPoint(0,0)).x();
    slider_->setTrackInsets(start,slider_->width()-start-width);
    // The timeline reads as the first row of the list below: the capture's own track.
    slider_->setLabel(tr("Capture"),modifierPanel_->nameLeft()-slider_->mapToGlobal(QPoint(0,0)).x());
}
int MainWindow::currentFrame() const {return int(std::round(project_.time*info_.fps));}
void MainWindow::keyCrop() {
    auto *m=project_.modifier();
    if (!m || m->type!=ModifierType::Crop || !m->cropAnimation.animated) return;
    m->cropAnimation.setKey(currentFrame(),m->crop);showCropKeys(*m);
}
void MainWindow::setCropAnimated(bool animated) {
    auto *m=project_.modifier();if (!m || m->type!=ModifierType::Crop || m->cropAnimation.animated==animated) return;
    auto &a=m->cropAnimation;
    if (animated) {
        // The static pose is set aside; a first key starts the animation where the crop is.
        a.still=m->crop;a.animated=true;
        if (a.keys.isEmpty()) a.setKey(currentFrame(),m->crop);
        m->crop=a.evaluate(m->crop,currentFrame());
    } else {m->crop=m->staticCrop();a.animated=false;}
    viewport_->setCrop(m->crop);syncUi();dirty();
}
void MainWindow::showCropKeys(const Modifier &m) {
    auto text=[](QVector3D v) {return QString("%1, %2, %3").arg(v.x(),0,'g',5).arg(v.y(),0,'g',5).arg(v.z(),0,'g',5);};
    const auto &keys=m.cropAnimation.keys;const bool box=m.crop.shape==CropShape::Box;const int frame=currentFrame();
    QSignalBlocker blocker(cropKeyTable_);cropKeyTable_->setRowCount(int(keys.size()));
    for (int row=0;row<keys.size();++row) {
        const auto &key=keys[row];
        const QString size=box ? tr("W %1 · D %2 · H %3").arg(key.width,0,'g',4).arg(key.depth,0,'g',4).arg(key.height,0,'g',4)
                               : tr("Rx %1 · Rz %2 · H %3").arg(key.radius,0,'g',4).arg(key.radiusZ,0,'g',4).arg(key.height,0,'g',4);
        const QString values[]={QString::number(key.frame),text(key.transform.position),text(key.transform.rotation),size};
        for (int col=0;col<4;++col) {
            auto *item=cropKeyTable_->item(row,col);if (!item) {item=new QTableWidgetItem;cropKeyTable_->setItem(row,col,item);}
            if (item->text()!=values[col]) item->setText(values[col]);
            item->setFlags(col==0 ? (item->flags()|Qt::ItemIsEditable) : (item->flags()&~Qt::ItemIsEditable));
        }
        if (key.frame==frame) cropKeyTable_->setCurrentCell(row,0);
    }
    // An animated crop keeps one key at least; Static is how it stops animating.
    cropRemoveKey_->setEnabled(keys.size()>1);
    cropRemoveKey_->setToolTip(keys.size()>1 ? tr("Remove the selected key, or the key at the current frame.") : tr("An animated crop keeps at least one key. Choose Static to stop animating it; the keys are kept."));
    cropKeyTable_->setFixedHeight(std::clamp(cropKeyTable_->horizontalHeader()->height()+4+28*std::max(1,int(keys.size())),100,220));
}
void MainWindow::syncUi() {
    syncing_ = true;
    // Animated crops show their pose at this frame. Edits are keyed before they get here.
    project_.showCropsAtFrame(currentFrame());
    if (project_.modifier() && project_.modifier()->type==ModifierType::Crop && project_.modifier()->cropAnimation.active()) viewport_->setCrop(project_.crop());
    syncModifiers();
    const auto *selected=project_.modifier();const bool selectedCrop=selected && selected->type==ModifierType::Crop;
    parametersHeading_->setVisible(selected!=nullptr);
    cropProperties_->setVisible(selectedCrop);greenProperties_->setVisible(selected && selected->type==ModifierType::RemoveGreen);
    const bool animation=selected && selected->type==ModifierType::AnimateTransform,isolation=selected && selected->type==ModifierType::PurgeIsolated;
    animationProperties_->setVisible(animation);isolationProperties_->setVisible(isolation);
    const bool walk=selected && selected->type==ModifierType::Walk;walkProperties_->setVisible(walk);
    const bool bake=selected && selected->type==ModifierType::BakeAntialiasing;bakeProperties_->setVisible(bake);
    if (bake) {bakeProperties_->setTitle(tr("Bake anti-aliasing: %1").arg(selected->name));bakeDistance_->setValue(selected->bakeDistance);bakeScreenHeight_->setValue(selected->bakeScreenHeight);
        bakeSizeLabel_->setText(tr("%1 mm").arg(selected->bakeSize()*1000,0,'f',2));}
    if (walk) {walkProperties_->setTitle(tr("Walk: %1").arg(selected->name));showWalkSpeed(*selected);}
    if (animation) {animationProperties_->setTitle(tr("Animate transform: %1").arg(selected->name));animationProperties_->setAnimation(selected->animation,int(std::round(project_.time*info_.fps)),std::max(0,info_.frames-1));}
    if (isolation) {isolationProperties_->setTitle(tr("Purge Isolated: %1").arg(selected->name));isolationNeighbour_->setValue(selected->isolation.neighbour);isolationPercent_->setValue(selected->isolation.medianPercent);}
    viewport_->setTransform(project_.transformAtFrame(std::round(project_.time*info_.fps)));
    displayControls_->setEnabled(true);
    ghostButton_->setEnabled(loaded_ && !loading_);
    ghostOpacitySlider_->setEnabled(viewport_->ghostEnabled() && loaded_ && !loading_);
    if (selected) {cropProperties_->setTitle(tr("Crop: %1").arg(selected->name));greenProperties_->setTitle(tr("Remove green: %1").arg(selected->name));greenSaturation_->setValue(selected->green.minimumSaturation*100);greenHue_->setValue(selected->green.hueTolerance);greenLinearRgb_->setChecked(selected->green.linearRgb);}
    tools_->setEnabled(true); timeline_->setEnabled(loaded_ && !loading_);
    for (auto *group : tools_->findChildren<QGroupBox *>(QString(),Qt::FindDirectChildrenOnly))
        group->setEnabled(group==presetBox_ || (loaded_ && !loading_ && !(animation && !selected->enabled && group->property("transformGroup").toBool())));
    resetTransformButton_->setEnabled(loaded_ && !loading_ && !(animation && !selected->enabled));
    // A crop's size is its radius, width, depth and height: scaling it is not a separate value.
    for (int a=0; a<3; ++a) transform_[2][a]->setEnabled(!viewport_->cropEditing());
    captureSettingsButton_->setEnabled(loaded_ && !loading_);
    savePresetButton_->setEnabled(loaded_ && !loading_); presetCombo_->setEnabled(loaded_ && !loading_ && presetCombo_->count()>1);
    presetFolderButton_->setEnabled(loaded_ && !loading_);
    saveAction_->setEnabled(loaded_ && !loading_); saveAsAction_->setEnabled(loaded_ && !loading_); imageAction_->setEnabled(loaded_ && !loading_); exportAction_->setEnabled(loaded_ && !loading_); plyAction_->setEnabled(loaded_ && !loading_);
    assetLabel_->setText(loaded_ ? info_.title : tr("No capture"));
    assetLabel_->setToolTip(project_.asset);
    metadata_->setText(loaded_ ? tr("%1 · %2 fps\n%3 s · %4 frames").arg(info_.format).arg(info_.fps,0,'f',2).arg(info_.duration,0,'f',3).arg(info_.frames) : QString());
    const auto target = viewport_->displayedTransform();
    const QVector3D vectors[] = {target.position,target.rotation,target.scale};
    for (int g=0; g<3; ++g) for (int a=0; a<3; ++a) transform_[g][a]->setValue(vectors[g][a]);
    const int maximum = std::max(0, info_.frames-1);
    slider_->setFrameRange(0,maximum); slider_->setFrameRate(info_.fps); slider_->setRangeValues(int(std::round(project_.in*info_.fps)),int(std::round(project_.out*info_.fps)));
    const bool seconds=timelineSecondsButton_->isChecked();const double divisor=seconds ? info_.fps : 1.0;
    const int decimals=seconds ? std::max(3,int(std::ceil(std::log10(std::max(1.0,info_.fps))))+1) : 0;
    for (auto *field:{frameSpin_,inFrame_,outFrame_}) {field->setDecimals(decimals);field->setRange(0,maximum/divisor);field->setSingleStep(1.0/divisor);field->setSuffix(seconds ? tr(" s") : QString());}
    frameSpin_->setPrefix(seconds ? tr("Time ") : tr("Frame "));
    frameSpin_->setSuffix(seconds ? tr(" of %1 s").arg(info_.duration,0,'f',decimals) : tr(" of %1").arg(info_.frames));
    inFrame_->setPrefix(tr("Start "));outFrame_->setPrefix(tr("End "));
    inFrame_->setValue(std::round(project_.in*info_.fps)/divisor);outFrame_->setValue(std::round(project_.out*info_.fps)/divisor);
    const int frame = int(std::round(project_.time*info_.fps)); slider_->setPlayheadValue(frame); frameSpin_->setValue(frame/divisor);
    bool timelineWidthChanged=false;
    for (auto *field:{frameSpin_,inFrame_,outFrame_}) {
        field->ensurePolished();const int width=field->sizeHint().width();
        if (field->minimumWidth()!=width) {field->setMinimumWidth(width);timelineWidthChanged=true;}
    }
    if (timelineWidthChanged) {frameSpin_->parentWidget()->layout()->activate();timeline_->layout()->invalidate();timeline_->layout()->activate();}
    frameSpin_->setToolTip(seconds ? tr("Current time of the full capture duration. Entered seconds snap to the nearest valid frame within Start/End.") : tr("Current zero-based frame index of %1 total frames. Seeking stays within Start/End.").arg(info_.frames));
    inFrame_->setToolTip(tr("First included frame of the playback/export range. Seconds snap to the nearest valid frame."));outFrame_->setToolTip(tr("Last included frame of the playback/export range. Seconds snap to the nearest valid frame."));
    timelineSecondsButton_->setToolTip(seconds ? tr("Display time in seconds. Click to display frame indices. Entries snap to the nearest valid frame.") : tr("Display frame indices. Click to display time in seconds. Entries snap to the nearest valid frame."));
    modifierPanel_->setTimeline(frame,maximum);
    viewport_->setPlaybackTime(project_.time,info_.duration);
    viewport_->setFloorScroll(project_.walkSpeed()>0,project_.walkDistance(project_.time));
    speed_->setValue(project_.speed); loop_->setChecked(project_.loop); gridAction_->setChecked(project_.grid);
    syncTransformButtons();
    // The group title names what the fields edit: the capture, or the selected crop/animation modifier.
    transformBox_->setTitle(tr("Transform · %1").arg(viewport_->cropEditing() || animation ? (selected ? selected->name : tr("Crop")) : tr("Capture")));
    for (int g=0; g<3; ++g) {
        const bool local = (g==2 && viewport_->cropEditing()) || project_.spaces[g]==CoordinateSpace::Local;
        spaceButtons_[g]->setText(local ? tr("Local") : tr("Global"));
        // This single button displays the chosen reference space; both choices are active selections.
        spaceButtons_[g]->setIcon(editorButtonIcon(local ? ":/icons/local.png" : ":/icons/global.png",false));
        spaceButtons_[g]->setAccessibleName(local ? tr("Local reference space") : tr("Global reference space"));
        const QString modeKeys[]={"G","R","S"};
        spaceButtons_[g]->setToolTip(tr("Toggle Global/Local reference space (F%1 or repeat %2 in this mode). Current: %3.").arg(g+6).arg(modeKeys[g],local ? tr("Local") : tr("Global")));
    }
    cropEditButton_->setChecked(viewport_->cropEditing());
    cropRadius_->setValue(project_.crop().radius); cropRadiusZ_->setValue(project_.crop().radiusZ); cropHeight_->setValue(project_.crop().height);
    cropShapeCombo_->setCurrentIndex(cropShapeCombo_->findData(int(project_.crop().shape)));
    cropModeCombo_->setCurrentIndex(cropModeCombo_->findData(project_.crop().remove ? 1 : 0));
    {
        const auto *m=project_.modifier();const bool animatedCrop=m && m->type==ModifierType::Crop && m->cropAnimation.animated;
        cropAnimationCombo_->setCurrentIndex(animatedCrop ? 1 : 0);cropForm_->setRowVisible(cropKeys_,animatedCrop);
        if (animatedCrop) showCropKeys(*m);
    }
    cropPreviewCombo_->setCurrentIndex(cropPreviewCombo_->findData(project_.crop().showRemovedInRed ? 1 : 0));
    cropWidth_->setValue(project_.crop().width); cropDepth_->setValue(project_.crop().depth);
    cropForm_->setRowVisible(cropRadius_,project_.crop().shape==CropShape::Cylinder); cropForm_->setRowVisible(cropRadiusZ_,project_.crop().shape==CropShape::Cylinder);
    cropForm_->setRowVisible(cropWidth_,project_.crop().shape==CropShape::Box); cropForm_->setRowVisible(cropDepth_,project_.crop().shape==CropShape::Box);
    cropWidth_->setEnabled(selectedCrop); cropDepth_->setEnabled(selectedCrop);
    cropRadius_->setEnabled(selectedCrop); cropRadiusZ_->setEnabled(selectedCrop); cropHeight_->setEnabled(selectedCrop); cropClearButton_->setEnabled(project_.crop().enabled);
    cropStatus_->setText(!project_.crop().enabled ? tr("Modifier disabled. Settings are retained; Edit adjusts this volume.") : viewport_->cropEditing()
        ? (project_.crop().showRemovedInRed ? tr("Editing the selected crop, colour coded: kept points lightened, deleted points red.") : tr("Editing the selected crop: what the crops would delete is hidden."))
        : tr("Keep crops preserve what is inside any of them; Remove crops delete what is inside them and win where they overlap, over the full timeline."));
    pointSize_->setValue(project_.pointSize); viewport_->setPointSize(float(project_.pointSize)); viewport_->setGrid(project_.grid);
    syncing_ = false;
}
void MainWindow::title() {
    setWindowTitle(tr("%1[*] — VGS Editor").arg(projectPath_.isEmpty() ? tr("Untitled project") : QFileInfo(projectPath_).fileName()));
}
void MainWindow::dirty() { if (loaded_) setWindowModified(true); }
bool MainWindow::canDiscard() {
    if (!isWindowModified()) return true;
    const auto answer = QMessageBox::question(this, tr("Unsaved changes"), tr("Save changes to this project?"), QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel) return false;
    return answer != QMessageBox::Save || save();
}
bool MainWindow::save(bool saveAs) {
    if (!loaded_ || loading_) return false;
    QString path = projectPath_;
    if (path.isEmpty() || saveAs) {
        const QString initial = path.isEmpty() ? history_.savePath("Project",QFileInfo(project_.asset).completeBaseName()+".vgsproj") : path;
        path = QFileDialog::getSaveFileName(this, tr("Save project"), initial, tr("VGS project (*.vgsproj)"));
    }
    if (path.isEmpty()) return false;
    if (!path.endsWith(".vgsproj", Qt::CaseInsensitive)) path += ".vgsproj";
    project_.camera = viewport_->camera(); QString error;
    if (!project_.write(path, &error)) { showError(error); return false; }
    projectPath_ = QFileInfo(path).absoluteFilePath(); history_.remember(projectPath_); updateRecentMenu();
    setWindowModified(false); title(); statusBar()->showMessage(tr("Project saved."),3000); return true;
}
void MainWindow::newProject() {
    if (loading_ || !canDiscard()) return;
    play(false); loaded_ = decoding_ = pendingDecode_ = false; generation_ = ++serial_;
    viewport_->setTransformMode(TransformMode::None);
    viewport_->setCropEditing(false); viewport_->setCrop({});
    project_ = defaultProject(); info_ = {}; projectPath_.clear(); viewport_->setFrame({}); viewport_->setTransform({});
    for (int g=0; g<3; ++g) viewport_->setCoordinateSpace(TransformMode(g+1),project_.spaces[g]);
    viewport_->setCamera(project_.camera); setWindowModified(false); refreshPresets(); syncUi(); title();
    // Release the source and its caches in their owning thread.
    QMetaObject::invokeMethod(worker_, &CaptureWorker::clear, Qt::QueuedConnection);
}
void MainWindow::openPath(const QString &path) {
    if (loading_ || !canDiscard()) return;
    pendingProject_.reset(); pendingProjectPath_.clear(); QString asset = path;
    if (QFileInfo(path).suffix().compare("vgsproj", Qt::CaseInsensitive) == 0) {
        Project p; QString error;
        if (!Project::read(path, &p, &error)) { showError(error); return; }
        pendingProject_ = p; pendingProjectPath_ = QFileInfo(path).absoluteFilePath(); asset = p.asset;
    }
    play(false); viewport_->setTransformMode(TransformMode::None); viewport_->setCropEditing(false); loading_ = true; openingGeneration_ = ++serial_; syncUi();
    statusBar()->showMessage(tr("Loading %1…").arg(QFileInfo(asset).fileName()));
    emit openRequested(asset, openingGeneration_, true);
}
int MainWindow::timelineFrame(double displayedValue) const {
    return int(std::clamp(std::round(displayedValue*(timelineSecondsButton_->isChecked() ? info_.fps : 1.0)),0.0,double(std::max(0,info_.frames-1))));
}
void MainWindow::setTime(double seconds, bool edited) {
    if (!loaded_ || loading_) return;
    seconds = std::clamp(std::round(seconds*info_.fps)/info_.fps, project_.in, project_.out);
    if (std::abs(project_.time-seconds) < 1e-8) return;
    project_.time = seconds; syncUi(); if (edited) dirty(); requestFrame();
}
void MainWindow::requestFrame() {
    if (!loaded_ || loading_) return;
    if (decoding_) { pendingDecode_ = true; return; }
    Project snapshot=project_;if (viewport_->cropEditing()) for (auto &m:snapshot.modifiers) if (m.type==ModifierType::Crop) m.enabled=false;
    decoding_ = true; emit decodeRequested(project_.time, generation_, true,snapshot);
}
void MainWindow::play(bool playing) {
    if (playing && (!loaded_ || loading_)) return;
    if (playing) {
        if (project_.time >= project_.out) setTime(project_.in,false);
        playStart_ = project_.time; clock_.restart(); playback_.start();
    } else playback_.stop();
    playButton_->setIcon(transportIcon(style(),playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
}
void MainWindow::receiveFrame(FramePtr frame) {
    viewport_->setFrame(frame);
    if (smokeOutput_.isEmpty()) return;
    qInfo("%s: %llu points / %llu records, t=%.3f, decode %.2f ms, SH=%d",
          qPrintable(info_.format), qulonglong(frame->points.size()), qulonglong(frame->records.size()), frame->seconds, frame->decodeMs, frame->coefficients);
    if (smokeStage_ == 0) {
        smokeStage_ = 1;
        project_.transform.position = {0.2f,0.1f,0}; project_.transform.rotation = {0,20,0}; project_.transform.scale = {1.1f,1.1f,1.1f};
        viewport_->setTransform(project_.transform); viewport_->fit(info_.minimum,info_.maximum);
        project_.camera = viewport_->camera(); project_.time = project_.out*0.5;
        project_.time = std::round(project_.time*info_.fps)/info_.fps;
        QString error; const QString path = smokeOutput_ + ".vgsproj";
        Project roundtrip;
        if (!project_.write(path,&error) || !Project::read(path,&roundtrip,&error) ||
            roundtrip.transform.position != project_.transform.position || roundtrip.asset != project_.asset) { showError(error); return; }
        syncUi(); requestFrame(); return;
    }
    if (smokeStage_ == 1) {
        smokeStage_ = 2;
        setTransformMode(TransformMode::Move);
        QTimer::singleShot(500,this,[this] {
            const QImage image = viewport_->grabFramebuffer();
            if (!viewport_->renderError().isEmpty() || image.isNull() || !image.save(smokeOutput_)) { qCritical("Viewport smoke test failed"); qApp->exit(2); return; }
            const bool ok = grab().save(smokeOutput_ + ".ui.png");
            const auto liveTransform=viewport_->transform();const auto liveCamera=viewport_->camera();
            if (!viewport_->setGhost(true)) {qCritical("Ghost snapshot failed");qApp->exit(2);return;}
            auto comparison=liveTransform;comparison.position.setX(comparison.position.x()+0.18f);viewport_->setTransform(comparison);
            viewport_->focusVisible();auto closeCamera=viewport_->camera();closeCamera.distance*=0.55f;viewport_->setCamera(closeCamera);
            if (!viewport_->grabFramebuffer().save(smokeOutput_+".ghost.png") || !grab().save(smokeOutput_+".ghost.ui.png")) {qApp->exit(2);return;}
            viewport_->setTransform(liveTransform);viewport_->setCamera(liveCamera);project_.camera=liveCamera;viewport_->setGhost(false);
            const bool seconds=timelineSecondsButton_->isChecked();timelineSecondsButton_->setChecked(!seconds);
            QCoreApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);
            if (!grab().save(smokeOutput_+".alternate-time.ui.png")) {qApp->exit(2);return;}
            timelineSecondsButton_->setChecked(seconds);
            auto *modifierType=modifierPanel_->findChild<QComboBox *>("newModifierType");modifierType->showPopup();
            QCoreApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);
            auto *popupView=modifierType->view();const auto hoverPosition=popupView->visualRect(modifierType->model()->index(2,0)).center();
            QMouseEvent hover(QEvent::MouseMove,QPointF(hoverPosition),QPointF(popupView->viewport()->mapToGlobal(hoverPosition)),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
            QApplication::sendEvent(popupView->viewport(),&hover);
            if (!popupView->window()->grab().save(smokeOutput_+".dropdown-hover.png")) {qApp->exit(2);return;}
            modifierType->hidePopup();
            QStyleOptionButton buttonOption;buttonOption.initFrom(captureSettingsButton_);buttonOption.state|=QStyle::State_MouseOver;buttonOption.rect=captureSettingsButton_->rect();buttonOption.text=captureSettingsButton_->text();
            QPixmap hoverImage(captureSettingsButton_->size()*captureSettingsButton_->devicePixelRatioF());hoverImage.setDevicePixelRatio(captureSettingsButton_->devicePixelRatioF());hoverImage.fill(Qt::transparent);
            QPainter buttonPainter(&hoverImage);buttonPainter.setFont(captureSettingsButton_->font());captureSettingsButton_->style()->drawControl(QStyle::CE_PushButton,&buttonOption,&buttonPainter,captureSettingsButton_);buttonPainter.end();
            if (!hoverImage.save(smokeOutput_+".button-hover.png")) {qApp->exit(2);return;}
            QHelpEvent buttonTip(QEvent::ToolTip,captureSettingsButton_->rect().center(),captureSettingsButton_->mapToGlobal(captureSettingsButton_->rect().center()));
            QApplication::sendEvent(captureSettingsButton_,&buttonTip);
            for (auto *window:QApplication::topLevelWidgets()) if (window->windowType()==Qt::ToolTip)
                if (!window->grab().save(smokeOutput_+".button-tooltip.png")) {qApp->exit(2);return;}
            QToolTip::hideText();
            for (int g=0; g<3; ++g) {
                setTransformMode(TransformMode(g+1));
                for (int other=0; other<3; ++other) if (modeButtons_[other]->isChecked() != (other==g)) { qApp->exit(2); return; }
                if (!viewport_->grabFramebuffer().save(smokeOutput_+QString(".gizmo-%1.png").arg(g))) { qApp->exit(2); return; }
            }
            viewport_->setTransformMode(TransformMode::None);
            for (auto *button : modeButtons_) if (button->isChecked()) { qApp->exit(2); return; }
            for (int g=0; g<3; ++g) {
                toggleTransformMode(TransformMode(g+1));
                if (!modeButtons_[g]->isChecked()) { qApp->exit(2); return; }
                toggleTransformMode(TransformMode(g+1));
                if (viewport_->transformMode()!=TransformMode::None || modeButtons_[g]->isChecked()) { qApp->exit(2); return; }
            }
            const Transform captureTransform = project_.transform;
            if (findChild<QObject *>("projectToolbar")) { qApp->exit(2); return; }
            for (int g=0; g<3; ++g) {
                if (project_.spaces[g]!=CoordinateSpace::Global) { qApp->exit(2); return; }
                setTransformMode(TransformMode(g+1));
                for (int other=0; other<3; ++other) if (spaceButtons_[other]->isEnabled()!=(other==g)) { qApp->exit(2); return; }
                toggleCoordinateSpace(g);
                if (spaceButtons_[g]->isChecked() || viewport_->coordinateSpace(TransformMode(g+1))!=CoordinateSpace::Local) { qApp->exit(2); return; }
                toggleCoordinateSpace(g);
            }
            viewport_->setTransformMode(TransformMode::None);
            for (auto *button : spaceButtons_) if (button->isEnabled()) { qApp->exit(2); return; }
            applyCropPreset(1.5f);
            if (project_.crop().radius!=1.5f || project_.crop().height!=2.5f || project_.crop().transform.position!=QVector3D() || project_.crop().transform.scale!=QVector3D(1,1,1)) { qApp->exit(2); return; }
            project_.crop().transform.position = {1,2,3}; project_.crop().transform.rotation = {10,20,30}; project_.crop().transform.scale = {2,3,4};
            viewport_->setCrop(project_.crop()); applyCropPreset(1.0f);
            if (project_.crop().radius!=1.0f || project_.crop().height!=2.5f || project_.crop().transform.position!=QVector3D() || project_.crop().transform.rotation!=QVector3D() || project_.crop().transform.scale!=QVector3D(1,1,1)) { qApp->exit(2); return; }
            setTransformMode(TransformMode::Scale); fitCrop();
            if (viewport_->transformMode()!=TransformMode::Move || focusWidget()!=viewport_ || !viewport_->cropEditing()) { qApp->exit(2); return; }
            toggleTransformMode(TransformMode::Scale);
            if (!modeButtons_[2]->isChecked()) { qApp->exit(2); return; }
            setTransformMode(TransformMode::Move);
            editCrop(true);
            const double cropX = viewport_->displayedTransform().position.x()+0.2;
            transform_[0][0]->setValue(cropX); cropRadius_->setValue(project_.crop().radius*0.7);
            if (project_.transform.position!=captureTransform.position || std::abs(viewport_->displayedTransform().position.x()-cropX)>0.001) { qApp->exit(2); return; }
            viewport_->setViewPreset(ViewPreset::Front);
            if (!viewport_->grabFramebuffer().save(smokeOutput_+".crop()-edit.png")) { qApp->exit(2); return; }
            if (!grab().save(smokeOutput_+".crop()-edit.ui.png")) { qApp->exit(2); return; }
            cropShapeCombo_->setCurrentIndex(cropShapeCombo_->findData(int(CropShape::Box)));
            QMetaObject::invokeMethod(cropShapeCombo_,"activated",Qt::DirectConnection,Q_ARG(int,cropShapeCombo_->currentIndex()));
            if (project_.crop().shape!=CropShape::Box || !cropWidth_->isVisible() || cropRadius_->isVisible()) { qApp->exit(2); return; }
            if (!viewport_->grabFramebuffer().save(smokeOutput_+".box-edit.png")) { qApp->exit(2); return; }
            CaptureSettings options = project_.captureSettings; options.title = "Editor metadata smoke"; options.despill = true; options.plain = true; options.tags = {"test","crop"};
            CaptureSettingsDialog metadataDialog(project_,presetStore_,this); metadataDialog.setSettings(options);
            if (metadataDialog.settings().json()!=options.json() || !metadataDialog.grab().save(smokeOutput_+".metadata.ui.png")) { qApp->exit(2); return; }
            project_.captureSettings = options;
            editCrop(false);
            if (!viewport_->grabFramebuffer().save(smokeOutput_+".crop()-preview.png")) { qApp->exit(2); return; }
            const Project snapshot = project_; QString presetPath,presetError;
            if (!presetStore_.save("Smoke setup",snapshot,&presetPath,&presetError,PresetScope::Editor)) { showError(presetError); return; }
            refreshPresets(presetPath);
            project_.transform = {}; project_.crop() = {}; viewport_->setTransform({}); viewport_->setCrop({});
            const QString asset = project_.asset; const double time = project_.time;
            loadSelectedPreset();
            if (project_.asset!=asset || project_.time!=time || project_.transform.position!=snapshot.transform.position ||
                project_.crop().shape!=snapshot.crop().shape || project_.crop().radius!=snapshot.crop().radius || project_.captureSettings.title!=snapshot.captureSettings.title || viewport_->cropEditing()) { qApp->exit(2); return; }
            setTransformMode(TransformMode::Rotate);
            if (!grab().save(smokeOutput_+".preset-loaded.ui.png")) { qApp->exit(2); return; }
            viewport_->setTransformMode(TransformMode::None);
            QString cropError;
            if (!project_.write(smokeOutput_+".vgsproj",&cropError)) { showError(cropError); return; }
            const auto selectedModifier=project_.selectedModifier;
            for (const auto &m:project_.modifiers) if (m.type==ModifierType::AnimateTransform || m.type==ModifierType::PurgeIsolated) {
                project_.selectedModifier=m.id;syncUi();auto *scroll=findChild<QScrollArea *>("toolsScrollArea");QWidget *panel=m.type==ModifierType::AnimateTransform ? static_cast<QWidget *>(animationProperties_) : isolationProperties_;scroll->ensureWidgetVisible(panel,0,12);
                if (!grab().save(smokeOutput_+(m.type==ModifierType::AnimateTransform ? ".animation.ui.png" : ".isolation.ui.png"))) {qApp->exit(2);return;}
            }
            project_.selectedModifier=selectedModifier;syncUi();
            qInfo("Viewport smoke test: %s, %dx%d, project roundtrip OK", qPrintable(info_.format), image.width(),image.height());
            if (!ok) { qApp->exit(2); return; }
            setWindowModified(false); smokeStage_ = 3;
            newProject(); openPath(smokeOutput_ + ".vgsproj");
        });
    }
    else if (smokeStage_ == 3) {
        if (project_.transform.position != QVector3D(0.2f,0.1f,0) || !project_.crop().enabled || viewport_->cropEditing() ||
            project_.camera.preset!=ViewPreset::Front || !project_.camera.orthographic) { qApp->exit(2); return; }
        qInfo("New project and reopen saved crop/orthographic view OK"); setWindowModified(false); qApp->exit(0);
    }
}
void MainWindow::exportCapture() {
    if (!loaded_ || loading_) return;
    play(false);
    // The format is the extension chosen here; the dialog offers the last one used.
    const QString last=QFileInfo(settings_.value("Export/LastFile").toString()).suffix().toLower();
    const QString extension=last=="pgs" || last=="mint" ? last : "vgs";
    const QString suggested=QDir(settings_.value("Export/Directory",QFileInfo(project_.asset).absolutePath()).toString()).filePath(QFileInfo(project_.asset).completeBaseName()+"_edited."+extension);
    QString selectedFilter=extension=="pgs" ? tr("Plain Gaussian capture (*.pgs)") : extension=="mint" ? tr("Gracia MINT capture (*.mint)") : tr("Compressed Gaussian capture (*.vgs)");
    QString destination=QFileDialog::getSaveFileName(this,tr("Export capture"),suggested,
        tr("Compressed Gaussian capture (*.vgs);;Plain Gaussian capture (*.pgs);;Gracia MINT capture (*.mint)"),&selectedFilter);
    if (destination.isEmpty()) return;
    if (QFileInfo(destination).suffix().isEmpty()) destination+=selectedFilter.contains("*.mint") ? ".mint" : selectedFilter.contains("*.pgs") ? ".pgs" : ".vgs";
    const auto outputExtension=QFileInfo(destination).suffix().toLower();
    if (outputExtension!="vgs" && outputExtension!="pgs" && outputExtension!="mint") {
        showError(tr("Select a .vgs, .pgs or .mint destination."));return;
    }
    Project snapshot=project_;snapshot.captureSettings.plain=outputExtension=="pgs";
    const qint64 originalBytes=QFileInfo(snapshot.asset).size();
    QProgressDialog progress(tr("Preparing export"),tr("Cancel"),0,100,this);
    progress.setWindowTitle(tr("Export capture"));progress.setWindowModality(Qt::ApplicationModal);
    progress.setAutoClose(false);progress.setAutoReset(false);progress.setMinimumDuration(0);
    std::atomic_bool cancelled{false};QString failure;ExportResult result;
    connect(&progress,&QProgressDialog::canceled,&progress,[&] {cancelled=true;});
    auto *job=QThread::create([&] {
        try { result=exportCaptureFile(snapshot,destination,[&](int value,const QString &message) {
            if (cancelled) return false;
            QMetaObject::invokeMethod(&progress,[&,value,message] {progress.setValue(value);progress.setLabelText(message);},Qt::QueuedConnection);
            return !cancelled.load();
        }); }
        catch (const std::exception &e) {failure=QString::fromUtf8(e.what());}
    });
    connect(job,&QThread::finished,&progress,&QDialog::accept);
    job->start();progress.exec();cancelled = cancelled || progress.wasCanceled();job->wait();delete job;
    if (!failure.isEmpty()) {if (!cancelled) showError(failure);return;}
    settings_.setValue("Export/Directory",QFileInfo(destination).absolutePath());
    settings_.setValue("Export/LastFile",destination);
    settings_.setValue("CaptureSettings/Last",QJsonDocument(snapshot.captureSettings.json()).toJson(QJsonDocument::Compact));
    statusBar()->showMessage(tr("Exported %1 frames to %2").arg(result.frames).arg(destination),15000);
    QMessageBox::information(this,tr("Export complete"),tr("Saved %1\nOriginal size: %2 MB  \u00b7  Exported size: %3 MB\n%4 frames \u00b7 %5 Gaussian samples kept \u00b7 %6 cropped.\n\n%7")
        .arg(destination).arg(originalBytes/1000000.0,0,'f',2).arg(QFileInfo(destination).size()/1000000.0,0,'f',2)
        .arg(result.frames).arg(result.kept).arg(result.removed).arg(result.notes.join("\n")));
}

void MainWindow::exportFrame() {
    if (!loaded_ || loading_) return;
    play(false);
    const int frame=currentFrame();
    const QString suggested=QDir(settings_.value("Export/PlyDirectory",settings_.value("Export/Directory",QFileInfo(project_.asset).absolutePath())).toString())
        .filePath(QString("%1_%2.ply").arg(QFileInfo(project_.asset).completeBaseName()).arg(frame,6,10,QChar('0')));
    QString destination=QFileDialog::getSaveFileName(this,tr("Export current frame as PLY"),suggested,tr("Gaussian splat (*.ply)"));
    if (destination.isEmpty()) return;
    if (QFileInfo(destination).suffix().compare("ply",Qt::CaseInsensitive)!=0) destination+=".ply";
    const Project snapshot=project_;const double seconds=project_.time;
    QProgressDialog progress(tr("Preparing export"),tr("Cancel"),0,100,this);
    progress.setWindowTitle(tr("Export current frame"));progress.setWindowModality(Qt::ApplicationModal);
    progress.setAutoClose(false);progress.setAutoReset(false);progress.setMinimumDuration(400);
    std::atomic_bool cancelled{false};QString failure;ExportResult result;
    connect(&progress,&QProgressDialog::canceled,&progress,[&] {cancelled=true;});
    auto *job=QThread::create([&] {
        try { result=exportFramePly(snapshot,seconds,destination,[&](int value,const QString &message) {
            QMetaObject::invokeMethod(&progress,[&,value,message] {progress.setValue(value);progress.setLabelText(message);},Qt::QueuedConnection);
            return !cancelled.load();
        }); }
        catch (const std::exception &e) {failure=QString::fromUtf8(e.what());}
    });
    connect(job,&QThread::finished,&progress,&QDialog::accept);
    job->start();progress.exec();cancelled = cancelled || progress.wasCanceled();job->wait();delete job;
    if (!failure.isEmpty()) {if (!cancelled) showError(failure);return;}
    settings_.setValue("Export/PlyDirectory",QFileInfo(destination).absolutePath());
    statusBar()->showMessage(tr("Frame %1 saved to %2: %3 Gaussians, %4 removed by modifiers.").arg(frame).arg(destination).arg(result.kept).arg(result.removed),15000);
}
void MainWindow::exportImage() {
    QString path = QFileDialog::getSaveFileName(this,tr("Export image"),history_.savePath("Image",QFileInfo(project_.asset).completeBaseName()+".png"),tr("PNG image (*.png)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(".png",Qt::CaseInsensitive)) path += ".png";
    if (!viewport_->grabFramebuffer().save(path)) { showError(tr("Could not save the image.")); return; }
    history_.rememberImage(path); statusBar()->showMessage(tr("Image saved."),3000);
}
void MainWindow::showError(const QString &message) {
    if (!smokeOutput_.isEmpty()) { qCritical("%s",qPrintable(message)); setWindowModified(false); qApp->exit(2); }
    else QMessageBox::critical(this,tr("VGS Editor"),message);
}
void MainWindow::smokeTest(const QString &path, const QString &output) {
    smokeOutput_ = output; openPath(path);
    QTimer::singleShot(60000,this,[] { qCritical("Smoke test timed out"); qApp->exit(3); });
}
void MainWindow::closeEvent(QCloseEvent *event) {
    if (!canDiscard()) { event->ignore(); return; }
    settings_.setValue("geometry",saveGeometry()); settings_.setValue("windowState",saveState()); settings_.sync();
    event->accept();
}
void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1 && event->mimeData()->urls().first().isLocalFile()) event->acceptProposedAction();
}
void MainWindow::dropEvent(QDropEvent *event) {
    if (!event->mimeData()->urls().isEmpty()) openPath(event->mimeData()->urls().first().toLocalFile());
}
Project MainWindow::defaultProject() const {
    Project p;
    p.crop().enabled=true;
    const auto last = QJsonDocument::fromJson(settings_.value("CaptureSettings/Last").toByteArray()); QString error;
    if (last.isObject()) CaptureSettings::fromJson(last.object(),&p.captureSettings,&error);
    p.pointSize = std::clamp(settings_.value("Display/PointSize",5).toDouble(),1.0,12.0);
    p.grid = settings_.value("Display/Grid",true).toBool();
    p.loop = settings_.value("Playback/Loop",true).toBool();
    p.speed = std::clamp(settings_.value("Playback/Speed",1).toDouble(),0.1,4.0);
    return p;
}
void MainWindow::setTransformMode(TransformMode mode) {
    if (!loaded_ || loading_) return;
    viewport_->setTransformMode(mode);
}
void MainWindow::toggleTransformMode(TransformMode mode) {
    if (!loaded_ || loading_) return;
    if (project_.modifier() && project_.modifier()->type==ModifierType::AnimateTransform && !project_.modifier()->enabled) return;
    viewport_->toggleTransformMode(mode);
    syncTransformButtons();viewport_->setFocus(Qt::OtherFocusReason);
}
void MainWindow::activateTransformShortcut(TransformMode mode) {
    if (!loaded_ || loading_ || mode==TransformMode::None) return;
    if (project_.modifier() && project_.modifier()->type==ModifierType::AnimateTransform && !project_.modifier()->enabled) return;
    const auto previous=viewport_->chosenCoordinateSpace(mode);viewport_->activateTransformShortcut(mode);
    const auto space=viewport_->chosenCoordinateSpace(mode);project_.spaces[int(mode)-1]=space;
    syncUi();if (previous!=space) dirty();viewport_->setFocus(Qt::OtherFocusReason);
}
void MainWindow::toggleCoordinateSpace(int group) {
    if (!loaded_ || loading_ || (group==2 && viewport_->cropEditing())) return;
    if (viewport_->transformMode()!=TransformMode(group+1)) return;
    const auto space = project_.spaces[group]==CoordinateSpace::Global ? CoordinateSpace::Local : CoordinateSpace::Global;
    viewport_->setCoordinateSpace(TransformMode(group+1),space); project_.spaces[group] = space;
    syncUi(); dirty(); viewport_->setFocus(Qt::OtherFocusReason);
}
void MainWindow::resetTransform() {
    if (!loaded_ || loading_) return;
    if (project_.modifier() && project_.modifier()->type==ModifierType::AnimateTransform && !project_.modifier()->enabled) return;
    viewport_->setTransformMode(TransformMode::None);
    if (viewport_->cropEditing()) { project_.crop().transform = {}; keyCrop(); viewport_->setCrop(project_.crop()); }
    else if (project_.modifier() && project_.modifier()->type==ModifierType::AnimateTransform) project_.modifier()->animation.setKey(int(std::round(project_.time*info_.fps)),{});
    else { project_.transform = {}; viewport_->setTransform(project_.transform); }
    syncUi(); dirty(); viewport_->setFocus(Qt::OtherFocusReason);
}
void MainWindow::refreshPresets(const QString &requestedPath) {
    if (!presetCombo_) return;
    const QString selected = requestedPath;
    QSignalBlocker blocker(presetCombo_); presetCombo_->clear(); presetCombo_->addItem(tr("Load preset…"),QString());
    for (const auto &entry : presetStore_.list()) presetCombo_->addItem(entry.name,entry.path);
    const int index = presetCombo_->findData(selected); presetCombo_->setCurrentIndex(index>=0 ? index : 0);
    presetCombo_->setEnabled(loaded_ && !loading_ && presetCombo_->count()>1);
}
void MainWindow::savePreset() {
    if (!loaded_ || loading_) return;
    play(false); bool accepted = false;
    const QString initial = presetCombo_->currentIndex()>0 ? presetCombo_->currentText() : QFileInfo(project_.asset).completeBaseName();
    const QString name = QInputDialog::getText(this,tr("Save preset"),tr("Preset name:"),QLineEdit::Normal,initial,&accepted).trimmed();
    if (!accepted) { viewport_->setFocus(); return; }
    for (const auto &entry : presetStore_.list()) if (entry.name.compare(name,Qt::CaseInsensitive)==0) {
        if (QMessageBox::question(this,tr("Replace preset"),tr("A preset named '%1' already exists. Replace it?").arg(entry.name),QMessageBox::Yes | QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes) return;
        break;
    }
    project_.camera = viewport_->camera(); QString path,error;
    if (!presetStore_.save(name,project_,&path,&error,PresetScope::Editor)) { showError(error); return; }
    refreshPresets(path);
    statusBar()->showMessage(tr("Preset saved: %1").arg(name),5000); viewport_->setFocus();
}
void MainWindow::applyPresetSettings(const Project &settings) {
    play(false); viewport_->setTransformMode(TransformMode::None); viewport_->setCropEditing(false);
    project_.transform = settings.transform; project_.modifiers=settings.modifiers;project_.selectedModifier=settings.selectedModifier; project_.camera = viewport_->camera();
    project_.speed = settings.speed; project_.loop = settings.loop;
    for (int g=0; g<3; ++g) { project_.spaces[g] = settings.spaces[g]; viewport_->setCoordinateSpace(TransformMode(g+1),settings.spaces[g]); }
    viewport_->setTransform(project_.transform); viewport_->setCrop(project_.crop());
    syncUi(); dirty(); viewport_->setFocus();
}
void MainWindow::editCaptureSettings() {
    if (!loaded_ || loading_) return;
    play(false); CaptureSettingsDialog dialog(project_,presetStore_,this);
    if (dialog.exec()==QDialog::Accepted) {
        project_.captureSettings = dialog.settings();
        settings_.setValue("CaptureSettings/Last",QJsonDocument(project_.captureSettings.json()).toJson(QJsonDocument::Compact)); settings_.sync(); dirty();
    }
    refreshPresets(); viewport_->setFocus();
}
void MainWindow::loadSelectedPreset() {
    if (!loaded_ || loading_) return;
    const QString path = presetCombo_->currentData().toString(); if (path.isEmpty()) return;
    EditorPreset preset; QString error;
    if (!presetStore_.read(path,PresetScope::Editor,&preset,&error)) { showError(error); return; }
    applyPresetSettings(preset.settings);
    statusBar()->showMessage(tr("Preset loaded: %1").arg(preset.name),5000);
}
void MainWindow::openPresetFolder() {
    QString error;
    if (!presetStore_.ensureDirectory(&error)) { showError(error); return; }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(presetStore_.directory()))) showError(tr("Could not open the presets folder."));
}
void MainWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if (event->type()==QEvent::ActivationChange && isActiveWindow() && presetCombo_) refreshPresets();
}
void MainWindow::clearCrop() {
    if (!loaded_ || loading_ || !project_.modifier() || project_.modifier()->type!=ModifierType::Crop) return;
    viewport_->setCropEditing(false);project_.modifier()->enabled=false;project_.crop().enabled=false;viewport_->setCrop(project_.crop());
    syncUi(); dirty(); viewport_->setFocus(Qt::OtherFocusReason);
}
void MainWindow::applyCropPreset(float radius) {
    if (!loaded_ || loading_ || !project_.modifier() || project_.modifier()->type!=ModifierType::Crop) return;
    project_.modifier()->enabled=true;
    viewport_->setTransformMode(TransformMode::None); viewport_->setCropEditing(false);
    const auto shape = project_.crop().shape;
    project_.crop() = {}; project_.crop().shape = shape; project_.crop().enabled = true; project_.crop().radius = project_.crop().radiusZ = radius; project_.crop().height = 2.5f;
    project_.crop().width = project_.crop().depth = 2*radius;
    keyCrop(); viewport_->setCrop(project_.crop()); viewport_->setCropEditing(true); viewport_->setTransformMode(TransformMode::Move);
    syncUi(); dirty(); viewport_->setFocus(Qt::OtherFocusReason);
}
void MainWindow::syncTransformFields() {
    const bool wasSyncing = syncing_; syncing_ = true;
    const auto target = viewport_->displayedTransform();
    const QVector3D values[] = {target.position,target.rotation,target.scale};
    for (int g=0; g<3; ++g) for (int a=0; a<3; ++a) transform_[g][a]->setValue(values[g][a]);
    syncing_ = wasSyncing;
}
void MainWindow::fitCrop() {
    if (!loaded_ || loading_ || !project_.modifier() || project_.modifier()->type!=ModifierType::Crop) return;
    project_.modifier()->enabled=true;
    viewport_->setTransformMode(TransformMode::None); viewport_->setCropEditing(false);
    const auto shape = project_.crop().shape; project_.crop() = {}; project_.crop().shape = shape; project_.crop().enabled = true;
    QVector3D minimum(1e30f,1e30f,1e30f),maximum(-1e30f,-1e30f,-1e30f);
    for (int i=0; i<8; ++i) {
        const auto p = project_.transformAtFrame(std::round(project_.time*info_.fps)).matrix().map(QVector3D((i&1)?info_.maximum.x():info_.minimum.x(),(i&2)?info_.maximum.y():info_.minimum.y(),(i&4)?info_.maximum.z():info_.minimum.z()));
        for (int a=0; a<3; ++a) { minimum[a] = std::min(minimum[a],p[a]); maximum[a] = std::max(maximum[a],p[a]); }
    }
    project_.crop().transform.position = (minimum+maximum)*0.5f;
    project_.crop().transform.position.setY(minimum.y());
    const auto extent = maximum-minimum;
    // The ellipse through the corners of the capture's footprint: each semi-axis is the
    // half-extent times sqrt(2), with the same 2% margin as the other dimensions.
    project_.crop().radius = std::clamp(extent.x()*0.5f*std::sqrt(2.0f)*1.02f,0.0001f,1e6f);
    project_.crop().radiusZ = std::clamp(extent.z()*0.5f*std::sqrt(2.0f)*1.02f,0.0001f,1e6f);
    project_.crop().height = std::clamp(extent.y()*1.02f,0.0001f,1e6f);
    project_.crop().width = std::clamp(extent.x()*1.02f,0.0001f,1e6f); project_.crop().depth = std::clamp(extent.z()*1.02f,0.0001f,1e6f);
    keyCrop(); viewport_->setCrop(project_.crop()); viewport_->setCropEditing(true); viewport_->setTransformMode(TransformMode::Move);
    syncUi(); dirty(); viewport_->setFocus(Qt::OtherFocusReason);
}
void MainWindow::editCrop(bool editing) {
    if (!loaded_ || loading_) return;
    if (!project_.modifier() || project_.modifier()->type!=ModifierType::Crop) return;
    if (editing && !project_.crop().enabled && project_.modifier()->enabled) { fitCrop(); return; }
    if (!editing) viewport_->setTransformMode(TransformMode::None);
    viewport_->setCrop(project_.crop());viewport_->setCropEditing(editing);
    if (editing && viewport_->transformMode()==TransformMode::None) viewport_->setTransformMode(TransformMode::Move);
    syncUi();
    viewport_->setFocus(Qt::OtherFocusReason);
}
void MainWindow::fitCurrentTarget() {
    if (!loaded_) return;
    if (!viewport_->focusVisible()) statusBar()->showMessage(tr("Nothing visible to focus."),5000);
}
void MainWindow::syncTransformButtons() {
    transformModes_->setExclusive(false);
    for (int g=0; g<3; ++g) {
        const bool active = viewport_->transformMode()==TransformMode(g+1);
        QSignalBlocker modeBlocker(modeButtons_[g]);QSignalBlocker spaceBlocker(spaceButtons_[g]);
        // A crop is scaled in its own axes only, so that reference space is not a choice.
        modeButtons_[g]->setChecked(active); spaceButtons_[g]->setEnabled(active && loaded_ && !loading_ && !(g==2 && viewport_->cropEditing()));
    }
    transformModes_->setExclusive(true);
}
void MainWindow::updateRecentMenu() {
    recentMenu_->clear();
    const QStringList files = history_.recent();
    recentMenu_->setEnabled(!files.isEmpty());
    int index = 0;
    for (const QString &path : files) {
        auto *action = recentMenu_->addAction(QString("%1 %2").arg(++index).arg(QFileInfo(path).fileName().replace('&',"&&")));
        action->setToolTip(QDir::toNativeSeparators(path)); action->setStatusTip(QDir::toNativeSeparators(path));
        connect(action,&QAction::triggered,this,[this,path] {
            if (!QFileInfo::exists(path)) {
                QMessageBox::warning(this,tr("File not found"),tr("The file no longer exists:\n%1").arg(QDir::toNativeSeparators(path)));
                return;
            }
            openPath(path);
        });
    }
    if (files.isEmpty()) return;
    recentMenu_->addSeparator();
    recentMenu_->addAction(tr("Clear recent"),this,[this] { history_.clearRecent(); updateRecentMenu(); });
}
