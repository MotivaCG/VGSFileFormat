// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#include "sgswindow.h"
#include "colmapfinder.h"
#include "sgsversion.h"
#include "viewport.h"
#include "viewcube.h"
#include "editortheme.h"
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QElapsedTimer>
#include <QVBoxLayout>
#include <thread>

namespace {
QLabel *sectionTitle(const QString &text) {auto *l=new QLabel(text);l->setObjectName("sectionTitle");return l;}
QLabel *note(const QString &text={}) {auto *l=new QLabel(text);l->setWordWrap(true);l->setStyleSheet("color:#a6a9ac");l->setTextInteractionFlags(Qt::TextSelectableByMouse);return l;}
QDoubleSpinBox *spin(double lo,double hi,double step,int decimals,const QString &suffix={}) {
    auto *s=new QDoubleSpinBox;s->setRange(lo,hi);s->setSingleStep(step);s->setDecimals(decimals);s->setSuffix(suffix);s->setKeyboardTracking(false);return s;
}
QString thousands(qint64 n) {return QLocale().toString(n);}
// A swatch of the colour on a button.
void paintSwatch(QPushButton *button,const QColor &colour) {
    button->setStyleSheet(QStringLiteral("QPushButton{background:%1;border:1px solid #555;border-radius:4px;min-height:22px}").arg(colour.name()));
}
bool isProject(const QString &path) {return QFileInfo(path).suffix().compare("sgsproj",Qt::CaseInsensitive)==0;}
bool isPly(const QString &path) {return QFileInfo(path).suffix().compare("ply",Qt::CaseInsensitive)==0;}
// A path as the panel shows it: native, and free to wrap after any separator.
QString shown(const QString &path) {
    const QChar space(0x200B); // zero width: a place to wrap that shows nothing
    QString s=QDir::toNativeSeparators(path);
    s.replace(QChar('\\'),QString(QChar('\\'))+space);s.replace(QChar('/'),QString(QChar('/'))+space);
    return s;
}
}

SgsWindow::SgsWindow() : gs2mesh_(new Gs2Mesh(this)), turnTimer_(new QTimer(this)) {
    qRegisterMetaType<FramePtr>();qRegisterMetaType<Transform>();
    setAcceptDrops(true);resize(1440,900);
    viewport_=new Viewport;viewport_->enableLighting();viewport_->setViewerMode(true);viewport_->setSplatRendering(true);viewport_->setSplatShDegree(-1);
    viewport_->setEmptyHint(tr("Open or drop a 3D Gaussian Splatting .ply\n\nCtrl+O"));
    auto *splitter=new QSplitter;splitter->addWidget(viewport_);
    buildPanel();
    auto *scroll=new QScrollArea;scroll->setWidgetResizable(true);auto *panel=findChild<QWidget *>("sgsPanel");scroll->setWidget(panel);
    // As wide as the panel needs: the scroll area only ever scrolls down.
    scroll->setMinimumWidth(std::max(340,panel->minimumSizeHint().width()+scroll->verticalScrollBar()->sizeHint().width()+2));scroll->setFrameShape(QFrame::NoFrame);scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    splitter->addWidget(scroll);splitter->setStretchFactor(0,1);splitter->setSizes({1100,340});
    setCentralWidget(splitter);
    buildMenus();
    // Under the view cube's background button, as the VGS editor's display controls: what is
    // shown (splats or the mesh, when there is one), the mesh's look, and the lights' icons.
    {
        displayControls_=new QWidget;displayControls_->setObjectName("viewportDisplayControls");
        // As the painted Background button: the theme's button colours, no border, its height.
        displayControls_->setStyleSheet(QString("QWidget#viewportDisplayControls QPushButton { background: %1; color: white; border: none; border-radius: %3px;"
            " padding: 0; min-height: %4px; max-height: %4px; font-family: 'Segoe UI'; font-size: 9pt; }"
            " QWidget#viewportDisplayControls QPushButton:hover { background: %2; }")
            .arg(EditorTheme::button().name(),EditorTheme::buttonHover().name()).arg(EditorTheme::controlRadius).arg(ViewCube::rowHeight));
        auto *column=new QVBoxLayout(displayControls_);column->setContentsMargins(0,0,0,0);column->setSpacing(4);
        auto button=[&](const QString &name,const QString &tip) {
            auto *b=new QPushButton;b->setObjectName(name);b->setToolTip(tip);b->setCursor(Qt::PointingHandCursor);
            b->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);column->addWidget(b);return b;};
        display_=button("displayShow",tr("The splats, or the mesh gs2mesh made of them"));
        meshLook_=button("displayMeshLook",tr("The mesh with its texture, or light grey to see its shape"));
        lightIconsButton_=button("displayLightIcons",tr("The lights' icons in the view (L)"));
        connect(display_,&QPushButton::clicked,this,[this] {
            const bool mesh=!showMesh_;
            if (mesh && !mesh_ && !loadMesh()) return;
            showMesh_=mesh;viewport_->setDrawMesh(mesh);viewport_->setDrawSplats(!mesh);syncDisplay();viewport_->setFocus();});
        connect(meshLook_,&QPushButton::clicked,this,[this] {meshGray_=!meshGray_;viewport_->setMeshTextured(!meshGray_);syncDisplay();viewport_->setFocus();});
        connect(lightIconsButton_,&QPushButton::clicked,this,[this] {setLightIconsShown(!viewport_->lightIcons());viewport_->setFocus();});
        viewport_->setDisplayControls(displayControls_);setLightIconsShown(true);
    }
    connect(viewport_,&Viewport::lightClicked,this,[this](int index) {selectLight(index);});
    connect(viewport_,&Viewport::gizmoTargetEdited,this,[this](Transform t) {
        if (selected_<0 || selected_>=project_.lights.size()) return;
        SgsLight shown=shownLight(selected_);shown.setTransform(t);storeShown(selected_,shown);syncLightEditor();
    });
    // The clock of turning lights: a repaint per screen refresh while it plays.
    turnTimer_->setInterval(16);turnTimer_->setTimerType(Qt::PreciseTimer);
    // Only the clock's own label: the speed fields stay editable while it plays.
    connect(turnTimer_,&QTimer::timeout,this,[this] {lightTime_=turnFrom_+turnClock_.nsecsElapsed()/1e9;pushLights();turnTime_->setText(tr("%1 s").arg(lightTime_,0,'f',1));});
    connect(viewport_,&Viewport::escapePressed,this,[this] {selectLight(-1);});
    connect(viewport_,&Viewport::cameraChanged,this,[this] {project_.camera=viewport_->camera();project_.cameraSet=true;});
    // gs2mesh's progress and its end.
    connect(gs2mesh_,&Gs2Mesh::progress,this,[this](QString stage,double,double total) {
        meshProgress_->setValue(int(total*1000));meshStage_->setText(tr("gs2mesh: %1").arg(stage));});
    connect(gs2mesh_,&Gs2Mesh::logged,this,[this](int level,QString message) {
        static const char *tags[]={"debug","info","warning","error"};meshLog_ << QStringLiteral("[%1] %2").arg(tags[std::clamp(level,0,3)],message);
        if (level>=1) statusBar()->showMessage(message,5000);});
    connect(gs2mesh_,&Gs2Mesh::finished,this,[this](bool ok,QString error,QStringList files) {
        if (ok) {
            project_.meshFiles=files;project_.meshInputs=project_.currentMeshInputs();modified();
            if (loadMesh()) {showMesh_=true;viewport_->setDrawMesh(true);viewport_->setDrawSplats(false);}
            statusBar()->showMessage(tr("Mesh made: %1").arg(QDir::toNativeSeparators(QFileInfo(files.value(0)).absolutePath())),10000);
        } else if (error!=tr("Cancelled.") && error!="Cancelled.") QMessageBox::warning(this,tr("Generate mesh"),tr("gs2mesh could not make the mesh:\n\n%1").arg(error));
        setMeshBusy(false);emit meshFinished(ok,error);
    });
    auto key=[&](const QKeySequence &sequence,auto action) {auto *s=new QShortcut(sequence,this);connect(s,&QShortcut::activated,this,action);};
    key(Qt::Key_G,[this] {if (selected_>=0) viewport_->setTransformMode(TransformMode::Move);});
    key(Qt::Key_R,[this] {if (selected_>=0 && project_.lights[selected_].directional) viewport_->setTransformMode(TransformMode::Rotate);});
    key(Qt::Key_H,[this] {if (selected_>=0) {project_.lights[selected_].enabled=!project_.lights[selected_].enabled;syncLights();modified();}});
    key(Qt::Key_Delete,[this] {if (selected_>=0 && viewport_->hasFocus()) deleteLight();});
    // Space and Home are the Lights menu's (Play / pause turning, Back to the start): a second
    // shortcut on the same key would leave both ambiguous, and neither would fire.
    syncTitle();syncCapture();syncPhotos();syncMesh();syncLights();syncTurning();
}
SgsWindow::~SgsWindow() {gs2mesh_->cancel();}

