#include "mainwindow.h"
#include "viewport.h"
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
#include <QDesktopServices>
#include <QCloseEvent>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
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
#include <QStatusBar>
#include <QStyle>
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
        viewport_->setGhost(false);
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
        const bool enabled=project_.crop().enabled;project_.crop() = crop;project_.crop().enabled=enabled;syncUi();dirty();
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
    exportAction_ = file->addAction(tr("Export capture\u2026"), QKeySequence("Ctrl+E"), this, &MainWindow::exportCapture);
    exportAction_->setToolTip(tr("Export the selected In/Out range to VGS, PGS or MINT, baking capture transforms and active modifiers (Ctrl+E). MINT omits capture metadata."));
    imageAction_ = file->addAction(tr("Export viewport image…"), QKeySequence("Ctrl+Shift+E"), this, &MainWindow::exportImage);
    file->addSeparator(); file->addAction(tr("Exit"), QKeySequence::Quit, this, &QWidget::close);
    newAction->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
    openAction->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    saveAction_->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    updateRecentMenu();
    newAction->setToolTip(tr("Create an empty project (Ctrl+N)."));
    openAction->setToolTip(tr("Open a VGS, PGS or MINT capture (Ctrl+O)."));
    openProject->setToolTip(tr("Restore a saved editor project (Ctrl+Shift+O)."));
    saveAction_->setToolTip(tr("Save this project and its crop settings (Ctrl+S)."));
    auto *center = new QWidget; auto *layout = new QVBoxLayout(center);
    layout->setContentsMargins(0,0,0,0); layout->setSpacing(0);
    viewport_ = new Viewport; layout->addWidget(viewport_, 1);
    timeline_ = new QWidget; timeline_->setObjectName("timeline");
    auto *tl = new QVBoxLayout(timeline_); tl->setContentsMargins(18,14,18,14);
    auto *head = new QHBoxLayout;
    auto *rangeIcon = new QLabel;
    rangeIcon->setPixmap(QPixmap(":/icons/range.png").scaled(24,24,Qt::KeepAspectRatio,Qt::SmoothTransformation)); head->addWidget(rangeIcon);
    auto *heading = new QLabel(tr("TIMELINE")); heading->setObjectName("sectionTitle"); head->addWidget(heading);
    head->addStretch(); timeLabel_ = new QLabel; head->addWidget(timeLabel_); tl->addLayout(head);
    slider_ = new RangeSlider; slider_->setObjectName("captureRangeSlider");
    slider_->setToolTip(tr("Drag the upper marker to set In, the lower marker to set Out, or the white playhead to seek. The selected range is exported.")); tl->addWidget(slider_);
    auto *controls = new QHBoxLayout;
    auto button = [&](QStyle::StandardPixmap icon, const QString &tip, auto fn) {
        auto *b = new QPushButton; b->setIcon(transportIcon(style(),icon)); b->setToolTip(tip); b->setFixedWidth(40);
        controls->addWidget(b); connect(b, &QPushButton::clicked, this, fn); return b;
    };
    button(QStyle::SP_MediaSkipBackward, tr("Go to the playback range start (Ctrl+Home)."), [this] { play(false); setTime(project_.in, true); });
    button(QStyle::SP_MediaSeekBackward, tr("Pause and step back one frame (Left Arrow)."), [this] { play(false); setTime(project_.time-1.0/info_.fps, true); });
    playButton_ = button(QStyle::SP_MediaPlay, tr("Play / pause (Space)"), [this] { play(!playback_.isActive()); });
    button(QStyle::SP_MediaSeekForward, tr("Pause and step forward one frame (Right Arrow)."), [this] { play(false); setTime(project_.time+1.0/info_.fps, true); });
    button(QStyle::SP_MediaSkipForward, tr("Go to the playback range end (Ctrl+End)."), [this] { play(false); setTime(project_.out, true); });
    controls->addSpacing(12); controls->addWidget(new QLabel(tr("Frame")));
    frameSpin_ = new QSpinBox; frameSpin_->setMinimumWidth(80); controls->addWidget(frameSpin_);
    controls->addWidget(new QLabel(tr("In"))); inFrame_ = new QSpinBox; controls->addWidget(inFrame_);
    controls->addWidget(new QLabel(tr("Out"))); outFrame_ = new QSpinBox; controls->addWidget(outFrame_);
    controls->addStretch(); controls->addWidget(new QLabel(tr("Speed")));
    speed_ = new QDoubleSpinBox; speed_->setRange(0.1,4); speed_->setSingleStep(0.25); speed_->setSuffix(" ×"); controls->addWidget(speed_);
    loop_ = new QCheckBox(tr("Loop")); controls->addWidget(loop_); tl->addLayout(controls);
    loop_->setToolTip(tr("Toggle looping within the playback range (L)."));
    modifierPanel_=new ModifierPanel;tl->addWidget(modifierPanel_);
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
    presetCombo_->setToolTip(tr("Choose a saved preset to restore capture and cylinder transforms, display and playback settings."));
    savedPresetRow->addWidget(presetCombo_,1);
    presetFolderButton_ = new QToolButton; presetFolderButton_->setIcon(transportIcon(style(),QStyle::SP_DirOpenIcon,Qt::white));
    presetFolderButton_->setFixedSize(34,34); presetFolderButton_->setAccessibleName(tr("Open presets folder"));
    presetFolderButton_->setToolTip(tr("Open the presets folder in the system file manager (Ctrl+Alt+P).")); savedPresetRow->addWidget(presetFolderButton_);
    presetLayout->addLayout(savedPresetRow);
    savePresetButton_ = new QPushButton(tr("Save preset…")); savePresetButton_->setObjectName("saveEditorPreset");
    savePresetButton_->setToolTip(tr("Save capture/crop transforms, view and playback settings (Ctrl+Shift+P). Metadata templates are stored as separate .presetmetadata files."));
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
    transformTarget_ = new QLabel(tr("Transform target: Capture (local)")); side->addWidget(transformTarget_);
    const QString groups[] = {tr("Position"), tr("Orientation (degrees)"), tr("Scale")};
    const QString axes[] = {"X", "Y", "Z"};
    const QString shortcuts[] = {"G", "R", "S"};
    const QString modeIcons[] = {":/icons/move.png",":/icons/rotate.png",":/icons/scale.png"};
    const QString accessibleModes[] = {tr("Move"),tr("Rotate"),tr("Scale")};
    const QString modeNames[] = {tr("Click to toggle Move. G activates Move; repeat G to switch Global/Local. Esc exits all modes."),tr("Click to toggle Rotate. R activates Rotate; repeat R to switch Global/Local. Esc exits all modes."),tr("Click to toggle Scale. S activates Scale; repeat S to switch Global/Local. Crop scaling stays anchored at its base. Esc exits all modes.")};
    transformModes_ = new QButtonGroup(this); transformModes_->setExclusive(true);
    for (int g = 0; g < 3; ++g) {
        auto *box = new QGroupBox(groups[g]); auto *row = new QHBoxLayout(box);
        box->setProperty("transformGroup",true);
        row->setSpacing(5);
        auto *modeButton = new QToolButton; modeButtons_[g] = modeButton;
        modeButton->setText(shortcuts[g]); modeButton->setCheckable(true); modeButton->setFixedSize(34,34);
        modeButton->setIcon(editorButtonIcon(modeIcons[g],true)); modeButton->setIconSize(QSize(26,26));
        modeButton->setToolButtonStyle(Qt::ToolButtonIconOnly); modeButton->setAccessibleName(accessibleModes[g]);
        modeButton->setToolTip(modeNames[g]); modeButton->setObjectName(QString("transformMode_%1").arg(g));
        transformModes_->addButton(modeButton,g); row->addWidget(modeButton);
        connect(modeButton,&QToolButton::clicked,this,[this,g] { toggleTransformMode(TransformMode(g+1)); viewport_->setFocus(); });
        auto *space = new QToolButton; spaceButtons_[g] = space; space->setText(tr("Global"));
        space->setObjectName(QString("coordinateSpace_%1").arg(g)); space->setFixedSize(34,34);
        space->setToolButtonStyle(Qt::ToolButtonIconOnly); space->setIconSize(QSize(26,26));
        space->setIcon(editorButtonIcon(":/icons/global.png",false)); row->addWidget(space);
        connect(space,&QToolButton::clicked,this,[this,g] { toggleCoordinateSpace(g); viewport_->setFocus(); });
        for (int axis = 0; axis < 3; ++axis) {
            auto *spin = new QDoubleSpinBox; transform_[g][axis] = spin; spin->setDecimals(g == 1 ? 2 : 4);
            spin->setRange(g == 2 ? 0.0001 : (g == 1 ? -36000 : -1e6), g == 2 ? 10000 : (g == 1 ? 36000 : 1e6));
            spin->setSingleStep(g == 1 ? 1 : 0.01);
            // Avoid a wide minimum size driven by the largest representable number.
            spin->setMinimumWidth(0); spin->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
            spin->setObjectName(QString("transform_%1_%2").arg(g).arg(axis));
            spin->setToolTip(groups[g] + " " + axes[axis]);
            auto *label = new QLabel(axes[axis]); label->setBuddy(spin);
            row->addWidget(label); row->addWidget(spin,1);
            connect(spin, &QDoubleSpinBox::valueChanged, this, [this, g, axis](double v) {
                if (syncing_) return;
                viewport_->setDisplayedComponent(g,axis,float(v));
            });
        }
        side->addWidget(box);
    }
    auto *reset = new QPushButton(tr("Reset transform")); side->addWidget(reset);
    resetTransformButton_ = reset;
    reset->setToolTip(tr("Reset the current target's position, rotation and scale (Alt+Home)."));
    connect(reset, &QPushButton::clicked, this, &MainWindow::resetTransform);
    side->addStretch(1);
    parametersHeading_=new QLabel(tr("PARAMETERS"));parametersHeading_->setObjectName("sectionTitle");side->addWidget(parametersHeading_);
    auto *cropBox = new QGroupBox(tr("Crop modifier")); cropProperties_=cropBox; auto *cropForm = new QFormLayout(cropBox); cropForm_ = cropForm;
    cropBox->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Maximum);
    cropShapeCombo_ = new QComboBox; cropShapeCombo_->addItem(tr("Cylinder"),int(CropShape::Cylinder)); cropShapeCombo_->addItem(tr("Box"),int(CropShape::Box));
    cropShapeCombo_->setObjectName("cropShape"); cropShapeCombo_->setToolTip(tr("Choose Cylinder or Box. Both share the same base pivot and transform; their dimensions are retained separately."));
    cropForm->addRow(tr("Shape"),cropShapeCombo_);
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
    connect(t4dsPresetButton_,&QPushButton::clicked,this,[this] { applyCropPreset(1.5f); });
    connect(smnPresetButton_,&QPushButton::clicked,this,[this] { applyCropPreset(1.0f); });
    cropRadius_ = new QDoubleSpinBox; cropHeight_ = new QDoubleSpinBox; cropWidth_ = new QDoubleSpinBox; cropDepth_ = new QDoubleSpinBox;
    for (auto *spin : {cropRadius_,cropHeight_,cropWidth_,cropDepth_}) { spin->setRange(0.0001,1e6); spin->setDecimals(4); spin->setSingleStep(0.05); spin->setSuffix(" m"); }
    cropForm->addRow(tr("Radius"),cropRadius_); cropForm->addRow(tr("Height"),cropHeight_);
    cropForm->addRow(tr("Width"),cropWidth_); cropForm->addRow(tr("Depth"),cropDepth_);
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
        if (syncing_) return; project_.crop().radius = float(value); viewport_->setCrop(project_.crop()); syncModifiers(); dirty();
    });
    connect(cropHeight_,&QDoubleSpinBox::valueChanged,this,[this](double value) {
        if (syncing_) return; project_.crop().height = float(value); viewport_->setCrop(project_.crop()); syncModifiers(); dirty();
    });
    connect(cropWidth_,&QDoubleSpinBox::valueChanged,this,[this](double value) { if (!syncing_) { project_.crop().width = float(value); viewport_->setCrop(project_.crop()); syncModifiers(); dirty(); } });
    connect(cropDepth_,&QDoubleSpinBox::valueChanged,this,[this](double value) { if (!syncing_) { project_.crop().depth = float(value); viewport_->setCrop(project_.crop()); syncModifiers(); dirty(); } });
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
    isolationNeighbour_->setToolTip(tr("Nearest-neighbour rank, excluding the point itself. Frames with too few surviving points are preserved."));isolationPercent_->setToolTip(tr("Maximum Nth-neighbour distance as a percentage of the frame median. Lower values remove more points."));
    auto isolationChanged=[this] {if (syncing_ || !project_.modifier() || project_.modifier()->type!=ModifierType::PurgeIsolated) return;project_.modifier()->isolation={isolationNeighbour_->value(),isolationPercent_->value()};syncModifiers();dirty();};
    connect(isolationNeighbour_,&QSpinBox::valueChanged,this,[isolationChanged](int) {isolationChanged();});connect(isolationPercent_,&QDoubleSpinBox::valueChanged,this,[isolationChanged](double) {isolationChanged();});
    displayControls_=new QWidget;displayControls_->setObjectName("viewportDisplayControls");
    displayControls_->setStyleSheet("QWidget#viewportDisplayControls QLabel { color: #dddddd; font-size: 9pt; }"
        "QWidget#viewportDisplayControls QDoubleSpinBox { padding: 2px; min-height: 18px; }");
    auto *displayLayout=new QVBoxLayout(displayControls_);displayLayout->setContentsMargins(6,0,6,0);displayLayout->setSpacing(4);
    auto *viewForm = new QFormLayout;displayLayout->addLayout(viewForm);
    pointSize_ = new QDoubleSpinBox;pointSize_->setObjectName("displayPointSize");pointSize_->setDecimals(1);pointSize_->setRange(1,12); pointSize_->setSingleStep(0.5); pointSize_->setSuffix(" px");pointSize_->setValue(5);
    pointSize_->setToolTip(tr("Opaque point diameter in viewport pixels. Default: 5 px."));
    pointSize_->setMinimumWidth(0);pointSize_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    viewForm->addRow(tr("Point size"), pointSize_);
    grid_ = new QCheckBox(tr("Grid and axes")); viewForm->addRow(grid_);
    grid_->setToolTip(tr("Toggle the world grid and reference axes (Shift+G)."));
    ghostButton_=new QToolButton;ghostButton_->setObjectName("ghostComparison");ghostButton_->setCheckable(true);ghostButton_->setChecked(false);ghostButton_->setIcon(editorButtonIcon(":/icons/ghost.png",true,":/icons/ghost_off.png"));ghostButton_->setIconSize({24,24});ghostButton_->setFixedSize(30,30);ghostButton_->setAccessibleName(tr("Ghost comparison"));
    ghostButton_->setStyleSheet("QToolButton:checked {background: #494949; border: 1px solid #888888; border-radius: 3px;}");
    ghostButton_->setToolTip(tr("Freeze currently visible points as a faint white ghost with a soft outline. Timeline and transform changes leave the copy fixed. Switch off to remove it."));displayLayout->addWidget(ghostButton_,0,Qt::AlignHCenter);
    connect(ghostButton_,&QToolButton::toggled,this,[this](bool checked) {const bool active=viewport_->setGhost(checked);QSignalBlocker blocker(ghostButton_);ghostButton_->setChecked(active);if (checked && !active) statusBar()->showMessage(tr("No visible points to freeze."),5000);viewport_->setFocus();});
    connect(viewport_,&Viewport::ghostChanged,this,[this](bool active) {QSignalBlocker blocker(ghostButton_);ghostButton_->setChecked(active);});
    viewport_->setDisplayControls(displayControls_);
    auto *note = new QLabel(tr("Projects save the capture reference, transform and view.")); note->setWordWrap(true); side->addWidget(note);
    auto *toolsScroll = new QScrollArea; toolsScroll->setWidgetResizable(true); toolsScroll->setFrameShape(QFrame::NoFrame);
    toolsScroll->setObjectName("toolsScrollArea");
    toolsScroll->setWidget(tools_);
    dock->setWidget(toolsScroll); dock->setMinimumWidth(450); addDockWidget(Qt::RightDockWidgetArea, dock);
    auto *viewMenu = menuBar()->addMenu(tr("View")); viewMenu->addAction(dock->toggleViewAction());
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
                          "<p>Opaque point preview · Qt / OpenGL</p>"
                          "<p>THE4DSCANNER · ScanMeNow</p>"));
        dialog.exec();
    });
    helpMenu->addAction(tr("About Qt"), qApp, &QApplication::aboutQt);
    connect(slider_, &RangeSlider::playheadChanged, this, [this](int f) { if (!syncing_) { play(false); setTime(f/info_.fps, true); } });
    connect(slider_, &RangeSlider::rangeChanged, this, [this](int first,int last,int preview) {
        if (syncing_ || !loaded_) return; play(false); project_.in=first/info_.fps; project_.out=last/info_.fps;
        setTime(preview/info_.fps,true); syncUi(); dirty();
    });
    connect(frameSpin_, &QSpinBox::valueChanged, this, [this](int f) { if (!syncing_) { play(false); setTime(f/info_.fps, true); } });
    connect(inFrame_, &QSpinBox::valueChanged, this, [this](int f) {
        if (syncing_) return; play(false); project_.in = f/info_.fps; project_.out = std::max(project_.out, project_.in);
        setTime(std::max(project_.time, project_.in), true); syncUi(); dirty();
    });
    connect(outFrame_, &QSpinBox::valueChanged, this, [this](int f) {
        if (syncing_) return; play(false); project_.out = f/info_.fps; project_.in = std::min(project_.in, project_.out);
        setTime(std::min(project_.time, project_.out), true); syncUi(); dirty();
    });
    connect(loop_, &QCheckBox::toggled, this, [this](bool value) { if (!syncing_) { project_.loop = value; settings_.setValue("Playback/Loop",value); dirty(); } });
    connect(grid_, &QCheckBox::toggled, this, [this](bool value) { if (!syncing_) { project_.grid = value; settings_.setValue("Display/Grid",value); viewport_->setGrid(value); dirty(); } });
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
    shortcut("C",[this] { editCrop(!viewport_->cropEditing()); });
    shortcut("Ctrl+F",[this] { fitCrop(); });
    shortcut("Ctrl+Shift+C",[this] { clearCrop(); });
    shortcut("Ctrl+Alt+1",[this] { applyCropPreset(1.5f); });
    shortcut("Ctrl+Alt+2",[this] { applyCropPreset(1.0f); });
    shortcut("Ctrl+Shift+P",[this] { savePreset(); });
    shortcut("Ctrl+Alt+P",[this] { openPresetFolder(); });
    shortcut("Ctrl+M",[this] { editCaptureSettings(); });
    shortcut("Shift+G",[this] {grid_->toggle();});
    shortcut("L",[this] { if (loaded_ && !loading_) loop_->toggle(); });
    for (int g=0; g<3; ++g) shortcut(QString("F%1").arg(g+6),[this,g] { toggleCoordinateSpace(g); });
    statusBar()->showMessage(tr("Open a capture to begin."));
}