void SgsWindow::buildPanel() {
    auto *panel=new QWidget;panel->setObjectName("sgsPanel");auto *layout=new QVBoxLayout(panel);layout->setContentsMargins(14,12,14,12);layout->setSpacing(8);
    // Capture: what is open, its normals and how much it glows when lit.
    layout->addWidget(sectionTitle(tr("CAPTURE")));
    captureInfo_=note(tr("No capture open."));layout->addWidget(captureInfo_);
    normalsInfo_=note();layout->addWidget(normalsInfo_);
    normalsButton_=new QPushButton(tr("Estimate normals"));normalsButton_->setObjectName("estimateNormals");
    normalsButton_->setToolTip(tr("Our estimate: each splat's side decided by the cameras of the rig the PLY names, or by cameras all around it. Replaces the PLY's normals."));
    layout->addWidget(normalsButton_);connect(normalsButton_,&QPushButton::clicked,this,[this] {estimateNormals();});
    auto *selfRow=new QHBoxLayout;selfRow->addWidget(new QLabel(tr("Self-illumination")));
    selfIllumination_=new QSlider(Qt::Horizontal);selfIllumination_->setObjectName("selfIllumination");selfIllumination_->setRange(0,100);selfIllumination_->setValue(10);
    selfIllumination_->setToolTip(tr("How much of the splats' colour glows on its own when lights are on: 0 lit by them only, 100% as captured plus the lights. 10% as Unreal's default."));
    selfValue_=new QLabel("10%");selfValue_->setMinimumWidth(36);selfRow->addWidget(selfIllumination_,1);selfRow->addWidget(selfValue_);layout->addLayout(selfRow);
    connect(selfIllumination_,&QSlider::valueChanged,this,[this](int v) {selfValue_->setText(QStringLiteral("%1%").arg(v));if (!syncing_) setSelfIllumination(v/100.f);});
    layout->addSpacing(8);
    // Photos: the COLMAP model the capture was trained from.
    layout->addWidget(sectionTitle(tr("PHOTOS (COLMAP)")));
    photosInfo_=note();layout->addWidget(photosInfo_);
    auto *photoRow=new QHBoxLayout;photosButton_=new QPushButton(tr("Choose folder…"));photosClear_=new QPushButton(tr("None"));
    photosClear_->setToolTip(tr("Make the mesh from the splats alone"));
    photoRow->addWidget(photosButton_);photoRow->addWidget(photosClear_);layout->addLayout(photoRow);
    connect(photosButton_,&QPushButton::clicked,this,[this] {askColmap(true);});
    connect(photosClear_,&QPushButton::clicked,this,[this] {project_.colmap.clear();project_.colmapDeclined=true;syncPhotos();syncMesh();modified();});
    layout->addSpacing(8);
    // Mesh: gs2mesh's.
    layout->addWidget(sectionTitle(tr("MESH")));
    meshInfo_=note();layout->addWidget(meshInfo_);
    meshButton_=new QPushButton(tr("Generate mesh…"));meshButton_->setObjectName("generateMesh");layout->addWidget(meshButton_);
    connect(meshButton_,&QPushButton::clicked,this,&SgsWindow::generateMesh);
    meshProgress_=new QProgressBar;meshProgress_->setRange(0,1000);meshProgress_->setTextVisible(false);meshProgress_->setMaximumHeight(8);layout->addWidget(meshProgress_);
    auto *stageRow=new QHBoxLayout;meshStage_=note();meshCancel_=new QPushButton(tr("Cancel"));stageRow->addWidget(meshStage_,1);stageRow->addWidget(meshCancel_);layout->addLayout(stageRow);
    connect(meshCancel_,&QPushButton::clicked,this,[this] {gs2mesh_->cancel();meshStage_->setText(tr("Cancelling…"));});
    layout->addSpacing(8);
    // Lights.
    layout->addWidget(sectionTitle(tr("LIGHTS")));
    layout->addWidget(note(tr("With no light on, the splats show as captured. Click a light's icon to select it; G moves it, R turns a directional one, H turns it off or on. Space plays the lights turning, Home takes them back to the start.")));
    lightList_=new QListWidget;lightList_->setObjectName("lights");lightList_->setMaximumHeight(130);layout->addWidget(lightList_);
    connect(lightList_,&QListWidget::currentRowChanged,this,[this](int row) {if (!syncing_) selectLight(row);});
    connect(lightList_,&QListWidget::itemChanged,this,[this](QListWidgetItem *item) {
        if (syncing_) return;const int row=lightList_->row(item);if (row<0 || row>=project_.lights.size()) return;
        project_.lights[row].enabled=item->checkState()==Qt::Checked;pushLights();modified();});
    auto *lightButtons=new QHBoxLayout;
    auto *addPoint=new QPushButton(tr("+ Point"));auto *addSun=new QPushButton(tr("+ Directional"));deleteLight_=new QPushButton(tr("Delete"));
    lightButtons->addWidget(addPoint);lightButtons->addWidget(addSun);lightButtons->addWidget(deleteLight_);layout->addLayout(lightButtons);
    connect(addPoint,&QPushButton::clicked,this,[this] {addLight(false);});connect(addSun,&QPushButton::clicked,this,[this] {addLight(true);});
    connect(deleteLight_,&QPushButton::clicked,this,&SgsWindow::deleteLight);
    // Turning: every light together, and the clock.
    auto *rigRow=new QHBoxLayout;rigRow->addWidget(new QLabel(tr("All turn every")));
    rigTurn_=spin(0,3600,1,1,tr(" s"));rigTurn_->setObjectName("rigTurn");rigTurn_->setSpecialValueText(tr("Still"));
    rigTurn_->setToolTip(tr("Every light turns about the scene's vertical axis, once every so many seconds, on top of its own turn"));
    rigDirection_=new QComboBox;rigDirection_->addItems({QStringLiteral("\u21BA ")+tr("Counter-clockwise"),QStringLiteral("\u21BB ")+tr("Clockwise")});
    rigRow->addWidget(rigTurn_,1);rigRow->addWidget(rigDirection_);layout->addLayout(rigRow);
    auto rigEdited=[this] {if (!syncing_) setRigTurn(float(rigTurn_->value()),rigDirection_->currentIndex()==1);};
    connect(rigTurn_,&QDoubleSpinBox::valueChanged,this,rigEdited);connect(rigDirection_,&QComboBox::currentIndexChanged,this,rigEdited);
    auto *clockRow=new QHBoxLayout;turnPlay_=new QPushButton;turnPlay_->setObjectName("turnPlay");auto *turnReset=new QPushButton(tr("Start"));
    turnPlay_->setToolTip(tr("Turn the lights (Space)"));turnReset->setToolTip(tr("Back to the lights' saved poses (Home)"));
    turnTime_=new QLabel;turnTime_->setMinimumWidth(70);
    clockRow->addWidget(turnPlay_);clockRow->addWidget(turnReset);clockRow->addWidget(turnTime_,1);layout->addLayout(clockRow);
    connect(turnPlay_,&QPushButton::clicked,this,[this] {setTurning(!turning());});
    connect(turnReset,&QPushButton::clicked,this,[this] {setTurning(false);setLightTime(0);});
    // The selected light.
    lightEditor_=new QWidget;auto *form=new QFormLayout(lightEditor_);form->setContentsMargins(0,4,0,0);
    lightName_=new QLineEdit;form->addRow(tr("Name"),lightName_);
    connect(lightName_,&QLineEdit::editingFinished,this,[this] {if (selected_<0 || syncing_) return;project_.lights[selected_].name=lightName_->text();syncLights();modified();});
    lightColour_=new QPushButton;form->addRow(tr("Colour"),lightColour_);
    connect(lightColour_,&QPushButton::clicked,this,[this] {
        if (selected_<0) return;const QColor c=QColorDialog::getColor(project_.lights[selected_].colour,this,tr("Light colour"));
        if (!c.isValid()) return;project_.lights[selected_].colour=c;syncLights();modified();});
    lightIntensity_=spin(0,1000,0.1,2);lightIntensity_->setObjectName("lightIntensity");form->addRow(tr("Intensity"),lightIntensity_);
    pointFields_=new QWidget;auto *pointForm=new QFormLayout(pointFields_);pointForm->setContentsMargins(0,0,0,0);
    lightRange_=spin(0,1000,0.5,2," m");lightRange_->setSpecialValueText(tr("No limit"));pointForm->addRow(tr("Range"),lightRange_);
    form->addRow(pointFields_);
    auto *xyz=new QHBoxLayout;lightX_=spin(-1000,1000,0.05,3);lightY_=spin(-1000,1000,0.05,3);lightZ_=spin(-1000,1000,0.05,3);
    for (auto *s:{lightX_,lightY_,lightZ_}) {s->setButtonSymbols(QAbstractSpinBox::NoButtons);s->setMinimumWidth(56);}
    xyz->addWidget(lightX_);xyz->addWidget(lightY_);xyz->addWidget(lightZ_);form->addRow(tr("Position"),xyz);
    directionFields_=new QWidget;auto *directionForm=new QFormLayout(directionFields_);directionForm->setContentsMargins(0,0,0,0);
    lightAzimuth_=spin(-180,180,5,1,QStringLiteral("°"));lightAzimuth_->setWrapping(true);lightElevation_=spin(-89.9,89.9,5,1,QStringLiteral("°"));
    lightAzimuth_->setToolTip(tr("Where the light comes from: 0° from the front (+Z), 90° from +X"));lightElevation_->setToolTip(tr("Above the horizon"));
    directionForm->addRow(tr("From azimuth"),lightAzimuth_);directionForm->addRow(tr("Elevation"),lightElevation_);form->addRow(directionFields_);
    auto *turnRow=new QHBoxLayout;lightTurn_=spin(0,3600,1,1,tr(" s"));lightTurn_->setObjectName("lightTurn");lightTurn_->setSpecialValueText(tr("Still"));
    lightTurn_->setToolTip(tr("This light turns about the scene's vertical axis, once every so many seconds"));
    lightDirection_=new QComboBox;lightDirection_->addItems({QStringLiteral("\u21BA"),QStringLiteral("\u21BB")});
    lightDirection_->setToolTip(tr("Counter-clockwise or clockwise, seen from above"));
    turnRow->addWidget(lightTurn_,1);turnRow->addWidget(lightDirection_);form->addRow(tr("Turn every"),turnRow);
    auto turnEdited=[this] {if (!syncing_ && selected_>=0) setLightTurn(selected_,float(lightTurn_->value()),lightDirection_->currentIndex()==1);};
    connect(lightTurn_,&QDoubleSpinBox::valueChanged,this,turnEdited);connect(lightDirection_,&QComboBox::currentIndexChanged,this,turnEdited);
    layout->addWidget(lightEditor_);
    auto edited=[this] {
        if (syncing_ || selected_<0) return;
        // The fields show the light as it is seen: edited so, then kept as its pose at time 0.
        SgsLight l=shownLight(selected_);
        l.intensity=float(lightIntensity_->value());l.range=float(lightRange_->value());l.position=QVector3D(float(lightX_->value()),float(lightY_->value()),float(lightZ_->value()));
        if (l.directional) l.setAngles(float(lightAzimuth_->value()),float(lightElevation_->value()));
        storeShown(selected_,l);
    };
    for (auto *s:{lightIntensity_,lightRange_,lightX_,lightY_,lightZ_,lightAzimuth_,lightElevation_}) connect(s,&QDoubleSpinBox::valueChanged,this,edited);
    layout->addStretch();
    panel->setParent(this);
}

void SgsWindow::buildMenus() {
    auto *file=menuBar()->addMenu(tr("File"));
    file->addAction(tr("Import .ply…"),QKeySequence("Ctrl+I"),this,[this] {
        if (!maybeSave()) return;
        // The last one imported, selected, else its folder.
        QSettings settings;const QString last=settings.value("Sgs/LastPly").toString();
        const QString initial=QFileInfo::exists(last) ? last : settings.value("Sgs/Directory").toString();
        const QString path=QFileDialog::getOpenFileName(this,tr("Import a 3D Gaussian Splatting PLY"),initial,tr("3D Gaussian Splatting PLY (*.ply)"));
        if (!path.isEmpty()) openPly(path);});
    file->addAction(tr("Open project…"),QKeySequence::Open,this,[this] {
        if (!maybeSave()) return;
        QSettings settings;const QString last=settings.value("Sgs/LastProject").toString();
        const QString initial=QFileInfo::exists(last) ? last : settings.value("Sgs/Directory").toString();
        const QString path=QFileDialog::getOpenFileName(this,tr("Open project"),initial,tr("SGS project (*.sgsproj)"));
        if (!path.isEmpty()) openProject(path);});
    recent_=file->addMenu(tr("Open recent"));updateRecent();
    file->addSeparator();
    file->addAction(tr("Save project"),QKeySequence::Save,this,[this] {
        if (!capture_) return;
        QString path=projectPath_;
        if (path.isEmpty()) path=QFileDialog::getSaveFileName(this,tr("Save project"),QFileInfo(project_.ply).absolutePath()+"/"+QFileInfo(project_.ply).completeBaseName()+".sgsproj",tr("SGS project (*.sgsproj)"));
        if (!path.isEmpty()) saveProject(path);});
    file->addAction(tr("Save project as…"),QKeySequence::SaveAs,this,[this] {
        if (!capture_) return;
        const QString path=QFileDialog::getSaveFileName(this,tr("Save project as"),projectPath_.isEmpty() ? QFileInfo(project_.ply).absolutePath() : projectPath_,tr("SGS project (*.sgsproj)"));
        if (!path.isEmpty()) saveProject(path);});
    file->addSeparator();
    file->addAction(tr("Export…"),QKeySequence("Ctrl+E"),this,&SgsWindow::exportDialog);
    file->addSeparator();file->addAction(tr("Exit"),QKeySequence::Quit,this,&QWidget::close);
    auto *captureMenu=menuBar()->addMenu(tr("Capture"));
    captureMenu->addAction(tr("Estimate normals"),this,[this] {estimateNormals();});
    captureMenu->addAction(tr("Photos (COLMAP)…"),this,[this] {askColmap(true);});
    captureMenu->addAction(tr("Generate mesh…"),QKeySequence("Ctrl+M"),this,&SgsWindow::generateMesh);
    captureMenu->addAction(tr("gs2mesh log"),this,&SgsWindow::showLog);
    auto *lights=menuBar()->addMenu(tr("Lights"));
    lights->addAction(tr("Add point light"),this,[this] {addLight(false);});
    lights->addAction(tr("Add directional light"),this,[this] {addLight(true);});
    lights->addAction(tr("Delete selected light"),this,&SgsWindow::deleteLight);
    lights->addSeparator();
    // Ready-made lightings: each replaces the lights there are.
    auto *presets=lights->addMenu(tr("Presets"));
    const QStringList names=SgsLight::presetNames();
    for (int i=0;i<names.size();++i) presets->addAction(names[i],this,[this,i] {applyPreset(i);});
    lights->addSeparator();
    lights->addAction(tr("Play / pause turning"),this,[this] {setTurning(!turning());})->setShortcut(QKeySequence(Qt::Key_Space));
    lights->addAction(tr("Back to the start"),this,[this] {setTurning(false);setLightTime(0);})->setShortcut(QKeySequence(Qt::Key_Home));
    lights->addSeparator();
    auto *all=lights->addMenu(tr("All lights"));
    all->addAction(tr("On"),this,[this] {for (auto &l:project_.lights) l.enabled=true;syncLights();modified();});
    all->addAction(tr("Off"),this,[this] {for (auto &l:project_.lights) l.enabled=false;syncLights();modified();});
    auto *view=menuBar()->addMenu(tr("View"));QSettings settings;
    auto toggle=[&](const QString &name,const QString &key,bool fallback,void (Viewport::*set)(bool)) {
        auto *a=view->addAction(name);a->setCheckable(true);const bool on=settings.value(key,fallback).toBool();a->setChecked(on);(viewport_->*set)(on);
        connect(a,&QAction::toggled,this,[this,key,set](bool value) {QSettings().setValue(key,value);(viewport_->*set)(value);});return a;};
    toggle(tr("Grid"),"Display/Grid",true,&Viewport::setGrid)->setShortcut(QKeySequence("Shift+G"));
    toggle(tr("Axes"),"Display/Axes",false,&Viewport::setAxes);
    toggle(tr("Origin and front marker"),"Display/FrontMarker",true,&Viewport::setFrontMarker);
    // Always on when the editor starts: hiding them is for a moment's look.
    lightIconsAction_=view->addAction(tr("Light icons"));lightIconsAction_->setCheckable(true);lightIconsAction_->setChecked(true);
    lightIconsAction_->setShortcut(QKeySequence(Qt::Key_L));
    connect(lightIconsAction_,&QAction::toggled,this,[this](bool on) {setLightIconsShown(on);});
    auto *help=menuBar()->addMenu(tr("Help"));
    help->addAction(tr("About SGS Editor"),this,[this] {
        QString version;const bool mesh=Gs2Mesh::available(nullptr,&version);
        QMessageBox::about(this,tr("About SGS Editor"),tr("<h2>SGS Editor</h2><p>Version %1</p><p>Static captures: normals, mesh, lights, .sgs export.</p><p>gs2mesh: %2</p><p>Víctor M. Feliz</p><p>The4DScanner · ScanMeNow</p>")
            .arg(SGS_EDITOR_VERSION_NAME,mesh ? version : tr("not available")));});
    help->addAction(tr("About Qt"),qApp,&QApplication::aboutQt);
}