void MainWindow::syncModifiers() {
    viewport_->setModifiers(project_.modifiers);modifierPanel_->setProject(project_);
    const bool purge=!CompiledModifiers(project_).isolations.isEmpty();
    QJsonObject state;if (purge) state={{"modifiers",project_.modifierJson()},{"transform",project_.json({})["transform"]},{"cropEditing",viewport_->cropEditing()}};
    if (state!=processingState_) {processingState_=state;if (purge) requestFrame();}
}
void MainWindow::revealModifierProperties() {
    QTimer::singleShot(0,this,[this] {
        const auto *m=project_.modifier();auto *scroll=findChild<QScrollArea *>("toolsScrollArea");if (!m || !scroll) return;
        QWidget *panel=m->type==ModifierType::AnimateTransform ? static_cast<QWidget *>(animationProperties_) : m->type==ModifierType::PurgeIsolated ? isolationProperties_ : m->type==ModifierType::Crop ? cropProperties_ : greenProperties_;
        scroll->ensureWidgetVisible(panel,0,12);
    });
}
void MainWindow::syncUi() {
    syncing_ = true;
    syncModifiers();
    const auto *selected=project_.modifier();const bool selectedCrop=selected && selected->type==ModifierType::Crop;
    parametersHeading_->setVisible(selected!=nullptr);
    cropProperties_->setVisible(selectedCrop);greenProperties_->setVisible(selected && selected->type==ModifierType::RemoveGreen);
    const bool animation=selected && selected->type==ModifierType::AnimateTransform,isolation=selected && selected->type==ModifierType::PurgeIsolated;
    animationProperties_->setVisible(animation);isolationProperties_->setVisible(isolation);
    if (animation) {animationProperties_->setTitle(tr("Animate transform: %1").arg(selected->name));animationProperties_->setAnimation(selected->animation,int(std::round(project_.time*info_.fps)),std::max(0,info_.frames-1));}
    if (isolation) {isolationProperties_->setTitle(tr("Purge Isolated: %1").arg(selected->name));isolationNeighbour_->setValue(selected->isolation.neighbour);isolationPercent_->setValue(selected->isolation.medianPercent);}
    viewport_->setTransform(project_.transformAtFrame(std::round(project_.time*info_.fps)));
    displayControls_->setEnabled(true);
    ghostButton_->setEnabled(loaded_ && !loading_);
    if (selected) {cropProperties_->setTitle(tr("Crop: %1").arg(selected->name));greenProperties_->setTitle(tr("Remove green: %1").arg(selected->name));greenSaturation_->setValue(selected->green.minimumSaturation*100);greenHue_->setValue(selected->green.hueTolerance);greenLinearRgb_->setChecked(selected->green.linearRgb);}
    tools_->setEnabled(true); timeline_->setEnabled(loaded_ && !loading_);
    for (auto *group : tools_->findChildren<QGroupBox *>(QString(),Qt::FindDirectChildrenOnly))
        group->setEnabled(group==presetBox_ || (loaded_ && !loading_ && !(animation && !selected->enabled && group->property("transformGroup").toBool())));
    resetTransformButton_->setEnabled(loaded_ && !loading_ && !(animation && !selected->enabled));
    captureSettingsButton_->setEnabled(loaded_ && !loading_);
    savePresetButton_->setEnabled(loaded_ && !loading_); presetCombo_->setEnabled(loaded_ && !loading_ && presetCombo_->count()>1);
    presetFolderButton_->setEnabled(loaded_ && !loading_);
    saveAction_->setEnabled(loaded_ && !loading_); saveAsAction_->setEnabled(loaded_ && !loading_); imageAction_->setEnabled(loaded_ && !loading_); exportAction_->setEnabled(loaded_ && !loading_);
    assetLabel_->setText(loaded_ ? info_.title : tr("No capture"));
    assetLabel_->setToolTip(project_.asset);
    metadata_->setText(loaded_ ? tr("%1 · %2 fps\n%3 s · %4 frames").arg(info_.format).arg(info_.fps,0,'f',2).arg(info_.duration,0,'f',3).arg(info_.frames) : QString());
    const auto target = viewport_->displayedTransform();
    const QVector3D vectors[] = {target.position,target.rotation,target.scale};
    for (int g=0; g<3; ++g) for (int a=0; a<3; ++a) transform_[g][a]->setValue(vectors[g][a]);
    const int maximum = std::max(0, info_.frames-1);
    slider_->setFrameRange(0,maximum); slider_->setRangeValues(int(std::round(project_.in*info_.fps)),int(std::round(project_.out*info_.fps)));
    frameSpin_->setRange(0,maximum); inFrame_->setRange(0,maximum); outFrame_->setRange(0,maximum);
    inFrame_->setValue(int(std::round(project_.in*info_.fps))); outFrame_->setValue(int(std::round(project_.out*info_.fps)));
    const int frame = int(std::round(project_.time*info_.fps)); slider_->setPlayheadValue(frame); frameSpin_->setValue(frame);
    modifierPanel_->setTimeline(frame,maximum);
    timeLabel_->setText(tr("%1 s / %2 s").arg(project_.time,0,'f',3).arg(info_.duration,0,'f',3));
    speed_->setValue(project_.speed); loop_->setChecked(project_.loop); grid_->setChecked(project_.grid);
    syncTransformButtons();
    transformTarget_->setText(viewport_->cropEditing() || animation ? tr("Transform target: %1").arg(selected ? selected->name : tr("Crop")) : tr("Transform target: Capture reference"));
    for (int g=0; g<3; ++g) {
        const bool local = project_.spaces[g]==CoordinateSpace::Local;
        spaceButtons_[g]->setText(local ? tr("Local") : tr("Global"));
        // This single button displays the chosen reference space; both choices are active selections.
        spaceButtons_[g]->setIcon(editorButtonIcon(local ? ":/icons/local.png" : ":/icons/global.png",false));
        spaceButtons_[g]->setAccessibleName(local ? tr("Local reference space") : tr("Global reference space"));
        const QString modeKeys[]={"G","R","S"};
        spaceButtons_[g]->setToolTip(tr("Toggle Global/Local reference space (F%1 or repeat %2 in this mode). Current: %3.").arg(g+6).arg(modeKeys[g],local ? tr("Local") : tr("Global")));
    }
    cropEditButton_->setChecked(viewport_->cropEditing());
    cropRadius_->setValue(project_.crop().radius); cropHeight_->setValue(project_.crop().height);
    cropShapeCombo_->setCurrentIndex(cropShapeCombo_->findData(int(project_.crop().shape)));
    cropWidth_->setValue(project_.crop().width); cropDepth_->setValue(project_.crop().depth);
    cropForm_->setRowVisible(cropRadius_,project_.crop().shape==CropShape::Cylinder);
    cropForm_->setRowVisible(cropWidth_,project_.crop().shape==CropShape::Box); cropForm_->setRowVisible(cropDepth_,project_.crop().shape==CropShape::Box);
    cropWidth_->setEnabled(selectedCrop); cropDepth_->setEnabled(selectedCrop);
    cropRadius_->setEnabled(selectedCrop); cropHeight_->setEnabled(selectedCrop); cropClearButton_->setEnabled(project_.crop().enabled);
    cropStatus_->setText(!project_.crop().enabled ? tr("Modifier disabled. Settings are retained; Edit adjusts this volume.") : viewport_->cropEditing()
        ? tr("Editing the selected crop. Crop clipping is paused; colour filters remain active.") : tr("Enabled crop modifiers combine by union over the full timeline."));
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
    const QString extension=project_.captureSettings.plain ? "pgs" : "vgs";
    const QString suggested=QDir(settings_.value("Export/Directory",QFileInfo(project_.asset).absolutePath()).toString()).filePath(QFileInfo(project_.asset).completeBaseName()+"_edited."+extension);
    QString selectedFilter=project_.captureSettings.plain ? tr("Plain Gaussian capture (*.pgs)") : tr("Compressed Gaussian capture (*.vgs)");
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
    const auto previous=viewport_->coordinateSpace(mode);viewport_->activateTransformShortcut(mode);
    const auto space=viewport_->coordinateSpace(mode);project_.spaces[int(mode)-1]=space;
    syncUi();if (previous!=space) dirty();viewport_->setFocus(Qt::OtherFocusReason);
}
void MainWindow::toggleCoordinateSpace(int group) {
    if (!loaded_ || loading_) return;
    if (viewport_->transformMode()!=TransformMode(group+1)) return;
    const auto space = project_.spaces[group]==CoordinateSpace::Global ? CoordinateSpace::Local : CoordinateSpace::Global;
    viewport_->setCoordinateSpace(TransformMode(group+1),space); project_.spaces[group] = space;
    syncUi(); dirty(); viewport_->setFocus(Qt::OtherFocusReason);
}
void MainWindow::resetTransform() {
    if (!loaded_ || loading_) return;
    if (project_.modifier() && project_.modifier()->type==ModifierType::AnimateTransform && !project_.modifier()->enabled) return;
    viewport_->setTransformMode(TransformMode::None);
    if (viewport_->cropEditing()) { project_.crop().transform = {}; viewport_->setCrop(project_.crop()); }
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
    project_.pointSize = settings.pointSize; project_.grid = settings.grid; project_.speed = settings.speed; project_.loop = settings.loop;
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
    project_.crop() = {}; project_.crop().shape = shape; project_.crop().enabled = true; project_.crop().radius = radius; project_.crop().height = 2.5f;
    project_.crop().width = project_.crop().depth = 2*radius;
    viewport_->setCrop(project_.crop()); viewport_->setCropEditing(true); viewport_->setTransformMode(TransformMode::Move);
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
    project_.crop().radius = std::clamp(std::sqrt(extent.x()*extent.x()+extent.z()*extent.z())*0.51f,0.0001f,1e6f);
    project_.crop().height = std::clamp(extent.y()*1.02f,0.0001f,1e6f);
    project_.crop().width = std::clamp(extent.x()*1.02f,0.0001f,1e6f); project_.crop().depth = std::clamp(extent.z()*1.02f,0.0001f,1e6f);
    viewport_->setCrop(project_.crop()); viewport_->setCropEditing(true); viewport_->setTransformMode(TransformMode::Move);
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
        modeButtons_[g]->setChecked(active); spaceButtons_[g]->setEnabled(active && loaded_ && !loading_);
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