void SgsWindow::updateRecent(const QString &path) {
    QSettings settings;QStringList recent=settings.value("Sgs/Recent").toStringList();
    if (!path.isEmpty()) {recent.removeAll(path);recent.prepend(path);recent=recent.mid(0,10);settings.setValue("Sgs/Recent",recent);settings.setValue("Sgs/Directory",QFileInfo(path).absolutePath());}
    recent_->clear();
    for (const auto &p:recent) recent_->addAction(QFileInfo(p).fileName(),this,[this,p] {if (maybeSave()) openFile(p);})->setToolTip(p);
    recent_->setEnabled(!recent.isEmpty());
}

bool SgsWindow::openFile(const QString &path) {
    if (isProject(path)) return openProject(path);
    if (isPly(path)) return openPly(path);
    QMessageBox::warning(this,tr("SGS Editor"),tr("Open a 3D Gaussian Splatting .ply or an SGS project (.sgsproj)."));
    return false;
}

bool SgsWindow::openPly(const QString &path) {
    if (gs2mesh_->running()) {QMessageBox::warning(this,tr("SGS Editor"),tr("Wait for the mesh to be made, or cancel it."));return false;}
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        SgsCapture loaded=SgsCapture::load(path);
        capture_=std::move(loaded);
    } catch (const std::exception &e) {
        QApplication::restoreOverrideCursor();
        QMessageBox::warning(this,tr("Open PLY"),QString::fromUtf8(e.what()));return false;
    }
    setTurning(false);lightTime_=turnFrom_=0;
    project_=SgsProject{};project_.ply=capture_->path;projectPath_.clear();
    // Lights cannot reach splats without normals: ours, when the PLY has none.
    QString how;
    if (capture_->normals==NormalsSource::None) {viewport_->setBusyText(tr("Estimating normals…"));capture_->estimateNormals(&how);viewport_->setBusyText({});project_.estimatedNormals=true;}
    QApplication::restoreOverrideCursor();
    selected_=-1;mesh_.reset();viewport_->setMesh(nullptr);showMesh_=false;viewport_->setDrawMesh(false);viewport_->setDrawSplats(true);viewport_->setGizmoTarget(std::nullopt);viewport_->setTransformMode(TransformMode::None);
    viewport_->setFrame(capture_->renderFrame());viewport_->fit(capture_->minimum,capture_->maximum);
    project_.camera=viewport_->camera();
    // Two coloured rim lights, off, ready to switch on.
    project_.lights=SgsLight::defaults((capture_->minimum+capture_->maximum)*0.5f,(capture_->maximum-capture_->minimum).length());
    // The COLMAP beside the PLY, when there is one.
    if (const QString found=suggestColmap(path);!found.isEmpty()) project_.colmap=found;
    modified_=false;updateRecent(capture_->path);QSettings().setValue("Sgs/LastPly",capture_->path);
    syncTitle();syncCapture();syncPhotos();syncMesh();syncLights();
    if (!how.isEmpty()) statusBar()->showMessage(how,10000);
    return true;
}

bool SgsWindow::openProject(const QString &path) {
    SgsProject loaded;
    try {
        QFile file(path);if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(file.errorString().toStdString());
        QJsonParseError error{};const auto doc=QJsonDocument::fromJson(file.readAll(),&error);
        if (error.error!=QJsonParseError::NoError || !doc.isObject()) throw std::runtime_error("The project is not valid JSON.");
        loaded=SgsProject::fromJson(doc.object(),QFileInfo(path).absoluteFilePath());
    } catch (const std::exception &e) {QMessageBox::warning(this,tr("Open project"),QString::fromUtf8(e.what()));return false;}
    if (!QFileInfo::exists(loaded.ply)) {QMessageBox::warning(this,tr("Open project"),tr("The project's PLY is missing:\n%1").arg(QDir::toNativeSeparators(loaded.ply)));return false;}
    if (!openPly(loaded.ply)) return false;
    if (loaded.estimatedNormals && capture_->normals==NormalsSource::File) {QApplication::setOverrideCursor(Qt::WaitCursor);capture_->estimateNormals();QApplication::restoreOverrideCursor();viewport_->setFrame(capture_->renderFrame());}
    loaded.estimatedNormals=loaded.estimatedNormals || project_.estimatedNormals;
    project_=loaded;projectPath_=QFileInfo(path).absoluteFilePath();setTurning(false);lightTime_=turnFrom_=0;
    // Nothing chosen for the photos yet: a model beside the PLY, as on import.
    if (project_.colmap.isEmpty() && !project_.colmapDeclined) project_.colmap=suggestColmap(project_.ply);
    if (project_.cameraSet) viewport_->setCamera(project_.camera);
    {QSignalBlocker b(selfIllumination_);selfIllumination_->setValue(int(std::lround(project_.selfIllumination*100)));selfValue_->setText(QStringLiteral("%1%").arg(selfIllumination_->value()));}
    modified_=false;updateRecent(projectPath_);QSettings().setValue("Sgs/LastProject",projectPath_);
    syncTitle();syncCapture();syncPhotos();syncMesh();syncLights();
    return true;
}

bool SgsWindow::saveProject(const QString &path) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(project_.json(QFileInfo(path).absoluteFilePath())).toJson())<0 || !file.commit()) {
        QMessageBox::warning(this,tr("Save project"),tr("Cannot save %1:\n%2").arg(QDir::toNativeSeparators(path),file.errorString()));return false;
    }
    projectPath_=QFileInfo(path).absoluteFilePath();modified_=false;updateRecent(projectPath_);QSettings().setValue("Sgs/LastProject",projectPath_);syncTitle();
    statusBar()->showMessage(tr("Saved %1").arg(QDir::toNativeSeparators(path)),5000);
    return true;
}

void SgsWindow::modified() {if (!modified_) {modified_=true;syncTitle();}}
bool SgsWindow::maybeSave() {
    if (!modified_ || !capture_) return true;
    const auto answer=QMessageBox::question(this,tr("SGS Editor"),tr("Save the changes to the project?"),QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel);
    if (answer==QMessageBox::Cancel) return false;
    if (answer==QMessageBox::Discard) return true;
    QString path=projectPath_;
    if (path.isEmpty()) path=QFileDialog::getSaveFileName(this,tr("Save project"),QFileInfo(project_.ply).absolutePath()+"/"+QFileInfo(project_.ply).completeBaseName()+".sgsproj",tr("SGS project (*.sgsproj)"));
    return !path.isEmpty() && saveProject(path);
}
void SgsWindow::closeEvent(QCloseEvent *event) {
    if (gs2mesh_->running() && QMessageBox::question(this,tr("SGS Editor"),tr("The mesh is still being made. Stop it and quit?"))!=QMessageBox::Yes) {event->ignore();return;}
    if (!maybeSave()) {event->ignore();return;}
    gs2mesh_->cancel();event->accept();
}
void SgsWindow::dragEnterEvent(QDragEnterEvent *event) {
    const auto urls=event->mimeData()->urls();
    if (!urls.isEmpty() && (isPly(urls.first().toLocalFile()) || isProject(urls.first().toLocalFile()))) event->acceptProposedAction();
}
void SgsWindow::dropEvent(QDropEvent *event) {if (!meshBusy_ && maybeSave()) openFile(event->mimeData()->urls().first().toLocalFile());}

void SgsWindow::syncTitle() {
    const QString name=!projectPath_.isEmpty() ? QFileInfo(projectPath_).fileName() : capture_ ? QFileInfo(capture_->path).fileName() : QString();
    setWindowTitle(name.isEmpty() ? tr("SGS Editor %1").arg(SGS_EDITOR_VERSION_NAME) : tr("%1%2 — SGS Editor %3").arg(name,modified_ ? "*" : "",SGS_EDITOR_VERSION_NAME));
}
void SgsWindow::syncCapture() {
    normalsButton_->setEnabled(bool(capture_));selfIllumination_->setEnabled(bool(capture_));
    if (!capture_) {captureInfo_->setText(tr("No capture open."));normalsInfo_->clear();return;}
    captureInfo_->setText(tr("%1\n%2 splats (%3 visible), SH degree %4").arg(QFileInfo(capture_->path).fileName(),thousands(qint64(capture_->frame.count)),thousands(qint64(capture_->visible))).arg(capture_->shDegree));
    QString rig;if (const auto r=capture_->rig()) rig=*r==gaussiannormals::Rig::SmnV1 ? tr(" The PLY names the SMNV1 rig.") : tr(" The PLY names the THE4DSCANNER rig.");
    normalsInfo_->setText(tr("Normals: %1.%2").arg(normalsSourceText(capture_->normals),rig));
}
void SgsWindow::syncPhotos() {
    photosButton_->setEnabled(bool(capture_));photosClear_->setEnabled(bool(capture_) && !project_.colmap.isEmpty());
    if (!capture_) {photosInfo_->clear();return;}
    if (project_.colmap.isEmpty()) {
        photosInfo_->setText(project_.colmapDeclined ? tr("None: the mesh is made from the splats alone.") : tr("Not set: asked for when the mesh is made."));return;
    }
    const auto m=findColmap(project_.colmap);
    QString text=QDir::toNativeSeparators(project_.colmap)+"\n";
    if (!m.usable()) text+=tr("⚠ %1").arg(m.problem);
    else {
        text+=tr("%1 photos, %2 cameras.").arg(m.photos).arg(m.cameras);
        const QString mismatch=colmapMatches(m,capture_->minimum,capture_->maximum);
        if (!mismatch.isEmpty()) text+=tr("\n⚠ %1").arg(mismatch);
    }
    photosInfo_->setText(text);
}
void SgsWindow::syncDisplay() {
    if (!display_) return;
    const bool hasMesh=!project_.meshFiles.isEmpty() && QFileInfo::exists(project_.meshFiles.first());
    if (!hasMesh && showMesh_) {showMesh_=false;viewport_->setDrawMesh(false);viewport_->setDrawSplats(true);} // back to the splats
    display_->setText(showMesh_ ? tr("Show: Mesh") : tr("Show: Splats"));meshLook_->setText(meshGray_ ? tr("Mesh: Gray") : tr("Mesh: Shaded"));
    display_->setVisible(hasMesh);meshLook_->setVisible(hasMesh && showMesh_);
    // The view cube's panel takes the controls' new height.
    viewport_->setDisplayControls(displayControls_);
}
void SgsWindow::syncMesh() {
    syncDisplay();
    const bool running=gs2mesh_->running();
    meshButton_->setEnabled(bool(capture_) && !running);meshCancel_->setVisible(running);meshProgress_->setVisible(running);
    if (!running) meshStage_->clear();
    QString why;
    if (!Gs2Mesh::available(&why)) {meshInfo_->setText(tr("gs2mesh is not available: %1").arg(shown(why)));meshButton_->setEnabled(false);return;}
    if (!capture_) {meshInfo_->clear();return;}
    if (project_.meshFiles.isEmpty()) meshInfo_->setText(tr("None yet. Will be: %1.").arg(project_.mesh.summary()));
    else {
        bool present=true;for (const auto &f:project_.meshFiles) present=present && QFileInfo::exists(f);
        meshInfo_->setText(tr("%1\n%2%3").arg(QDir::toNativeSeparators(QFileInfo(project_.meshFiles.first()).absolutePath()),project_.mesh.summary(),
            !present ? tr("\n⚠ Some of its files are missing.") : project_.meshOutOfDate() ? tr("\n⚠ Out of date: the PLY, the photos or the settings changed since.") : QString()));
    }
}

void SgsWindow::estimateNormals() {
    if (!capture_) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);viewport_->setBusyText(tr("Estimating normals…"));
    QString how;capture_->estimateNormals(&how);
    viewport_->setBusyText({});QApplication::restoreOverrideCursor();
    project_.estimatedNormals=true;viewport_->setFrame(capture_->renderFrame());
    syncCapture();modified();statusBar()->showMessage(how,10000);
}

void SgsWindow::setSelfIllumination(float amount) {
    project_.selfIllumination=std::clamp(amount,0.f,1.f);
    {QSignalBlocker b(selfIllumination_);selfIllumination_->setValue(int(std::lround(project_.selfIllumination*100)));selfValue_->setText(QStringLiteral("%1%").arg(selfIllumination_->value()));}
    pushLights();modified();
}

// ---- Lights
void SgsWindow::addLight(bool directional) {
    if (!capture_) return;
    const QVector3D centre=(capture_->minimum+capture_->maximum)*0.5f;const float size=(capture_->maximum-capture_->minimum).length();
    SgsLight l=directional ? SgsLight::sun(centre,size) : SgsLight::bulb(centre,size);
    int same=0;for (const auto &o:project_.lights) same+=o.directional==directional;
    if (same) l.name+=QStringLiteral(" %1").arg(same+1);
    project_.lights.append(l);modified();selectLight(int(project_.lights.size())-1);
}
void SgsWindow::applyPreset(int index) {
    if (!capture_) return;
    const auto lights=SgsLight::preset(index,(capture_->minimum+capture_->maximum)*0.5f,(capture_->maximum-capture_->minimum).length());
    if (lights.isEmpty()) return;
    project_.lights=lights;selected_=-1;modified();selectLight(-1);
    statusBar()->showMessage(tr("Lights: %1").arg(SgsLight::presetNames().value(index)),5000);
}
void SgsWindow::deleteLight() {
    if (selected_<0 || selected_>=project_.lights.size()) return;
    project_.lights.removeAt(selected_);selected_=-1;modified();selectLight(-1);
}
void SgsWindow::selectLight(int index) {
    if (meshBusy_ && index>=0) return; // the lights wait while the mesh is made
    selected_=index>=0 && index<project_.lights.size() ? index : -1;
    if (selected_<0) {viewport_->setGizmoTarget(std::nullopt);viewport_->setTransformMode(TransformMode::None);}
    else {
        setTurning(false); // editing pauses the turning: what is edited is what is seen
        viewport_->setGizmoTarget(shownLight(selected_).transform());
        viewport_->setTransformMode(project_.lights[selected_].directional ? TransformMode::Rotate : TransformMode::Move);
    }
    syncLights();
}
void SgsWindow::pushLights() {
    QVector<Viewport::ViewLight> lights;for (int i=0;i<project_.lights.size();++i) lights.append(shownLight(i).view(i==selected_));
    viewport_->setLights(lights,project_.selfIllumination);
    if (selected_>=0) viewport_->setGizmoTarget(shownLight(selected_).transform());
}
SgsLight SgsWindow::shownLight(int i) const {
    if (i<0 || i>=project_.lights.size()) return {};
    return project_.lights[i].turned(project_.lightAngle(i,lightTime_));
}
void SgsWindow::storeShown(int i,const SgsLight &shown) {
    if (i<0 || i>=project_.lights.size()) return;
    project_.lights[i]=shown.turned(-project_.lightAngle(i,lightTime_));
    pushLights();modified();
}
void SgsWindow::setLightTurn(int i,float seconds,bool clockwise) {
    if (i<0 || i>=project_.lights.size()) return;
    // Where it is seen stays put: the new turn goes on from there.
    SgsLight shown=shownLight(i);shown.turnSeconds=std::max(0.f,seconds);shown.clockwise=clockwise;
    project_.lights[i].turnSeconds=shown.turnSeconds;project_.lights[i].clockwise=clockwise;
    storeShown(i,shown);syncTurning();
}
void SgsWindow::setRigTurn(float seconds,bool clockwise) {
    QVector<SgsLight> shown;for (int i=0;i<project_.lights.size();++i) shown.append(shownLight(i));
    project_.rigTurnSeconds=std::max(0.f,seconds);project_.rigClockwise=clockwise;
    for (int i=0;i<shown.size();++i) project_.lights[i]=shown[i].turned(-project_.lightAngle(i,lightTime_));
    pushLights();modified();syncTurning();
}
void SgsWindow::setLightIconsShown(bool shown) {
    viewport_->setLightIcons(shown);
    if (lightIconsButton_) lightIconsButton_->setText(shown ? tr("Light icons: On") : tr("Light icons: Off"));
    if (lightIconsAction_ && lightIconsAction_->isChecked()!=shown) {QSignalBlocker b(lightIconsAction_);lightIconsAction_->setChecked(shown);}
}
void SgsWindow::setMeshBusy(bool busy) {
    meshBusy_=busy;
    if (busy) {setTurning(false);selectLight(-1);}
    // The panel: everything but the mesh's progress, its stage and Cancel.
    auto *panel=findChild<QWidget *>("sgsPanel");
    const QList<QWidget *> keep{meshProgress_,meshStage_,meshCancel_,meshInfo_};
    for (auto *w:panel->findChildren<QWidget *>(QString(),Qt::FindDirectChildrenOnly))
        if (!keep.contains(w) && w->objectName()!="sectionTitle") w->setEnabled(!busy);
    // The menus' actions, but View's and Help's; and the light icons' button.
    if (busyActions_.isEmpty()) {
        std::function<void(QMenu *)> collect=[&](QMenu *menu) {
            for (auto *a:menu->actions()) {if (a->menu()) collect(a->menu());else if (!a->isSeparator()) busyActions_ << a;}};
        for (auto *top:menuBar()->actions()) {
            const QString name=top->text().remove('&');
            if (top->menu() && name!=tr("View") && name!=tr("Help")) collect(top->menu());
        }
    }
    for (auto *a:busyActions_) a->setEnabled(!busy);
    // gs2mesh's log stays reachable while it works.
    for (auto *a:busyActions_) if (a->text()==tr("gs2mesh log")) a->setEnabled(true);
    syncMesh();
}
bool SgsWindow::turning() const {return turnTimer_->isActive();}
void SgsWindow::setTurning(bool play) {
    if (play==turning()) {syncTurning();return;}
    if (play) {
        if (!project_.anyTurning()) {statusBar()->showMessage(tr("No light turns: set how often in Turn every, for one light or all of them."),6000);syncTurning();return;}
        turnFrom_=lightTime_;turnClock_.start();turnTimer_->start();
    } else {
        turnTimer_->stop();lightTime_=turnFrom_+turnClock_.nsecsElapsed()/1e9;pushLights();
    }
    syncTurning();syncLightEditor();
}
void SgsWindow::setLightTime(double seconds) {
    lightTime_=std::max(0.0,seconds);turnFrom_=lightTime_;turnClock_.restart();
    pushLights();syncTurning();syncLightEditor();
}
void SgsWindow::syncTurning() {
    if (!turnPlay_) return;
    turnPlay_->setText(turning() ? tr("Pause") : tr("Play"));
    turnTime_->setText(tr("%1 s").arg(lightTime_,0,'f',1));
    const QScopedValueRollback<bool> guard(syncing_,true);
    rigTurn_->setValue(project_.rigTurnSeconds);rigDirection_->setCurrentIndex(project_.rigClockwise ? 1 : 0);
}
void SgsWindow::syncLights() {
    {
        const QScopedValueRollback<bool> guard(syncing_,true);
        lightList_->clear();
        for (const auto &l:project_.lights) {
            auto *item=new QListWidgetItem(QStringLiteral("%1  ·  %2").arg(l.name,l.directional ? tr("directional") : tr("point")));
            item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(l.enabled ? Qt::Checked : Qt::Unchecked);
            QPixmap swatch(12,12);swatch.fill(l.colour);item->setIcon(QIcon(swatch));lightList_->addItem(item);
        }
        lightList_->setCurrentRow(selected_);
    }
    deleteLight_->setEnabled(selected_>=0);lightList_->setEnabled(bool(capture_));
    pushLights();syncLightEditor();
}
void SgsWindow::syncLightEditor() {
    lightEditor_->setVisible(selected_>=0);if (selected_<0) return;
    const QScopedValueRollback<bool> guard(syncing_,true);const SgsLight l=shownLight(selected_);
    lightName_->setText(l.name);paintSwatch(lightColour_,l.colour);lightIntensity_->setValue(l.intensity);lightRange_->setValue(l.range);
    lightX_->setValue(l.position.x());lightY_->setValue(l.position.y());lightZ_->setValue(l.position.z());
    pointFields_->setVisible(!l.directional);directionFields_->setVisible(l.directional);
    float azimuth=0,elevation=0;l.angles(&azimuth,&elevation);lightAzimuth_->setValue(azimuth);lightElevation_->setValue(elevation);
    lightTurn_->setValue(l.turnSeconds);lightDirection_->setCurrentIndex(l.clockwise ? 1 : 0);
}

// ---- Mesh
QString SgsWindow::meshFolder() const {
    const QFileInfo base(projectPath_.isEmpty() ? project_.ply : projectPath_);
    return base.absolutePath()+"/"+QFileInfo(project_.ply).completeBaseName()+"_mesh";
}

bool SgsWindow::askColmap(bool force) {
    if (!capture_) return false;
    if (!force && (!project_.colmap.isEmpty() || project_.colmapDeclined)) return true;
    QDialog dialog(this);dialog.setWindowTitle(tr("Photos of the capture"));auto *layout=new QVBoxLayout(&dialog);
    layout->addWidget(note(tr("gs2mesh makes a better mesh with the photos the capture was trained from: an undistorted COLMAP model (cameras.bin, images.bin) and its images. "
                              "It takes the geometry from stereo between them, smooths what no photo sees, adds the faces' relief and the albedo from them. Without them, everything comes from the splats.")));
    auto *row=new QHBoxLayout;auto *path=new QLineEdit(project_.colmap.isEmpty() ? suggestColmap(project_.ply) : project_.colmap);path->setMinimumWidth(460);
    auto *browse=new QPushButton(tr("Browse…"));row->addWidget(path,1);row->addWidget(browse);layout->addLayout(row);
    auto *status=note();layout->addWidget(status);
    auto check=[&] {
        if (path->text().trimmed().isEmpty()) {status->setText(tr("Choose the folder of the COLMAP model, or continue without photos."));return false;}
        const auto m=findColmap(path->text().trimmed());
        if (!m.usable()) {status->setText(tr("⚠ %1").arg(m.problem));return false;}
        const QString mismatch=colmapMatches(m,capture_->minimum,capture_->maximum);
        status->setText(tr("%1 photos, %2 cameras, in %3.%4").arg(m.photos).arg(m.cameras).arg(QDir::toNativeSeparators(m.sparse),mismatch.isEmpty() ? QString() : tr("\n⚠ %1").arg(mismatch)));
        return true;
    };
    connect(path,&QLineEdit::textChanged,&dialog,check);
    connect(browse,&QPushButton::clicked,&dialog,[&] {const QString dir=QFileDialog::getExistingDirectory(&dialog,tr("COLMAP folder"),path->text().isEmpty() ? QFileInfo(project_.ply).absolutePath() : path->text());if (!dir.isEmpty()) path->setText(dir);});
    auto *buttons=new QDialogButtonBox;auto *use=buttons->addButton(tr("Use these photos"),QDialogButtonBox::AcceptRole);
    auto *without=buttons->addButton(tr("Continue without photos"),QDialogButtonBox::ActionRole);buttons->addButton(QDialogButtonBox::Cancel);layout->addWidget(buttons);
    bool declined=false;
    connect(use,&QPushButton::clicked,&dialog,[&] {if (check()) dialog.accept();});
    connect(without,&QPushButton::clicked,&dialog,[&] {declined=true;dialog.accept();});
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    check();
    if (dialog.exec()!=QDialog::Accepted) return false;
    if (declined) {project_.colmap.clear();project_.colmapDeclined=true;}
    else {project_.colmap=QDir::cleanPath(path->text().trimmed());project_.colmapDeclined=false;}
    modified();syncPhotos();syncMesh();
    return true;
}

bool SgsWindow::meshDialog() {
    QDialog dialog(this);dialog.setWindowTitle(tr("Generate mesh"));auto *layout=new QVBoxLayout(&dialog);auto *form=new QFormLayout;
    auto combo=[&](const QList<QPair<QString,QVariant>> &items,const QVariant &current) {
        auto *c=new QComboBox;for (const auto &i:items) c->addItem(i.first,i.second);
        int at=c->findData(current);if (at<0) {c->addItem(current.toString(),current);at=c->count()-1;}c->setCurrentIndex(at);return c;};
    const auto &s=project_.mesh;
    auto *triangles=combo({{"50 000",50000},{"100 000",100000},{tr("200 000 (default)"),200000},{"400 000",400000},{"800 000",800000}},s.triangles);
    QStringList lodText;for (int l:s.lods) lodText << QString::number(l);
    auto *lods=combo({{tr("40 000 and 3 000 (default)"),"40000,3000"},{"80 000, 20 000 and 3 000","80000,20000,3000"},{"20 000","20000"},{"3 000","3000"},{tr("None"),""}},lodText.join(","));
    auto *texture=combo({{"1024",1024},{"2048",2048},{tr("4096 (default)"),4096},{"8192",8192}},s.textureSize);
    auto *voxel=combo({{tr("2 mm (finer, slower)"),2.0},{tr("3 mm (default)"),3.0},{"5 mm",5.0}},double(s.voxelMm));
    auto *normalMap=new QCheckBox(tr("Normal map"));normalMap->setChecked(s.normalMap);
    form->addRow(tr("Main mesh, triangles"),triangles);form->addRow(tr("LODs, triangles"),lods);form->addRow(tr("Texture"),texture);form->addRow(tr("Volume voxel"),voxel);form->addRow(QString(),normalMap);
    layout->addLayout(form);
    // The photos, changeable here too; and the models that use them.
    auto *photos=note(project_.colmap.isEmpty() ? tr("Photos: none, from the splats alone.") : tr("Photos: %1").arg(QDir::toNativeSeparators(project_.colmap)));
    auto *change=new QPushButton(tr("Change photos…"));auto *photoRow=new QHBoxLayout;photoRow->addWidget(photos,1);photoRow->addWidget(change);layout->addLayout(photoRow);
    connect(change,&QPushButton::clicked,&dialog,[&] {if (askColmap(true)) photos->setText(project_.colmap.isEmpty() ? tr("Photos: none, from the splats alone.") : tr("Photos: %1").arg(QDir::toNativeSeparators(project_.colmap)));});
    QSettings settings;
    auto modelRow=[&](const QString &label,const QString &key,const QString &tip) {
        // What was chosen on this machine, else the model shipped beside gs2mesh.
        auto *edit=new QLineEdit(settings.value(key,Gs2Mesh::defaultModel(key=="Models/Stereo" ? Gs2Mesh::Model::Stereo : Gs2Mesh::Model::MoGe)).toString());
        edit->setPlaceholderText(tr("not used"));edit->setToolTip(tip);
        auto *pick=new QPushButton(tr("…"));pick->setFixedWidth(32);auto *row=new QHBoxLayout;row->addWidget(edit,1);row->addWidget(pick);
        connect(pick,&QPushButton::clicked,&dialog,[&dialog,edit] {const QString f=QFileDialog::getOpenFileName(&dialog,QString(),edit->text(),QStringLiteral("ONNX (*.onnx)"));if (!f.isEmpty()) edit->setText(f);});
        auto *box=new QFormLayout;box->addRow(label,row);layout->addLayout(box);return edit;};
    auto *stereo=modelRow(tr("FoundationStereo"),"Models/Stereo",tr("With photos: stereo between them for the geometry (NVIDIA TensorRT)."));
    auto *moge=modelRow(tr("MoGe-2"),"Models/MoGe",tr("With photos: the faces' relief."));
    layout->addWidget(note(tr("The models are used with photos only. The mesh goes to %1.").arg(QDir::toNativeSeparators(meshFolder()))));
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Ok)->setText(tr("Generate"));layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if (dialog.exec()!=QDialog::Accepted) return false;
    MeshSettings next=project_.mesh;
    next.triangles=triangles->currentData().toInt();next.textureSize=texture->currentData().toInt();next.voxelMm=float(voxel->currentData().toDouble());next.normalMap=normalMap->isChecked();
    next.lods.clear();for (const auto &part:lods->currentData().toString().split(',',Qt::SkipEmptyParts)) next.lods.append(part.toInt());
    if (!(next==project_.mesh)) {project_.mesh=next;modified();}
    settings.setValue("Models/Stereo",stereo->text().trimmed());settings.setValue("Models/MoGe",moge->text().trimmed());
    return true;
}

void SgsWindow::generateMesh() {
    if (!capture_ || gs2mesh_->running()) return;
    QString why;if (!Gs2Mesh::available(&why)) {QMessageBox::warning(this,tr("Generate mesh"),tr("gs2mesh is not available:\n\n%1").arg(why));return;}
    // The photos first, when the project does not know them yet; then the resolutions.
    if (!askColmap(false) || !meshDialog()) return;
    startMesh();
}

bool SgsWindow::startMesh() {
    if (!capture_ || gs2mesh_->running()) return false;
    Gs2Mesh::Job job;job.ply=capture_->path;job.outputDir=meshFolder();job.name=QFileInfo(capture_->path).completeBaseName();job.settings=project_.mesh;
    if (!project_.colmap.isEmpty()) {
        const auto m=findColmap(project_.colmap);
        if (!m.usable()) {QMessageBox::warning(this,tr("Generate mesh"),tr("The photos cannot be used:\n\n%1").arg(m.problem));return false;}
        job.sparse=m.sparse;job.images=m.images;
        QSettings settings;
        job.stereoModel=settings.value("Models/Stereo",Gs2Mesh::defaultModel(Gs2Mesh::Model::Stereo)).toString();
        job.mogeModel=settings.value("Models/MoGe",Gs2Mesh::defaultModel(Gs2Mesh::Model::MoGe)).toString();
        job.engineCache=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/tensorrt";QDir().mkpath(job.engineCache);
    }
    if (!QDir().mkpath(job.outputDir)) {QMessageBox::warning(this,tr("Generate mesh"),tr("Cannot create %1.").arg(QDir::toNativeSeparators(job.outputDir)));return false;}
    meshLog_.clear();meshProgress_->setValue(0);meshStage_->setText(tr("Starting gs2mesh…"));
    const bool started=gs2mesh_->start(job);
    if (started) setMeshBusy(true);
    syncMesh();return started;
}

bool SgsWindow::loadMesh() {
    const QString obj=project_.meshFiles.value(0);
    if (obj.isEmpty() || !QFileInfo::exists(obj)) return false;
    QApplication::setOverrideCursor(Qt::WaitCursor);viewport_->setBusyText(tr("Loading the mesh…"));
    try {
        const MeshData data=readObj(obj);
        auto mesh=std::make_shared<Viewport::ViewMesh>();
        mesh->vertices.reserve(data.vertices()*8);
        for (size_t i=0;i<data.vertices();++i) {
            for (int a=0;a<3;++a) mesh->vertices.push_back(data.positions[i*3+a]);
            for (int a=0;a<3;++a) mesh->vertices.push_back(data.normals[i*3+a]);
            mesh->vertices.push_back(data.uvs[i*2]);mesh->vertices.push_back(data.uvs[i*2+1]);
        }
        mesh->indices=data.indices;
        if (!data.albedo.isEmpty()) mesh->albedo=QImage(data.albedo);
        mesh_=mesh;viewport_->setMesh(mesh);
    } catch (const std::exception &e) {
        viewport_->setBusyText({});QApplication::restoreOverrideCursor();
        QMessageBox::warning(this,tr("Mesh"),QString::fromUtf8(e.what()));return false;
    }
    viewport_->setBusyText({});QApplication::restoreOverrideCursor();
    return true;
}

void SgsWindow::showLog() {
    QDialog dialog(this);dialog.setWindowTitle(tr("gs2mesh log"));dialog.resize(900,500);auto *layout=new QVBoxLayout(&dialog);
    auto *text=new QPlainTextEdit(meshLog_.isEmpty() ? tr("gs2mesh has not run in this session.") : meshLog_.join('\n'));text->setReadOnly(true);layout->addWidget(text);
    dialog.exec();
}

// ---- Export
SgsExportResult SgsWindow::exportSgsTo(const QString &path) {
    if (!capture_) throw std::runtime_error("No capture open.");
    SgsExportOptions options;
    if (project_.sgsIncludesMesh) {
        const QString obj=project_.sgsMeshFile();
        if (!obj.isEmpty() && QFileInfo::exists(obj)) {
            const MeshData mesh=readObj(obj);
            GlbOptions glb;glb.textureSize=project_.sgsTextureSize;
            options.glb=writeGlb(mesh,mesh.albedo.isEmpty() ? QImage() : QImage(mesh.albedo),mesh.normalMap.isEmpty() ? QImage() : QImage(mesh.normalMap),glb);
            options.provenance["mesh"]=QJsonObject{{"source",QFileInfo(obj).fileName()},{"triangles",double(mesh.triangles())},{"settings",project_.mesh.json()},
                                                    {"photos",!project_.colmap.isEmpty()}};
        }
    }
    const QImage thumbnail=viewport_->cleanImage(256);
    if (!thumbnail.isNull()) {QBuffer buffer(&options.thumbnailJpeg);buffer.open(QIODevice::WriteOnly);thumbnail.save(&buffer,"JPG",88);}
    const auto c=viewport_->camera();const auto eye=c.position();
    options.provenance["view"]=QJsonObject{{"target",QJsonArray{c.target.x(),c.target.y(),c.target.z()}},{"position",QJsonArray{eye.x(),eye.y(),eye.z()}},{"verticalFov",45}};
    options.provenance["selfIllumination"]=double(project_.selfIllumination);
    return exportSgs(*capture_,options,path);
}
void SgsWindow::exportPlyTo(const QString &path) {
    if (!capture_) throw std::runtime_error("No capture open.");
    exportSgsPly(*capture_,path);
}
bool SgsWindow::sgsOptionsDialog() {
    QDialog dialog(this);dialog.setWindowTitle(tr("Export .sgs"));auto *layout=new QVBoxLayout(&dialog);auto *form=new QFormLayout;
    auto *include=new QCheckBox(tr("Carry the mesh"));include->setChecked(project_.sgsIncludesMesh);
    auto *which=new QComboBox;auto *texture=new QComboBox;
    QStringList objs;for (const auto &f:project_.meshFiles) if (f.endsWith(".obj",Qt::CaseInsensitive)) objs << f;
    for (int i=0;i<objs.size();++i) {
        int triangles=i==0 ? project_.mesh.triangles : project_.mesh.lods.value(i-1);
        which->addItem(i==0 ? tr("Main, %1 triangles").arg(thousands(triangles)) : tr("LOD %1, %2 triangles").arg(i).arg(thousands(triangles)),i==objs.size()-1 ? -1 : i);
    }
    which->setCurrentIndex(project_.sgsMesh<0 ? which->count()-1 : std::min(project_.sgsMesh,which->count()-1));
    for (int size:{1024,2048,4096}) texture->addItem(QString::number(size),size);texture->addItem(tr("As made"),0);
    texture->setCurrentIndex(std::max(0,texture->findData(project_.sgsTextureSize)));
    const bool hasMesh=!objs.isEmpty();include->setEnabled(hasMesh);which->setEnabled(hasMesh);texture->setEnabled(hasMesh);
    if (!hasMesh) include->setText(tr("Carry the mesh (none generated yet)"));
    form->addRow(QString(),include);form->addRow(tr("Mesh"),which);form->addRow(tr("Its textures, longest side"),texture);layout->addLayout(form);
    layout->addWidget(note(tr("The .sgs holds the splats with their normals, a thumbnail of this view and, when carried, the mesh as one .glb for shadows, collision and a stand-in.")));
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Ok)->setText(tr("Export"));layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if (dialog.exec()!=QDialog::Accepted) return false;
    project_.sgsIncludesMesh=hasMesh ? include->isChecked() : project_.sgsIncludesMesh;
    if (hasMesh) {const int at=which->currentIndex();project_.sgsMesh=at==which->count()-1 ? -1 : at;project_.sgsTextureSize=texture->currentData().toInt();}
    modified();
    if (hasMesh && project_.sgsIncludesMesh && project_.meshOutOfDate() &&
        QMessageBox::question(this,tr("Export .sgs"),tr("The mesh is out of date: the PLY, the photos or the settings changed since it was made. Export it anyway?"))!=QMessageBox::Yes) return false;
    return true;
}
// One export: what is written follows the extension chosen, .sgs (with its options) or .ply.
void SgsWindow::exportDialog() {
    if (!capture_) return;
    QSettings settings;
    QString filter=settings.value("Sgs/ExportFilter").toString();
    const QString sgsFilter=tr("Static capture (*.sgs)"),plyFilter=tr("3D Gaussian Splatting PLY with normals (*.ply)");
    if (filter!=plyFilter) filter=sgsFilter;
    const QString base=settings.value("Sgs/ExportDirectory",QFileInfo(capture_->path).absolutePath()).toString()+"/"+QFileInfo(capture_->path).completeBaseName();
    QString path=QFileDialog::getSaveFileName(this,tr("Export"),filter==plyFilter ? base+"_normals.ply" : base+".sgs",sgsFilter+";;"+plyFilter,&filter);
    if (path.isEmpty()) return;
    QString suffix=QFileInfo(path).suffix().toLower();
    if (suffix!="sgs" && suffix!="ply") {suffix=filter==plyFilter ? "ply" : "sgs";path+="."+suffix;}
    settings.setValue("Sgs/ExportDirectory",QFileInfo(path).absolutePath());settings.setValue("Sgs/ExportFilter",suffix=="ply" ? plyFilter : sgsFilter);
    if (suffix=="ply") {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        try {exportPlyTo(path);QApplication::restoreOverrideCursor();statusBar()->showMessage(tr("Exported %1").arg(QDir::toNativeSeparators(path)),8000);}
        catch (const std::exception &e) {QApplication::restoreOverrideCursor();QMessageBox::warning(this,tr("Export"),QString::fromUtf8(e.what()));}
        return;
    }
    if (!sgsOptionsDialog()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        const auto result=exportSgsTo(path);QApplication::restoreOverrideCursor();
        QMessageBox::information(this,tr("Export"),tr("Exported %1 (%2 MB).\n\n%3").arg(QDir::toNativeSeparators(path)).arg(result.bytes/1e6,0,'f',1).arg(result.notes.join('\n')));
    } catch (const std::exception &e) {QApplication::restoreOverrideCursor();QMessageBox::warning(this,tr("Export"),QString::fromUtf8(e.what()));}
}
