// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#include "sgswindow.h"
#include "sgsqueue.h"
#include "colmapfinder.h"
#include "sgsautomation.h"
#include "sgsversion.h"
#include "viewport.h"
#include "viewcube.h"
#include "editortheme.h"
#include <QApplication>
#include <QtMath>
#include <QStyle>
#include <QPainter>
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
#include <QLocale>
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
#include <QProgressDialog>
#include <QActionGroup>
#include <QToolBar>
#include <QToolButton>
#include <QPainterPath>
#include <QMenu>
#include <QButtonGroup>
#include <QGridLayout>
#include <QEventLoop>
#include <QThread>
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
// The style's symbol tinted light, grey when disabled: the viewer's transport icons.
QIcon tinted(QStyle *style,QStyle::StandardPixmap symbol) {
    QIcon icon;
    for (auto mode:{QIcon::Normal,QIcon::Disabled}) {
        QPixmap pixmap=style->standardIcon(symbol).pixmap(24,24);
        QPainter painter(&pixmap);painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(),mode==QIcon::Disabled ? QColor("#777777") : QColor("#eeeeee"));painter.end();
        icon.addPixmap(pixmap,mode);
    }
    return icon;
}
// A new light as the view draws one, with a plus: a bulb (rays all around) or a sun (eight
// rays) with the ray of the way its light goes.
QIcon lightIcon(bool directional) {
    QPixmap pixmap(48,48);pixmap.setDevicePixelRatio(2);pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);p.setRenderHint(QPainter::Antialiasing);
    const QColor ink("#eeeeee");const QPointF c(10.5,10.5);
    p.setPen(QPen(ink,1.6,Qt::SolidLine,Qt::RoundCap));
    const int rays=directional ? 8 : 4;
    for (int k=0;k<rays;++k) {
        const double a=(directional ? 0.0 : M_PI/8)+k*2*M_PI/rays;
        p.drawLine(QPointF(c.x()+6.2*std::cos(a),c.y()+6.2*std::sin(a)),QPointF(c.x()+8.6*std::cos(a),c.y()+8.6*std::sin(a)));
    }
    p.setBrush(ink);p.setPen(Qt::NoPen);p.drawEllipse(c,4,4);
    if (directional) {p.setPen(QPen(ink,1.6,Qt::SolidLine,Qt::RoundCap));p.drawLine(QPointF(14,14),QPointF(19,19));p.drawLine(QPointF(19,19),QPointF(19,15.5));p.drawLine(QPointF(19,19),QPointF(15.5,19));}
    // The plus, in the accent colour, at the bottom right (top right for the sun, whose ray is there).
    const QPointF plus=directional ? QPointF(19.5,4.5) : QPointF(19.5,19.5);
    p.setPen(QPen(EditorTheme::accent(),2.2,Qt::SolidLine,Qt::RoundCap));
    p.drawLine(QPointF(plus.x()-3.2,plus.y()),QPointF(plus.x()+3.2,plus.y()));p.drawLine(QPointF(plus.x(),plus.y()-3.2),QPointF(plus.x(),plus.y()+3.2));
    p.end();
    return QIcon(pixmap);
}
// A bin, drawn as the light icons are (the style's, tinted, is a solid block).
QIcon binIcon() {
    QIcon icon;
    for (auto mode:{QIcon::Normal,QIcon::Disabled}) {
        QPixmap pixmap(48,48);pixmap.setDevicePixelRatio(2);pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(mode==QIcon::Disabled ? QColor("#777777") : QColor("#eeeeee"),1.6,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(5,6.5),QPointF(19,6.5));                       // lid
        p.drawPolyline(QPolygonF{{9.5,6.5},{9.5,4},{14.5,4},{14.5,6.5}}); // handle
        p.drawPolyline(QPolygonF{{6.5,6.5},{7.5,20},{16.5,20},{17.5,6.5}}); // can
        for (double x:{10.0,12.0,14.0}) p.drawLine(QPointF(x,9.5),QPointF(x,17));
        p.end();icon.addPixmap(pixmap,mode);
    }
    return icon;
}
// Which way a light turns, seen from above: an arc with its arrowhead, counter-clockwise or
// mirrored for clockwise.
QIcon turnIcon(bool clockwise) {
    QPixmap pixmap(48,48);pixmap.setDevicePixelRatio(2);pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);p.setRenderHint(QPainter::Antialiasing);
    if (clockwise) {p.translate(24,0);p.scale(-1,1);}
    p.setPen(QPen(QColor("#eeeeee"),1.8,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.setBrush(Qt::NoBrush);
    // From the top right, round the top, the left and the bottom, back up the right side:
    // counter-clockwise on screen, with a gap at the top right.
    const QRectF ring(5,5,14,14);const double start=100,span=255;
    p.drawArc(ring,int(start*16),int(span*16));
    // The head: a filled triangle past the arc's end, pointing the way it goes on. At angle
    // th (y up) that way on screen (y down) is (-sin th, -cos th).
    const double end=qDegreesToRadians(start+span);
    const QPointF at(12+7*std::cos(end),12-7*std::sin(end)),way(-std::sin(end),-std::cos(end)),across(way.y(),-way.x());
    p.setPen(Qt::NoPen);p.setBrush(QColor("#eeeeee"));
    p.drawPolygon(QPolygonF{at+way*4.6,at+across*3.4-way*0.6,at-across*3.4-way*0.6});
    p.end();
    return QIcon(pixmap);
}
// The sculpting tools' icons: T4DEditor's (its Icons::tool), and Relax and the whole-mesh
// fixes drawn the same way, in the theme's ink with one touch of the accent.
QPainterPath iconSurface(double bump) {
    QPainterPath path;path.moveTo(4,25);path.cubicTo(10,25,11,25-bump,16,25-bump);path.cubicTo(21,25-bump,22,25,28,25);return path;
}
void iconArrow(QPainter &p,QPointF from,QPointF to) {
    p.drawLine(from,to);const QPointF d=to-from;const double length=std::hypot(d.x(),d.y());if (length<1e-6) return;
    const QPointF u=d/length,n(-u.y(),u.x());p.drawLine(to,to-u*4+n*3);p.drawLine(to,to-u*4-n*3);
}
void iconBrush(QPainter &p,QPointF tip) {
    QPainterPath b;b.moveTo(tip);b.lineTo(tip+QPointF(6,-8));b.lineTo(tip+QPointF(9,-5));b.closeSubpath();p.drawPath(b);
    p.drawLine(tip+QPointF(7.5,-6.5),tip+QPointF(14,-13));
}
QIcon sculptIcon(int tool) {
    QPixmap pm(32,32);pm.fill(Qt::transparent);
    QPainter p(&pm);p.setRenderHint(QPainter::Antialiasing);
    const QColor ink=EditorTheme::text(),accent=EditorTheme::accent();
    p.setPen(QPen(ink,2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.setBrush(Qt::NoBrush);
    switch (tool) {
    case SgsWindow::Navigate: p.drawEllipse(QPointF(16,16),10,5);p.setPen(QPen(accent,2));iconArrow(p,{21,9},{26,13});break;
    case SgsWindow::Move: p.drawPath(iconSurface(8));p.setPen(QPen(accent,2));iconArrow(p,{16,15},{16,4});iconArrow(p,{16,15},{26,15});break;
    case SgsWindow::Draw: p.drawPath(iconSurface(9));p.setPen(QPen(accent,2));iconArrow(p,{16,12},{16,3});break;
    case SgsWindow::Inflate:
        p.drawEllipse(QPointF(16,18),7,7);p.setPen(QPen(accent,2));
        iconArrow(p,{16,8},{16,3});iconArrow(p,{24,18},{29,18});iconArrow(p,{8,18},{3,18});break;
    case SgsWindow::Pinch: p.drawPath(iconSurface(-6));p.setPen(QPen(accent,2));iconArrow(p,{5,14},{13,14});iconArrow(p,{27,14},{19,14});break;
    case SgsWindow::Smooth: {
        QPainterPath wave;wave.moveTo(4,22);
        for (int x=4;x<=28;++x) wave.lineTo(x,22-4*std::sin(x*0.9)*(1.0-(x-4)/28.0));
        p.drawPath(wave);p.setPen(QPen(accent,2));p.drawLine(QPointF(4,10),QPointF(28,10));break;
    }
    case SgsWindow::Relax: {
        // Faces folded over, then the same surface with its points evenly spread.
        QPainterPath fold;fold.moveTo(4,25);fold.lineTo(9,25);fold.lineTo(13,17);fold.lineTo(8,20);fold.lineTo(14,25);p.drawPath(fold);
        p.drawLine(QPointF(14,25),QPointF(28,25));
        p.setPen(Qt::NoPen);p.setBrush(accent);for (int i=0;i<4;++i) p.drawEllipse(QPointF(16+i*4,25),1.7,1.7);
        p.setBrush(Qt::NoBrush);p.setPen(QPen(accent,2));iconArrow(p,{10,9},{22,9});break;
    }
    case SgsWindow::Flatten: p.drawPath(iconSurface(6));p.setPen(QPen(accent,2));p.drawLine(QPointF(5,13),QPointF(27,13));iconArrow(p,{16,5},{16,11});break;
    case SgsWindow::Erase: p.drawPath(iconSurface(8));p.setPen(QPen(accent,2));p.drawRoundedRect(QRectF(9,4,14,8),2,2);break;
    case SgsWindow::Paint: p.setBrush(QColor(220,60,60));iconBrush(p,{6,26});break;
    case SgsWindow::Unpaint:
        iconBrush(p,{6,26});p.setPen(QPen(accent,2));p.drawLine(QPointF(18,22),QPointF(28,30));p.drawLine(QPointF(28,22),QPointF(18,30));break;
    case SgsWindow::Clone: p.drawEllipse(QPointF(10,11),5,5);p.setPen(QPen(accent,2));p.drawEllipse(QPointF(22,22),5,5);iconArrow(p,{14,15},{18,19});break;
    case SgsWindow::Blur:
        for (int i=0;i<3;++i) {QColor c=ink;c.setAlphaF(1.0-i*0.3);p.setPen(QPen(c,2));p.drawEllipse(QPointF(16,16),4+i*4,4+i*4);}
        break;
    case SgsWindow::Lighten:
    case SgsWindow::Darken:
        p.setBrush(tool==SgsWindow::Lighten ? QColor(240,230,170) : QColor(50,50,55));p.drawEllipse(QPointF(16,16),7,7);
        if (tool==SgsWindow::Lighten)
            for (int i=0;i<8;++i) {const double a=i*M_PI/4;p.drawLine(QPointF(16+10*std::cos(a),16+10*std::sin(a)),QPointF(16+13*std::cos(a),16+13*std::sin(a)));}
        break;
    case 100: { // Fix spikes: a spike on the surface, brought down
        QPainterPath spike;spike.moveTo(4,25);spike.lineTo(12,25);spike.lineTo(15,7);spike.lineTo(18,25);spike.lineTo(28,25);p.drawPath(spike);
        p.setPen(QPen(accent,2));iconArrow(p,{24,6},{24,18});break;
    }
    case 101: { // Remove loose pieces: the body, a stray piece crossed out
        p.drawEllipse(QPointF(12,18),8,9);p.setPen(QPen(accent,2));
        p.drawEllipse(QPointF(25,8),3,3);p.drawLine(QPointF(20,3),QPointF(30,13));break;
    }
    }
    p.end();return QIcon(pm);
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

// A button that shows which way a light turns and switches it on a click. Not a checked
// button: the theme would light it up as if something were on.
QPushButton *SgsWindow::directionButton(const QString &name) {
    auto *b=new QPushButton;b->setObjectName(name);b->setIconSize({22,22});b->setFixedSize(34,30);
    setClockwise(b,false);return b;
}
bool SgsWindow::clockwise(const QPushButton *b) {return b->property("clockwise").toBool();}
void SgsWindow::setClockwise(QPushButton *b,bool clockwise) {
    b->setProperty("clockwise",clockwise);b->setIcon(turnIcon(clockwise));
    b->setToolTip(clockwise ? tr("Clockwise, seen from above: click for counter-clockwise") : tr("Counter-clockwise, seen from above: click for clockwise"));
}
SgsWindow::SgsWindow() : gs2mesh_(new Gs2Mesh(this)), turnTimer_(new QTimer(this)) {
    qRegisterMetaType<FramePtr>();qRegisterMetaType<Transform>();
    setAcceptDrops(true);resize(1440,900);
    automation_=new SgsAutomation(this,[this](const QString &method,const QJsonObject &arguments) {return automation(method,arguments);});
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
        meshLook_=button("displayMeshLook",tr("The mesh with its texture, or light grey to see its shape (S)"));
        meshLevelButton_=button("displayMeshLevel",tr("The main mesh or one of its LODs"));
        lightIconsButton_=button("displayLightIcons",tr("The lights' icons in the view (L)"));
        connect(meshLevelButton_,&QPushButton::clicked,this,[this] {
            const int levels=int(project_.meshObjs().size());if (levels<2) return;
            loadMesh((meshLevel_+1)%levels);syncDisplay();syncSculpt();viewport_->setFocus();});
        connect(display_,&QPushButton::clicked,this,[this] {setShowMesh(!showMesh_);viewport_->setFocus();});
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
    connect(viewport_,&Viewport::escapePressed,this,[this] {selectLight(-1);setSculptTool(Navigate);});
    connect(viewport_,&Viewport::paintStroke,this,&SgsWindow::sculptStroke);
    // Where the mouse is over the mesh: brushes paint there; elsewhere the view moves.
    viewport_->setPaintHitTest([this](const QPointF &at,QVector3D *hit) {
        if (!sculpt_) return false;QVector3D o,d;viewport_->captureRay(at,&o,&d);return sculpt_->raycast(o,d,hit);});
    connect(viewport_,&Viewport::paintWheel,this,[this](int setting,int steps) {
        // As T4DEditor: Ctrl + wheel the radius (15 % a step), Shift the strength, Alt the hardness (5 points).
        if (setting==0) setBrush(brushRadius_->value()*std::pow(1.15,steps),brushStrength_->value(),brushHardness_->value());
        else if (setting==1) setBrush(brushRadius_->value(),brushStrength_->value()+5*steps,brushHardness_->value());
        else setBrush(brushRadius_->value(),brushStrength_->value(),brushHardness_->value()+5*steps);
    });
    // The tools down the left, as T4DEditor's: the brushes behind one button that drops their
    // list down (Ctrl+F1...), the whole-mesh fixes below; only while there is something to sculpt.
    sculptToolbar_=new QToolBar(tr("Tools"),this);sculptToolbar_->setObjectName("sculptToolbar");sculptToolbar_->setOrientation(Qt::Vertical);
    sculptToolbar_->setMovable(false);sculptToolbar_->setIconSize(QSize(22,22));sculptToolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    addToolBar(Qt::LeftToolBarArea,sculptToolbar_);
    {
        auto *bar=sculptToolbar_;
        auto section=[&](const QString &title) {
            auto *label=new QLabel(title);
            label->setStyleSheet(QStringLiteral("color: %1; font-size: 8pt; font-weight: 600; letter-spacing: 1px; padding: 8px 4px 2px 4px;").arg(EditorTheme::mutedText().name()));
            bar->addWidget(label);};
        auto noFocus=[&](QAction *a) {
            // Keyboard focus stays on the view.
            if (QWidget *button=bar->widgetForAction(a)) {
                button->setFocusPolicy(Qt::NoFocus);
                if (auto *tool=qobject_cast<QToolButton *>(button)) tool->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
            }};
        auto *group=new QActionGroup(this);group->setExclusive(true);
        for (int t=Navigate;t<=Erase;++t) sculptTools_ << nullptr;
        // Navigate has no button: Esc returns to it from any tool.
        sculptTools_[Navigate]=new QAction(sculptIcon(Navigate),sculptToolName(Navigate),this);sculptTools_[Navigate]->setCheckable(true);group->addAction(sculptTools_[Navigate]);
        section(tr("BRUSHES"));
        brushMenu_=new QToolButton;brushMenu_->setObjectName("meshBrushes");
        auto *menu=new QMenu(brushMenu_);menu->setToolTipsVisible(true);
        brushMenu_->setMenu(menu);brushMenu_->setPopupMode(QToolButton::InstantPopup);brushMenu_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        brushMenu_->setIconSize(bar->iconSize());brushMenu_->setCheckable(true);brushMenu_->setFocusPolicy(Qt::NoFocus);
        brushMenu_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);brushMenu_->setToolTip(tr("Mesh brushes: click for the list"));
        for (int t=Move;t<=Erase;++t) {
            const QKeySequence key(Qt::CTRL|Qt::Key(int(Qt::Key_F1)+t-Move));
            QAction *a=menu->addAction(sculptIcon(t),sculptToolName(t));a->setObjectName(QStringLiteral("sculptTool%1").arg(t));
            a->setCheckable(true);a->setShortcut(key);a->setShortcutContext(Qt::WidgetShortcut); // shown in the list; the QShortcut below acts
            a->setToolTip(QStringLiteral("%1 (%2)<br><small>%3</small>").arg(sculptToolName(t),key.toString(QKeySequence::NativeText),sculptToolHint(t)));
            group->addAction(a);sculptTools_[t]=a;
            connect(a,&QAction::triggered,this,[this,t] {setSculptTool(t);viewport_->setFocus();});
            auto *shortcut=new QShortcut(key,this);shortcut->setAutoRepeat(false);
            connect(shortcut,&QShortcut::activated,this,[this,t] {if (canSculpt()) setSculptTool(t);});
        }
        brushMenu_->setIcon(sculptIcon(Move));brushMenu_->setText(sculptToolName(Move));
        bar->addWidget(brushMenu_);
        // The texture brushes, painting the albedo (Alt+F1...).
        textureMenu_=new QToolButton;textureMenu_->setObjectName("textureBrushes");
        auto *textures=new QMenu(textureMenu_);textures->setToolTipsVisible(true);
        textureMenu_->setMenu(textures);textureMenu_->setPopupMode(QToolButton::InstantPopup);textureMenu_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        textureMenu_->setIconSize(bar->iconSize());textureMenu_->setCheckable(true);textureMenu_->setFocusPolicy(Qt::NoFocus);
        textureMenu_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);textureMenu_->setToolTip(tr("Texture brushes: click for the list"));
        for (int t=Paint;t<=Darken;++t) {
            sculptTools_ << nullptr;
            const QKeySequence key(Qt::ALT|Qt::Key(int(Qt::Key_F1)+t-Paint));
            QAction *a=textures->addAction(sculptIcon(t),sculptToolName(t));a->setObjectName(QStringLiteral("sculptTool%1").arg(t));
            a->setCheckable(true);a->setShortcut(key);a->setShortcutContext(Qt::WidgetShortcut);
            a->setToolTip(QStringLiteral("%1 (%2)<br><small>%3</small>").arg(sculptToolName(t),key.toString(QKeySequence::NativeText),sculptToolHint(t)));
            group->addAction(a);sculptTools_[t]=a;
            connect(a,&QAction::triggered,this,[this,t] {setSculptTool(t);viewport_->setFocus();});
            auto *shortcut=new QShortcut(key,this);shortcut->setAutoRepeat(false);
            connect(shortcut,&QShortcut::activated,this,[this,t] {if (canSculpt()) setSculptTool(t);});
        }
        textureMenu_->setIcon(sculptIcon(Paint));textureMenu_->setText(sculptToolName(Paint));
        bar->addWidget(textureMenu_);
        section(tr("CLEAN UP"));
        auto *spikes=bar->addAction(sculptIcon(100),tr("Fix spikes"));spikes->setObjectName("fixSpikes");
        spikes->setToolTip(tr("Fix spikes<br><small>Finds the spikes and folded faces over the whole mesh and smooths them into what is around</small>"));
        auto *pieces=bar->addAction(sculptIcon(101),tr("Remove loose pieces"));pieces->setObjectName("removeLoosePieces");
        pieces->setToolTip(tr("Remove loose pieces<br><small>Removes the pieces not joined to the rest with fewer than 2 % of the triangles</small>"));
        connect(spikes,&QAction::triggered,this,&SgsWindow::fixSpikes);connect(pieces,&QAction::triggered,this,&SgsWindow::removeLoosePieces);
        noFocus(spikes);noFocus(pieces);
        // One column: every button as wide as the widest, icon and name on the left.
        std::vector<QToolButton *> buttons;int widest=0;
        for (QAction *a:bar->actions())
            if (auto *b=qobject_cast<QToolButton *>(bar->widgetForAction(a))) {
                b->setStyleSheet(QStringLiteral("QToolButton { text-align: left; padding-left: 6px; }"));b->ensurePolished();
                widest=std::max(widest,b->sizeHint().width());buttons.push_back(b);
            }
        for (QToolButton *b:buttons) b->setFixedWidth(widest+8);
        sculptToolbar_->hide();
    }
    // In the view: Tab shows the splats or the mesh.
    {
        auto *tab=new QShortcut(QKeySequence(Qt::Key_Tab),viewport_);tab->setContext(Qt::WidgetWithChildrenShortcut);
        connect(tab,&QShortcut::activated,this,[this] {if (!project_.meshObjs().isEmpty() && !meshBusy_) setShowMesh(!showMesh_);});
    }
    connect(viewport_,&Viewport::cameraChanged,this,[this] {project_.camera=viewport_->camera();project_.cameraSet=true;});
    // gs2mesh's progress and its end.
    // The bar, the stage and what gs2mesh said last: a long step (TensorRT building its engine
    // the first time, some twenty minutes) then reads as work, not as a hang.
    connect(gs2mesh_,&Gs2Mesh::progress,this,[this](QString stage,double stageProgress,double total) {
        meshProgress_->setValue(int(1000*(lodsJob_ ? (stage==QLatin1String("lods") ? stageProgress : 0.0) : total)));
        if (stage!=meshStageName_) {meshStageName_=stage;meshStage_->setText(tr("gs2mesh: %1").arg(stage));}});
    connect(gs2mesh_,&Gs2Mesh::logged,this,[this](int level,QString message) {
        static const char *tags[]={"debug","info","warning","error"};meshLog_ << QStringLiteral("[%1] %2").arg(tags[std::clamp(level,0,3)],message);
        if (level>=1) {statusBar()->showMessage(message,5000);meshStage_->setText(tr("gs2mesh: %1").arg(message));}});
    connect(gs2mesh_,&Gs2Mesh::finished,this,[this](bool ok,QString error,QStringList files) {
        const bool lods=lodsJob_;lodsJob_=false;
        if (ok && lods) {
            // The LODs made before go: the new ones follow the main mesh's files.
            QStringList kept;for (const auto &f:project_.meshFiles) if (!QFileInfo(f).completeBaseName().contains(QLatin1String("_lod"))) kept << f;
            for (auto it=meshCache_.begin();it!=meshCache_.end();) it=QFileInfo(it->first).completeBaseName().contains(QLatin1String("_lod")) ? meshCache_.erase(it) : std::next(it);
            // With an edited main mesh gs2mesh wrote it again too (its normal map baked anew): kept as the edit, not a LOD.
            QStringList made;for (const auto &f:files) if (QFileInfo(f).completeBaseName().contains(QLatin1String("_lod"))) made << f;
            project_.meshFiles=kept+made;project_.lodsSource=SgsProject::fileSignature(project_.meshObjs().value(0));modified();
            if (sculpt_) sculptSaved_=sculpt_->state();
            if (showMesh_) loadMesh(int(project_.meshObjs().size())-1); // the lowest, to see it
            statusBar()->showMessage(tr("LODs made: %1").arg(project_.mesh.lodsSummary()),10000);
        } else if (ok) {
            project_.meshFiles=files;project_.meshInputs=project_.currentMeshInputs();meshCache_.clear();albedoCache_.clear();meshLevel_=0;modified();
            project_.meshEdited.clear();project_.lodsSource.clear();sculpt_.reset();
            if (loadMesh(0)) {showMesh_=true;viewport_->setDrawMesh(true);viewport_->setDrawSplats(false);}
            statusBar()->showMessage(tr("Mesh made: %1").arg(QDir::toNativeSeparators(QFileInfo(files.value(0)).absolutePath())),10000);
        } else if (error!=tr("Cancelled.") && error!="Cancelled." && !quietMesh_)
            QMessageBox::warning(this,lods ? tr("Generate LODs") : tr("Generate mesh"),tr("gs2mesh could not make the %1:\n\n%2").arg(lods ? tr("LODs") : tr("mesh"),error));
        meshError_=ok ? QString() : error;quietMesh_=false;
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
    if (QSettings().value("Mcp/Enabled",false).toBool()) setMcpEnabled(true);
}
SgsWindow::~SgsWindow() {gs2mesh_->cancel();}

void SgsWindow::setMcpEnabled(bool enabled) {
    QString error;
    if (enabled && !automation_->start(&error)) {
        statusBar()->showMessage(tr("Cannot enable MCP: %1").arg(error),8000);enabled=false;
    } else if (!enabled) automation_->stop();
    else statusBar()->showMessage(tr("MCP enabled."),3000);
    if (mcpAction_) {const QSignalBlocker block(mcpAction_);mcpAction_->setChecked(enabled);}
    QSettings().setValue("Mcp/Enabled",enabled);
}

void SgsWindow::buildPanel() {
    auto *panel=new QWidget;panel->setObjectName("sgsPanel");auto *layout=new QVBoxLayout(panel);layout->setContentsMargins(14,12,14,12);layout->setSpacing(8);
    // Capture: what is open, its normals and how much it glows when lit.
    layout->addWidget(sectionTitle(tr("CAPTURE")));
    captureInfo_=note(tr("No capture open."));layout->addWidget(captureInfo_);
    normalsInfo_=note();layout->addWidget(normalsInfo_);
    normalsButton_=new QPushButton(tr("Estimate normals"));normalsButton_->setObjectName("estimateNormals");
    normalsButton_->setToolTip(tr("Our estimate: each splat's side decided by the cameras of the rig the PLY names, or by cameras all around it, seeing finer than on import (some seconds). Replaces the PLY's normals."));
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
    lodsButton_=new QPushButton(tr("Generate LODs…"));lodsButton_->setObjectName("generateLods");layout->addWidget(lodsButton_);
    lodsButton_->setToolTip(tr("Lighter versions of the mesh, made from it once it looks right: they share its texture, each with a normal map of its own"));
    connect(lodsButton_,&QPushButton::clicked,this,&SgsWindow::generateLods);
    meshProgress_=new QProgressBar;meshProgress_->setRange(0,1000);meshProgress_->setTextVisible(false);meshProgress_->setMaximumHeight(8);layout->addWidget(meshProgress_);
    auto *stageRow=new QHBoxLayout;meshStage_=note();meshCancel_=new QPushButton(tr("Cancel"));stageRow->addWidget(meshStage_,1);stageRow->addWidget(meshCancel_);layout->addLayout(stageRow);
    connect(meshCancel_,&QPushButton::clicked,this,[this] {gs2mesh_->cancel();meshStage_->setText(tr("Cancelling…"));});
    layout->addSpacing(8);
    // Sculpt: the main mesh by hand, in mesh view on the main mesh.
    sculptSection_=new QWidget;sculptSection_->setObjectName("sculptSection");
    {
        auto *s=new QVBoxLayout(sculptSection_);s->setContentsMargins(0,0,0,8);s->setSpacing(8);
        s->addWidget(sectionTitle(tr("SCULPT")));
        sculptInfo_=note();s->addWidget(sculptInfo_);
        // The brush, as T4DEditor's.
        auto *form=new QFormLayout;form->setContentsMargins(0,0,0,0);
        QSettings settings;
        brushRadius_=spin(0.1,100,0.5,1,tr(" cm"));brushRadius_->setObjectName("brushRadius");brushRadius_->setValue(settings.value("Sculpt/RadiusCm",3.0).toDouble());
        brushRadius_->setToolTip(tr("Brush radius on the surface (Ctrl+wheel in the view)."));
        brushStrength_=spin(1,100,5,0,tr(" %"));brushStrength_->setObjectName("brushStrength");brushStrength_->setValue(settings.value("Sculpt/Strength",50).toDouble());
        brushStrength_->setToolTip(tr("Strength per dab (Shift+wheel in the view)."));
        brushHardness_=spin(0,95,5,0,tr(" %"));brushHardness_->setObjectName("brushHardness");brushHardness_->setValue(settings.value("Sculpt/Hardness",30).toDouble());
        brushHardness_->setToolTip(tr("Share of the radius painted at full strength (Alt+wheel in the view)."));
        brushReach_=new QComboBox;brushReach_->addItems({tr("Along the surface"),tr("Sphere")});brushReach_->setObjectName("brushReach");
        brushReach_->setCurrentIndex(settings.value("Sculpt/Sphere",false).toBool() ? 1 : 0);
        brushReach_->setToolTip(tr("Along the surface keeps the brush on one part of the body even when another passes close."));
        paintColourButton_=new QPushButton;paintColourButton_->setObjectName("paintColour");paintColourButton_->setToolTip(tr("Paint color"));
        paintColour_=settings.value("Sculpt/PaintColour",QColor(220,60,60)).value<QColor>();
        connect(paintColourButton_,&QPushButton::clicked,this,[this] {
            const QColor c=QColorDialog::getColor(paintColour_,this,tr("Paint color"));if (!c.isValid()) return;
            paintColour_=c;QSettings().setValue("Sculpt/PaintColour",c);syncPaintColour();viewport_->setFocus();});
        form->addRow(tr("Radius"),brushRadius_);form->addRow(tr("Strength"),brushStrength_);form->addRow(tr("Hardness"),brushHardness_);form->addRow(tr("Reach"),brushReach_);
        form->addRow(tr("Color"),paintColourButton_);
        for (auto *s:{brushRadius_,brushStrength_,brushHardness_}) connect(s,&QDoubleSpinBox::valueChanged,this,[this] {if (!syncing_) syncBrush();});
        connect(brushReach_,&QComboBox::currentIndexChanged,this,[this] {syncBrush();});
        s->addLayout(form);syncPaintColour();
        auto *history=new QHBoxLayout;
        sculptUndo_=new QPushButton(tr("Undo"));sculptRedo_=new QPushButton(tr("Redo"));sculptRevert_=new QPushButton(tr("Revert to generated"));sculptRevert_->setObjectName("revertSculpt");
        sculptUndo_->setToolTip(tr("Ctrl+Z"));sculptRedo_->setToolTip(tr("Ctrl+Y"));sculptRevert_->setToolTip(tr("Back to the mesh as gs2mesh made it: the edits are lost"));
        history->addWidget(sculptUndo_);history->addWidget(sculptRedo_);history->addStretch();history->addWidget(sculptRevert_);s->addLayout(history);
        connect(sculptUndo_,&QPushButton::clicked,this,&SgsWindow::undoSculpt);connect(sculptRedo_,&QPushButton::clicked,this,&SgsWindow::redoSculpt);
        connect(sculptRevert_,&QPushButton::clicked,this,[this] {revertSculpt(true);});
    }
    layout->addWidget(sculptSection_);sculptSection_->hide();
    // Lights.
    layout->addWidget(sectionTitle(tr("LIGHTS")));
    layout->addWidget(note(tr("With no light on, the splats show as captured.")));
    // Add a point light, add a directional one, delete the selected one: icons, as the view draws them.
    auto *lightTools=new QHBoxLayout;lightTools->setSpacing(4);
    auto tool=[&](const QIcon &icon,const QString &tip,const QString &name) {
        auto *b=new QPushButton;b->setObjectName(name);b->setIcon(icon);b->setIconSize({22,22});b->setFixedSize(34,30);b->setToolTip(tip);lightTools->addWidget(b);return b;};
    auto *addPoint=tool(lightIcon(false),tr("Add a point light"),"addPointLight");
    auto *addSun=tool(lightIcon(true),tr("Add a directional light"),"addDirectionalLight");
    lightTools->addStretch();
    deleteLight_=tool(binIcon(),tr("Delete the selected light (Delete)"),"deleteLight");
    layout->addLayout(lightTools);
    lightList_=new QListWidget;lightList_->setObjectName("lights");lightList_->setMaximumHeight(130);
    lightList_->setToolTip(tr("Tick to turn a light on or off (H). Select one here or click its icon in the view; G moves it, R turns a directional one."));
    layout->addWidget(lightList_);
    connect(lightList_,&QListWidget::currentRowChanged,this,[this](int row) {if (!syncing_) selectLight(row);});
    connect(lightList_,&QListWidget::itemChanged,this,[this](QListWidgetItem *item) {
        if (syncing_) return;const int row=lightList_->row(item);if (row<0 || row>=project_.lights.size()) return;
        project_.lights[row].enabled=item->checkState()==Qt::Checked;pushLights();modified();});
    connect(addPoint,&QPushButton::clicked,this,[this] {addLight(false);});connect(addSun,&QPushButton::clicked,this,[this] {addLight(true);});
    connect(deleteLight_,&QPushButton::clicked,this,&SgsWindow::deleteLight);
    // Turning: the clock (play / pause, back to the start) and every light together.
    auto *clockRow=new QHBoxLayout;clockRow->setSpacing(4);
    turnPlay_=new QPushButton;turnPlay_->setObjectName("turnPlay");turnPlay_->setIconSize({22,22});turnPlay_->setFixedSize(34,30);
    auto *turnReset=new QPushButton;turnReset->setObjectName("turnStart");turnReset->setIcon(tinted(style(),QStyle::SP_MediaStop));turnReset->setIconSize({22,22});turnReset->setFixedSize(34,30);
    turnPlay_->setToolTip(tr("Turn the lights, or pause them (Space)"));turnReset->setToolTip(tr("Stop, back to the lights' saved poses (Home)"));
    turnTime_=new QLabel;turnTime_->setMinimumWidth(56);
    clockRow->addWidget(turnPlay_);clockRow->addWidget(turnReset);clockRow->addSpacing(6);clockRow->addWidget(turnTime_);clockRow->addStretch();
    clockRow->addWidget(new QLabel(tr("All every")));
    rigTurn_=spin(0,3600,1,1,tr(" s"));rigTurn_->setObjectName("rigTurn");rigTurn_->setSpecialValueText(tr("Still"));rigTurn_->setFixedWidth(84);
    rigTurn_->setToolTip(tr("Every light turns about the scene's vertical axis, once every so many seconds, on top of its own turn"));
    rigDirection_=directionButton("rigDirection");
    clockRow->addWidget(rigTurn_);clockRow->addWidget(rigDirection_);layout->addLayout(clockRow);
    auto rigEdited=[this] {if (!syncing_) setRigTurn(float(rigTurn_->value()),clockwise(rigDirection_));};
    connect(rigTurn_,&QDoubleSpinBox::valueChanged,this,rigEdited);connect(rigDirection_,&QPushButton::clicked,this,[this,rigEdited] {setClockwise(rigDirection_,!clockwise(rigDirection_));rigEdited();});
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
    lightDirection_=directionButton("lightDirection");
    turnRow->addWidget(lightTurn_,1);turnRow->addWidget(lightDirection_);form->addRow(tr("Turn every"),turnRow);
    auto turnEdited=[this] {if (!syncing_ && selected_>=0) setLightTurn(selected_,float(lightTurn_->value()),clockwise(lightDirection_));};
    connect(lightTurn_,&QDoubleSpinBox::valueChanged,this,turnEdited);connect(lightDirection_,&QPushButton::clicked,this,[this,turnEdited] {setClockwise(lightDirection_,!clockwise(lightDirection_));turnEdited();});
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
    file->addAction(tr("Import .ply…"),QKeySequence("Ctrl+I"),this,[this] {
        if (!maybeSave()) return;
        // The last one imported, selected, else its folder.
        QSettings settings;const QString last=settings.value("Sgs/LastPly").toString();
        const QString initial=QFileInfo::exists(last) ? last : settings.value("Sgs/Directory").toString();
        const QString path=QFileDialog::getOpenFileName(this,tr("Import a 3D Gaussian Splatting PLY"),initial,tr("3D Gaussian Splatting PLY (*.ply)"));
        if (!path.isEmpty()) openPly(path);});
    file->addAction(tr("Export…"),QKeySequence("Ctrl+E"),this,&SgsWindow::exportDialog);
    file->addSeparator();file->addAction(tr("Exit"),QKeySequence::Quit,this,&QWidget::close);
    auto *editMenu=menuBar()->addMenu(tr("Edit"));
    undoAction_=editMenu->addAction(tr("Undo sculpting"),QKeySequence::Undo,this,&SgsWindow::undoSculpt);
    redoAction_=editMenu->addAction(tr("Redo sculpting"),QKeySequence("Ctrl+Y"),this,&SgsWindow::redoSculpt);
    editMenu->addSeparator();
    editMenu->addAction(tr("Fix spikes"),this,&SgsWindow::fixSpikes);
    editMenu->addAction(tr("Remove loose pieces"),this,&SgsWindow::removeLoosePieces);
    editMenu->addAction(tr("Revert mesh to generated"),this,[this] {revertSculpt(true);});
    editMenu->addSeparator();
    mcpAction_=editMenu->addAction(tr("Enable MCP"));mcpAction_->setCheckable(true);mcpAction_->setChecked(automation_->enabled());
    mcpAction_->setToolTip(tr("Lets an AI assistant on this computer drive the editor through its MCP adapter (mcp/README.md)."));
    connect(mcpAction_,&QAction::toggled,this,[this](bool on) {setMcpEnabled(on);});
    auto *captureMenu=menuBar()->addMenu(tr("Capture"));
    captureMenu->addAction(tr("Estimate normals"),this,[this] {estimateNormals();});
    captureMenu->addAction(tr("Photos (COLMAP)…"),this,[this] {askColmap(true);});
    captureMenu->addAction(tr("Generate mesh…"),QKeySequence("Ctrl+M"),this,&SgsWindow::generateMesh);
    captureMenu->addAction(tr("Generate LODs…"),this,&SgsWindow::generateLods);
    captureMenu->addSeparator();
    captureMenu->addAction(tr("Mesh queue…"),this,&SgsWindow::showMeshQueue);
    captureMenu->addSeparator();
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
    // The mesh shaded or grey, as its button beside the view cube (only while it is shown).
    view->addAction(tr("Mesh: shaded / gray"),QKeySequence(Qt::Key_S),this,[this] {if (showMesh_ && meshLook_) meshLook_->click();});
    view->addAction(tr("Splats / mesh (Tab in the view)"),this,[this] {if (!project_.meshObjs().isEmpty() && !meshBusy_) setShowMesh(!showMesh_);});
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
    inspectingFile_=false;
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
    selected_=-1;mesh_.reset();meshCache_.clear();albedoCache_.clear();meshLevel_=0;sculpt_.reset();setSculptTool(Navigate);viewport_->setMesh(nullptr);showMesh_=false;viewport_->setDrawMesh(false);viewport_->setDrawSplats(true);viewport_->setGizmoTarget(std::nullopt);viewport_->setTransformMode(TransformMode::None);
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
    inspectingFile_=false;
    SgsProject loaded;
    try {
        QFile file(path);if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(file.errorString().toStdString());
        QJsonParseError error{};const auto doc=QJsonDocument::fromJson(file.readAll(),&error);
        if (error.error!=QJsonParseError::NoError || !doc.isObject()) throw std::runtime_error("The project is not valid JSON.");
        loaded=SgsProject::fromJson(doc.object(),QFileInfo(path).absoluteFilePath());
    } catch (const std::exception &e) {QMessageBox::warning(this,tr("Open project"),QString::fromUtf8(e.what()));return false;}
    if (!QFileInfo::exists(loaded.ply)) {QMessageBox::warning(this,tr("Open project"),tr("The project's PLY is missing:\n%1").arg(QDir::toNativeSeparators(loaded.ply)));return false;}
    if (!openPly(loaded.ply)) return false;
    if (loaded.fineNormals) {fineNormals(nullptr);viewport_->setFrame(capture_->renderFrame());}
    else if (loaded.estimatedNormals && capture_->normals==NormalsSource::File) {QApplication::setOverrideCursor(Qt::WaitCursor);capture_->estimateNormals();QApplication::restoreOverrideCursor();viewport_->setFrame(capture_->renderFrame());}
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
    if (sculptDirty() && !saveSculpt()) return false;
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
    if (queue_ && queue_->running() && QMessageBox::question(this,tr("SGS Editor"),tr("The mesh queue is still making meshes. Stop it and quit?"))!=QMessageBox::Yes) {event->ignore();return;}
    if (gs2mesh_->running() && QMessageBox::question(this,tr("SGS Editor"),tr("The mesh is still being made. Stop it and quit?"))!=QMessageBox::Yes) {event->ignore();return;}
    if (!maybeSave()) {event->ignore();return;}
    gs2mesh_->cancel();if (queue_) queue_->stop();event->accept();
}

SgsMeshQueue *SgsWindow::meshQueue() {
    if (!queue_) {
        queue_=new SgsMeshQueue(gs2mesh_,[this] {return capture_ ? capture_->path : QString();},this);
        connect(queue_,&SgsMeshQueue::openRequested,this,[this](QString path) {if (!gs2mesh_->running() && maybeSave()) openProject(path);});
        connect(queue_,&SgsMeshQueue::runningChanged,this,[this] {syncMesh();});
    }
    return queue_;
}
void SgsWindow::showMeshQueue() {meshQueue()->show();queue_->raise();queue_->activateWindow();}
bool SgsWindow::queueBusy(const QString &title) {
    if (!queue_ || !queue_->running()) return false;
    QMessageBox::information(this,title,tr("The mesh queue is making meshes, one at a time on the GPU. Wait for it, or stop it (Capture > Mesh queue)."));
    return true;
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
    // The model gs2mesh reads, wherever it was chosen.
    QString text=shown(m.usable() ? m.sparse : project_.colmap)+"\n";
    if (!m.usable()) text+=tr("⚠ %1").arg(m.problem);
    else {
        text+=tr("%1 photos in %2, %3 cameras.").arg(m.photos).arg(shown(m.images)).arg(m.cameras);
        const QString mismatch=colmapMatches(m,capture_->minimum,capture_->maximum);
        if (!mismatch.isEmpty()) text+=tr("\n⚠ %1").arg(mismatch);
    }
    photosInfo_->setText(text);
}
void SgsWindow::syncDisplay() {
    if (!display_) return;
    // a mesh file shown over MCP (view_mesh_file) counts as a mesh until the project's mesh or the splats are shown
    const bool hasMesh=inspectingFile_ || (!project_.meshFiles.isEmpty() && QFileInfo::exists(project_.meshFiles.first()));
    if (!hasMesh && showMesh_) {showMesh_=false;viewport_->setDrawMesh(false);viewport_->setDrawSplats(true);} // back to the splats
    display_->setText(showMesh_ ? tr("Show: Mesh") : tr("Show: Splats"));meshLook_->setText(meshGray_ ? tr("Mesh: Gray") : tr("Mesh: Shaded"));
    const int levels=int(project_.meshObjs().size());
    meshLevelButton_->setText(meshLevel_==0 ? tr("LOD: Main") : tr("LOD: %1").arg(meshLevel_));
    display_->setVisible(hasMesh);meshLook_->setVisible(hasMesh && showMesh_);meshLevelButton_->setVisible(hasMesh && showMesh_ && levels>1);
    syncMeshInfo();
    // The view cube's panel takes the controls' new height.
    viewport_->setDisplayControls(displayControls_);
}
void SgsWindow::syncMeshInfo() {
    // in the mesh view, which mesh and how many triangles, over the controls hint
    if (!showMesh_ || !mesh_) {viewport_->setInfoText({});return;}
    const QString triangles=tr("%1 triangles").arg(QLocale().toString(qlonglong(mesh_->indices.size()/3)));
    const QString which=inspectingFile_ ? tr("File: %1").arg(inspectedName_) : meshLevel_==0 ? tr("LOD: Main") : tr("LOD: %1").arg(meshLevel_);
    viewport_->setInfoText(which+QStringLiteral("   ·   ")+triangles);
}
void SgsWindow::syncMesh() {
    syncDisplay();
    const bool running=gs2mesh_->running();
    const QStringList objs=project_.meshObjs();const bool hasMain=!objs.isEmpty() && QFileInfo::exists(objs.first());
    meshButton_->setEnabled(bool(capture_) && !running);meshCancel_->setVisible(running);meshProgress_->setVisible(running);
    lodsButton_->setEnabled(bool(capture_) && hasMain && !running && Gs2Mesh::makesLods());
    if (!running) meshStage_->clear();
    QString why;
    if (!Gs2Mesh::available(&why)) {meshInfo_->setText(tr("gs2mesh is not available: %1").arg(shown(why)));meshButton_->setEnabled(false);return;}
    if (!capture_) {meshInfo_->clear();return;}
    if (project_.meshFiles.isEmpty()) meshInfo_->setText(tr("None yet. Will be: %1.").arg(project_.mesh.summary()));
    else {
        bool present=true;for (const auto &f:project_.meshFiles) present=present && QFileInfo::exists(f);
        const QString lods=objs.size()>1 ? tr("%1 LODs: %2.").arg(objs.size()-1).arg(project_.mesh.lodsSummary()) : tr("No LODs yet.");
        const bool edited=!project_.meshEdited.isEmpty() || sculptDirty();
        const QString lodsStale=objs.size()>1 && (sculptDirty() || project_.lodsOutOfDate()) ? tr("\n⚠ The LODs are of the mesh before its last edits: generate them again.") : QString();
        meshInfo_->setText(tr("%1\n%2%3. %4%5%6").arg(QDir::toNativeSeparators(QFileInfo(project_.meshFiles.first()).absolutePath()),project_.mesh.summary(),edited ? tr(", edited") : QString(),lods,
            !present ? tr("\n⚠ Some of its files are missing.") : project_.meshOutOfDate() ? tr("\n⚠ Out of date: the PLY, the photos or the settings changed since.") : QString(),lodsStale));
    }
    syncSculpt();
}

void SgsWindow::estimateNormals() {
    if (!capture_) return;
    QString how;fineNormals(&how);
    project_.estimatedNormals=project_.fineNormals=true;viewport_->setFrame(capture_->renderFrame());
    syncCapture();modified();statusBar()->showMessage(how,10000);
}

void SgsWindow::fineNormals(QString *note) {
    if (!capture_) return;
    QProgressDialog progress(tr("Estimating normals, seeing finely…"),QString(),0,0,this);
    progress.setWindowTitle(tr("Estimate normals"));progress.setWindowModality(Qt::WindowModal);progress.setMinimumDuration(0);
    progress.setCancelButton(nullptr);progress.show();
    SgsCapture::NormalsEstimate estimate;
    const SgsCapture *capture=&*capture_;
    QEventLoop wait;
    QThread *worker=QThread::create([&estimate,capture] {estimate=capture->computeNormals(SgsCapture::SurroundingViews,true);});
    connect(worker,&QThread::finished,&wait,&QEventLoop::quit);
    worker->start();wait.exec(QEventLoop::ExcludeUserInputEvents);
    worker->wait();delete worker;
    capture_->applyNormals(std::move(estimate),note);
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
    turnPlay_->setIcon(tinted(style(),turning() ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    turnTime_->setText(tr("%1 s").arg(lightTime_,0,'f',1));
    const QScopedValueRollback<bool> guard(syncing_,true);
    rigTurn_->setValue(project_.rigTurnSeconds);setClockwise(rigDirection_,project_.rigClockwise);
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
    lightTurn_->setValue(l.turnSeconds);setClockwise(lightDirection_,l.clockwise);
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
    else {
        // The model found in what was chosen, so that what is shown and kept is what gs2mesh reads.
        const auto m=findColmap(QDir::cleanPath(path->text().trimmed()));
        project_.colmap=m.usable() ? m.sparse : QDir::cleanPath(path->text().trimmed());project_.colmapDeclined=false;
    }
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
    auto *texture=combo({{"1024",1024},{"2048",2048},{tr("4096 (default)"),4096},{"8192",8192}},s.textureSize);
    auto *voxel=combo({{tr("2 mm (finer, slower)"),2.0},{tr("3 mm (default)"),3.0},{"5 mm",5.0}},double(s.voxelMm));
    auto *normalMap=new QCheckBox(tr("Normal map"));normalMap->setChecked(s.normalMap);
    form->addRow(tr("Main mesh, triangles"),triangles);form->addRow(tr("Texture"),texture);form->addRow(tr("Volume voxel"),voxel);form->addRow(QString(),normalMap);
    layout->addLayout(form);
    // The photos, changeable here too; and the models that use them.
    auto photoText=[this] {
        if (project_.colmap.isEmpty()) return tr("Photos: none, from the splats alone.");
        const auto m=findColmap(project_.colmap);
        return m.usable() ? tr("Photos: %1 in %2, model %3").arg(m.photos).arg(shown(m.images),shown(m.sparse)) : tr("Photos: %1").arg(shown(project_.colmap));};
    auto *photos=note(photoText());
    auto *change=new QPushButton(tr("Change photos…"));auto *photoRow=new QHBoxLayout;photoRow->addWidget(photos,1);photoRow->addWidget(change);layout->addLayout(photoRow);
    connect(change,&QPushButton::clicked,&dialog,[&] {if (askColmap(true)) photos->setText(photoText());});
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
    if (!(next==project_.mesh)) {project_.mesh=next;modified();}
    settings.setValue("Models/Stereo",stereo->text().trimmed());settings.setValue("Models/MoGe",moge->text().trimmed());
    return true;
}

void SgsWindow::generateMesh() {
    if (!capture_ || gs2mesh_->running() || queueBusy(tr("Generate mesh"))) return;
    QString why;if (!Gs2Mesh::available(&why)) {QMessageBox::warning(this,tr("Generate mesh"),tr("gs2mesh is not available:\n\n%1").arg(why));return;}
    if ((!project_.meshEdited.isEmpty() || sculptDirty()) &&
        QMessageBox::question(this,tr("Generate mesh"),tr("The mesh was edited by hand. A new one replaces it, and the edits are lost. Generate it?"))!=QMessageBox::Yes) return;
    // The photos first, when the project does not know them yet; then the resolutions.
    if (!askColmap(false) || !meshDialog()) return;
    startMesh();
}

bool SgsWindow::startMesh() {
    if (!capture_ || gs2mesh_->running() || queueBusy(tr("Generate mesh"))) return false;
    // The mesh is made on the capture as it will be kept: with our fine normals.
    if (!project_.fineNormals) estimateNormals();
    Gs2Mesh::Job job;job.ply=capture_->path;job.outputDir=meshFolder();job.name=QFileInfo(capture_->path).completeBaseName();job.settings=project_.mesh;
    job.settings.lods.clear(); // made afterwards, from the mesh once it looks right (Generate LODs)
    if (!project_.colmap.isEmpty()) {
        const auto m=findColmap(project_.colmap);
        if (!m.usable()) {QMessageBox::warning(this,tr("Generate mesh"),tr("The photos cannot be used:\n\n%1").arg(m.problem));return false;}
        job.sparse=m.sparse;job.images=m.images;
        Gs2Mesh::usePhotoModels(job);
    }
    if (!QDir().mkpath(job.outputDir)) {QMessageBox::warning(this,tr("Generate mesh"),tr("Cannot create %1.").arg(QDir::toNativeSeparators(job.outputDir)));return false;}
    meshLog_.clear();meshProgress_->setValue(0);meshStage_->setText(tr("Starting gs2mesh…"));
    lodsJob_=false;
    const bool started=gs2mesh_->start(job);
    if (started) setMeshBusy(true);
    syncMesh();return started;
}

void SgsWindow::generateLods() {
    if (!capture_ || gs2mesh_->running() || project_.meshObjs().isEmpty() || queueBusy(tr("Generate LODs"))) return;
    QDialog dialog(this);dialog.setWindowTitle(tr("Generate LODs"));auto *layout=new QVBoxLayout(&dialog);auto *form=new QFormLayout;
    QStringList current;for (int l:project_.mesh.lods) current << QString::number(l);
    auto *lods=new QComboBox;lods->setObjectName("lodsChoice");
    const QList<QPair<QString,QString>> choices{{tr("40 000 and 3 000 (default)"),"40000,3000"},{"80 000, 20 000 and 3 000","80000,20000,3000"},
                                                 {"20 000 and 3 000","20000,3000"},{"40 000","40000"},{"3 000","3000"}};
    for (const auto &c:choices) lods->addItem(c.first,c.second);
    lods->setCurrentIndex(std::max(0,lods->findData(current.join(','))));
    form->addRow(tr("LODs, triangles"),lods);layout->addLayout(form);
    layout->addWidget(note(tr("Each LOD is the mesh reduced keeping its texture layout: it shares its albedo and gets a normal map of its own. "
                              "LODs made before are replaced. They go beside the mesh, in %1.").arg(shown(QFileInfo(project_.meshObjs().first()).absolutePath()))));
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Ok)->setText(tr("Generate"));layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if (dialog.exec()!=QDialog::Accepted) return;
    QVector<int> chosen;for (const auto &part:lods->currentData().toString().split(',',Qt::SkipEmptyParts)) chosen.append(part.toInt());
    if (chosen!=project_.mesh.lods) {project_.mesh.lods=chosen;modified();}
    startLods();
}

bool SgsWindow::startLods() {
    const QStringList objs=project_.meshObjs();
    if (!capture_ || gs2mesh_->running() || objs.isEmpty() || project_.mesh.lods.isEmpty() || queueBusy(tr("Generate LODs"))) return false;
    const QFileInfo generated(project_.generatedObj());
    QString dense=generated.absolutePath()+"/"+generated.completeBaseName()+"_dense.ply";
    for (const auto &f:project_.meshFiles) if (f.endsWith("_dense.ply",Qt::CaseInsensitive)) dense=f;
    if (!QFileInfo::exists(dense)) {QMessageBox::warning(this,tr("Generate LODs"),tr("The mesh's dense surface is missing:\n%1\n\nGenerate the mesh again.").arg(QDir::toNativeSeparators(dense)));return false;}
    // Edited by hand: the LODs are made from the edit, and from the dense surface moved alike.
    const bool edited=!project_.meshEdited.isEmpty() || sculptDirty();
    if (edited) {
        if (!saveSculpt()) return false;
        const QString editedDense=QFileInfo(project_.meshEdited).absolutePath()+"/"+QFileInfo(project_.meshEdited).completeBaseName()+"_dense.ply";
        QString error;
        {
            QProgressDialog progress(tr("Moving the dense surface as the mesh was edited…"),QString(),0,0,this);
            progress.setWindowTitle(tr("Generate LODs"));progress.setWindowModality(Qt::WindowModal);progress.setMinimumDuration(0);progress.setCancelButton(nullptr);progress.show();
            const QString generatedPath=generated.absoluteFilePath(),editedPath=project_.meshEdited;
            std::vector<float> now=sculpt_ ? sculpt_->points() : std::vector<float>{};
            QEventLoop wait;
            QThread *worker=QThread::create([&] {
                try {
                    const auto byId=[](const MeshData &m) {
                        std::vector<float> p(m.positionCount*3,0.f);
                        for (size_t v=0;v<m.vertices();++v) for (int a=0;a<3;++a) p[size_t(m.positionIds[v])*3+a]=m.positions[v*3+a];
                        return p;};
                    const std::vector<float> before=byId(readObj(generatedPath));
                    if (now.empty()) now=byId(readObj(editedPath));
                    transferEdits(before,now,dense,editedDense);
                } catch (const std::exception &e) {error=QString::fromUtf8(e.what());}
            });
            connect(worker,&QThread::finished,&wait,&QEventLoop::quit);
            worker->start();wait.exec(QEventLoop::ExcludeUserInputEvents);worker->wait();delete worker;
        }
        if (!error.isEmpty()) {QMessageBox::warning(this,tr("Generate LODs"),tr("Cannot move the dense surface as the mesh:\n\n%1").arg(error));return false;}
        dense=editedDense;
    }
    const QFileInfo main(edited ? project_.meshEdited : objs.first());
    Gs2Mesh::Job job;job.ply=capture_->path;job.outputDir=main.absolutePath();job.name=main.completeBaseName();job.settings=project_.mesh;
    job.lodsFrom=main.absoluteFilePath();job.lodsDense=dense;job.rebakeMain=edited && Gs2Mesh::rebakesMain();
    meshLog_.clear();meshProgress_->setValue(0);meshStage_->setText(tr("Starting gs2mesh…"));
    lodsJob_=true;
    const bool started=gs2mesh_->start(job);
    if (started) setMeshBusy(true);else lodsJob_=false;
    syncMesh();return started;
}

bool SgsWindow::loadMesh(int level) {
    inspectingFile_=false;
    const QStringList objs=project_.meshObjs();
    if (level<0) level=meshLevel_;
    level=std::clamp(level,0,std::max(0,int(objs.size())-1));
    const QString obj=objs.value(level);
    if (level==0 && sculpt_) {mesh_=sculpt_->view();meshLevel_=0;viewport_->setMesh(mesh_);return true;}
    if (obj.isEmpty() || !QFileInfo::exists(obj)) return false;
    if (const auto found=meshCache_.find(obj);found!=meshCache_.end()) {mesh_=found->second;meshLevel_=level;viewport_->setMesh(mesh_);return true;}
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
        if (!data.albedo.isEmpty()) {
            // The LODs share the main mesh's albedo: read once.
            auto image=albedoCache_.find(data.albedo);
            if (image==albedoCache_.end()) image=albedoCache_.insert(data.albedo,QImage(data.albedo));
            mesh->albedo=*image;
        }
        mesh_=mesh;meshCache_[obj]=mesh;meshLevel_=level;viewport_->setMesh(mesh);
    } catch (const std::exception &e) {
        viewport_->setBusyText({});QApplication::restoreOverrideCursor();
        QMessageBox::warning(this,tr("Mesh"),QString::fromUtf8(e.what()));return false;
    }
    viewport_->setBusyText({});QApplication::restoreOverrideCursor();
    return true;
}

// ---- Sculpting
void SgsWindow::setShowMesh(bool mesh) {
    if (!mesh) inspectingFile_=false;
    if (mesh && !mesh_ && !loadMesh(meshLevel_)) return;
    showMesh_=mesh;viewport_->setDrawMesh(mesh);viewport_->setDrawSplats(!mesh);syncDisplay();syncSculpt();
}
bool SgsWindow::canSculpt() const {
    const QStringList objs=project_.meshObjs();
    return capture_ && showMesh_ && meshLevel_==0 && !meshBusy_ && !objs.isEmpty() && (sculpt_ || QFileInfo::exists(objs.first()));
}
QString SgsWindow::sculptToolName(int tool) {
    switch (tool) {
    case Navigate: return tr("Navigate");
    case Move: return tr("Move");
    case Draw: return tr("Draw");
    case Inflate: return tr("Inflate");
    case Pinch: return tr("Pinch");
    case Smooth: return tr("Smooth");
    case Relax: return tr("Relax");
    case Flatten: return tr("Flatten");
    case Erase: return tr("Erase sculpt");
    case Paint: return tr("Paint");
    case Unpaint: return tr("Erase paint");
    case Clone: return tr("Clone");
    case Blur: return tr("Blur");
    case Lighten: return tr("Lighten");
    case Darken: return tr("Darken");
    }
    return {};
}
bool SgsWindow::sculptToolHasOpposite(int tool) {
    return tool==Draw || tool==Inflate || tool==Pinch || tool==Paint || tool==Unpaint || tool==Lighten || tool==Darken;
}
QString SgsWindow::sculptToolHint(int tool) {
    // T4DEditor's words.
    const QString common=(sculptToolHasOpposite(tool) ? tr("Right button: the opposite · ") : QString())
        +tr("Ctrl+wheel: radius · Alt+wheel: hardness (inner circle) · Shift+wheel: strength · off the mesh: left drag orbits, right drag pans · Esc: navigate");
    switch (tool) {
    case Navigate: return tr("Left drag: orbit · right, middle or Shift+drag: pan · wheel: zoom · double click: focus");
    case Move: return tr("Drag to move the surface under the brush · ")+common;
    case Draw: return tr("Paint to push the surface out (Ctrl: in, Alt: smooth) · ")+common;
    case Inflate: return tr("Paint to inflate (Ctrl: deflate, Alt: smooth) · ")+common;
    case Pinch: return tr("Paint to pinch towards the stroke (Ctrl: spread) · ")+common;
    case Smooth: return tr("Paint to smooth the surface · ")+common;
    case Relax: return tr("Paint to even the triangles out and unfold faces turned over, keeping the shape (Alt: smooth) · ")+common;
    case Flatten: return tr("Paint to flatten the surface under the brush · ")+common;
    case Erase: return tr("Paint to bring the surface back to how gs2mesh made it · ")+common;
    case Paint: return tr("Paint color on the texture · ")+common;
    case Unpaint: return tr("Paint to bring the texture back to how gs2mesh made it · ")+common;
    case Clone: return tr("Alt+click sets the source, then paint to copy it · ")+common;
    case Blur: return tr("Paint to blur the texture · ")+common;
    case Lighten: return tr("Paint to lighten the texture · ")+common;
    case Darken: return tr("Paint to darken the texture · ")+common;
    }
    return {};
}
void SgsWindow::setBrush(double radiusCm,double strength,double hardness) {
    {
        const QScopedValueRollback<bool> quiet(syncing_,true);
        brushRadius_->setValue(std::clamp(radiusCm,0.1,100.0));brushStrength_->setValue(std::clamp(strength,1.0,100.0));brushHardness_->setValue(std::clamp(hardness,0.0,95.0));
    }
    syncBrush();
}
void SgsWindow::syncBrush() {
    QSettings settings;settings.setValue("Sculpt/RadiusCm",brushRadius_->value());settings.setValue("Sculpt/Strength",brushStrength_->value());
    settings.setValue("Sculpt/Hardness",brushHardness_->value());settings.setValue("Sculpt/Sphere",brushReach_->currentIndex()==1);
    viewport_->setPaintBrush(float(brushRadius_->value()/100.0),float(brushStrength_->value()/100.0),float(brushHardness_->value()/100.0),sculptToolHasOpposite(sculptTool_));
}
bool SgsWindow::ensureSculpt() {
    if (sculpt_) return true;
    const QStringList objs=project_.meshObjs();
    if (objs.isEmpty() || !QFileInfo::exists(objs.first())) return false;
    QApplication::setOverrideCursor(Qt::WaitCursor);viewport_->setBusyText(tr("Making the mesh editable…"));
    try {
        const MeshData data=readObj(objs.first());
        QImage albedo;
        if (!data.albedo.isEmpty()) {auto image=albedoCache_.find(data.albedo);if (image==albedoCache_.end()) image=albedoCache_.insert(data.albedo,QImage(data.albedo));albedo=*image;}
        sculpt_=std::make_unique<SculptMesh>(data,albedo);sculptSaved_=sculpt_->state();
        // An edit read back with its own albedo: Unpaint goes back to the generated one.
        const QFileInfo generated(project_.generatedObj());
        const QString made=materialAlbedo(generated.absolutePath()+"/"+generated.completeBaseName()+".mtl");
        if (!made.isEmpty() && QFileInfo(made)!=QFileInfo(data.albedo) && QFileInfo::exists(made)) sculpt_->setOriginalAlbedo(QImage(made));
    } catch (const std::exception &e) {
        viewport_->setBusyText({});QApplication::restoreOverrideCursor();
        QMessageBox::warning(this,tr("Sculpt"),QString::fromUtf8(e.what()));return false;
    }
    viewport_->setBusyText({});QApplication::restoreOverrideCursor();
    meshCache_[objs.first()]=sculpt_->view();
    if (meshLevel_==0) {mesh_=sculpt_->view();viewport_->setMesh(mesh_);}
    syncMeshInfo();
    return true;
}
void SgsWindow::setSculptTool(int tool) {
    tool=std::clamp(tool,int(Navigate),int(Darken));
    if (tool!=Navigate && (!canSculpt() || !ensureSculpt())) tool=Navigate;
    if (isTextureTool(tool) && !sculpt_->hasAlbedo()) {statusBar()->showMessage(tr("This mesh has no texture to paint on."),6000);tool=Navigate;}
    if (tool==sculptTool_) {if (sculptTools_.value(tool)) sculptTools_[tool]->setChecked(true);return;}
    const bool wasNavigating=sculptTool_==Navigate;
    if (strokeLive_ && sculpt_) {strokeLive_=false;if (sculpt_->endStroke()) sculptChanged();}
    sculptTool_=tool;
    if (sculptTools_.value(tool)) sculptTools_[tool]->setChecked(true);
    // The brushes' button shows the last one chosen, checked while one is the tool.
    if (brushMenu_) {
        const bool texture=isTextureTool(tool);
        QToolButton *menu=texture ? textureMenu_ : brushMenu_;
        if (tool!=Navigate) {menu->setIcon(sculptIcon(tool));menu->setText(sculptToolName(tool));}
        brushMenu_->setChecked(tool!=Navigate && !texture);textureMenu_->setChecked(texture);
    }
    // A mesh brush shows the shape: grey, lit to read it; a texture brush the texture; Navigate
    // gives back the look there was.
    if (tool!=Navigate && wasNavigating) grayBeforeSculpt_=meshGray_;
    if (tool==Navigate) {if (!wasNavigating) {meshGray_=grayBeforeSculpt_;viewport_->setMeshTextured(!meshGray_);viewport_->setMeshShapeLight(false);}}
    else if (isTextureTool(tool)) {meshGray_=false;viewport_->setMeshTextured(true);viewport_->setMeshShapeLight(false);}
    else {meshGray_=true;viewport_->setMeshTextured(false);viewport_->setMeshShapeLight(true);}
    syncDisplay();
    viewport_->setPaintHint(tool==Navigate ? QString() : sculptToolHint(tool));
    viewport_->setSelectTool(tool==Navigate ? Viewport::SelectTool::None : Viewport::SelectTool::Paint,0);
    syncBrush();
}
void SgsWindow::textureDab(const QPointF &position) {
    QVector3D o,d,hit;viewport_->captureRay(position,&o,&d);if (!sculpt_->raycast(o,d,&hit)) return;
    if (sculptTool_==Clone && !cloneOffsetSet_) {cloneOffset_=cloneSource_-hit;cloneOffsetSet_=true;}
    SculptMesh::TextureDab dab;dab.centre=hit;dab.radius=float(brushRadius_->value()/100.0);dab.hardness=float(brushHardness_->value()/100.0);
    dab.opacity=float(brushStrength_->value()/100.0);dab.colour=paintColour_;dab.cloneOffset=cloneOffset_;
    dab.eye=viewport_->transform().matrix().inverted().map(viewport_->camera().position());
    const QRect changed=sculpt_->textureDab(dab);
    if (!changed.isNull()) viewport_->meshTextureChanged(changed);
}
void SgsWindow::sculptStroke(const QPointF &position,int phase,Qt::MouseButton button,Qt::KeyboardModifiers modifiers) {
    if (sculptTool_==Navigate || !sculpt_) return;
    if (isTextureTool(sculptTool_)) {
        if (phase==0) {
            strokeLive_=false;
            QVector3D o,d,hit;viewport_->captureRay(position,&o,&d);if (!sculpt_->raycast(o,d,&hit)) return;
            if (sculptTool_==Clone && (modifiers & Qt::AltModifier)) {
                cloneSource_=hit;cloneSourceSet_=true;cloneOffsetSet_=false;
                statusBar()->showMessage(tr("Clone source set. Paint where it should be copied."),6000);return;
            }
            if (sculptTool_==Clone && !cloneSourceSet_) {statusBar()->showMessage(tr("Alt+click the clone source first."),6000);return;}
            using T=SculptMesh::TextureBrush;
            static const T brushes[]={T::Paint,T::Unpaint,T::Clone,T::Blur,T::Lighten,T::Darken};
            T kind=brushes[sculptTool_-Paint];
            // The right button: the opposite (paint and erase, lighten and darken).
            if (button==Qt::RightButton) kind=kind==T::Paint ? T::Unpaint : kind==T::Unpaint ? T::Paint : kind==T::Lighten ? T::Darken : kind==T::Darken ? T::Lighten : kind;
            sculpt_->beginTextureStroke(kind);strokeLive_=true;
            textureDab(position);lastDab_=position;return;
        }
        if (!strokeLive_) return;
        if (phase==1) {
            // Dabs every quarter radius along the stroke.
            QVector3D o,d,hit;viewport_->captureRay(position,&o,&d);
            const double pixels=sculpt_->raycast(o,d,&hit) ? brushRadius_->value()/100.0/std::max(1e-9f,viewport_->pixelSizeAt(hit)) : 16.0;
            const QPointF way=position-lastDab_;const double length=std::hypot(way.x(),way.y()),spacing=std::max(2.0,pixels*0.25);
            const int steps=int(length/spacing);
            for (int i=1;i<=steps;++i) textureDab(lastDab_+way*(i*spacing/length));
            if (steps>0) lastDab_+=way*(steps*spacing/length);
            return;
        }
        strokeLive_=false;
        if (sculpt_->endStroke()) {sculpt_->padAlbedo();viewport_->meshTextureChanged();sculptChanged();}
        return;
    }
    // The right button paints the opposite, as Ctrl does.
    if (button==Qt::RightButton) modifiers^=Qt::ControlModifier;
    const float radius=float(brushRadius_->value()/100.0);
    auto dabAt=[&](const QPointF &at,SculptMesh::Dab *d) {
        QVector3D o,dir;viewport_->captureRay(at,&o,&dir);int triangle=-1;
        if (!sculpt_->raycast(o,dir,&d->centre,nullptr,&triangle)) return false;
        d->triangle=triangle;d->radius=radius;d->strength=float(brushStrength_->value()/100.0);d->hardness=float(brushHardness_->value()/100.0);
        d->alongSurface=brushReach_->currentIndex()==0;d->invert=modifiers & Qt::ControlModifier;d->smooth=modifiers & Qt::AltModifier;
        return true;};
    static const SculptMesh::Brush brushes[]={SculptMesh::Brush::Smooth,SculptMesh::Brush::Move,SculptMesh::Brush::Draw,SculptMesh::Brush::Inflate,
        SculptMesh::Brush::Pinch,SculptMesh::Brush::Smooth,SculptMesh::Brush::Relax,SculptMesh::Brush::Flatten,SculptMesh::Brush::Erase};
    if (phase==0) {
        SculptMesh::Dab d;if (!dabAt(position,&d)) {strokeLive_=false;return;}
        strokeBrush_=brushes[sculptTool_];strokeLive_=true;
        sculpt_->beginStroke(strokeBrush_,d);
        if (strokeBrush_==SculptMesh::Brush::Move) {QVector3D o;viewport_->captureRay(position,&o,&grabNormal_);grabOrigin_=d.centre;}
        else sculpt_->dab(d);
        lastDab_=position;viewport_->meshGeometryChanged();return;
    }
    if (!strokeLive_) return;
    if (phase==1) {
        if (strokeBrush_==SculptMesh::Brush::Move) {
            // On the plane through where it began, facing the view.
            QVector3D o,d;viewport_->captureRay(position,&o,&d);
            const float along=QVector3D::dotProduct(d,grabNormal_);
            if (std::abs(along)>1e-6f) sculpt_->moveBy(o+d*(QVector3D::dotProduct(grabOrigin_-o,grabNormal_)/along)-grabOrigin_);
        } else {
            // Dabs every quarter radius along the stroke.
            SculptMesh::Dab here;
            const double pixels=dabAt(position,&here) ? radius/std::max(1e-9f,viewport_->pixelSizeAt(here.centre)) : 16.0;
            const QPointF way=position-lastDab_;const double length=std::hypot(way.x(),way.y()),spacing=std::max(2.0,pixels*0.25);
            const int steps=int(length/spacing);
            for (int i=1;i<=steps;++i) {SculptMesh::Dab d;if (dabAt(lastDab_+way*(i*spacing/length),&d)) sculpt_->dab(d);}
            if (steps>0) lastDab_+=way*(steps*spacing/length);
        }
        viewport_->meshGeometryChanged();return;
    }
    strokeLive_=false;
    if (sculpt_->endStroke()) sculptChanged();
}
void SgsWindow::sculptChanged() {
    viewport_->meshGeometryChanged();modified();syncMesh();
}
void SgsWindow::fixSpikes() {
    if (!canSculpt() || !ensureSculpt()) {statusBar()->showMessage(tr("Fix spikes works on the main mesh, in mesh view."),6000);return;}
    QApplication::setOverrideCursor(Qt::WaitCursor);const int found=sculpt_->fixSpikes();QApplication::restoreOverrideCursor();
    if (found>0) sculptChanged();
    statusBar()->showMessage(found>0 ? tr("%1 points of spikes and folds smoothed with what is around them (Ctrl+Z undoes it).").arg(thousands(found)) : tr("No spikes or folds found."),8000);
}
void SgsWindow::removeLoosePieces() {
    if (!canSculpt() || !ensureSculpt()) {statusBar()->showMessage(tr("Remove loose pieces works on the main mesh, in mesh view."),6000);return;}
    QApplication::setOverrideCursor(Qt::WaitCursor);const int removed=sculpt_->removeLoosePieces();QApplication::restoreOverrideCursor();
    if (removed>0) sculptChanged();
    statusBar()->showMessage(removed>0 ? tr("%1 triangles of loose pieces removed (Ctrl+Z undoes it).").arg(thousands(removed)) : tr("No loose pieces found."),8000);
}
void SgsWindow::undoSculpt() {
    if (!sculpt_ || !sculpt_->canUndo() || meshBusy_ || strokeLive_) return;
    if (!sculpt_->undo().isNull()) {sculpt_->padAlbedo();viewport_->meshTextureChanged();}
    sculptChanged();
}
void SgsWindow::redoSculpt() {
    if (!sculpt_ || !sculpt_->canRedo() || meshBusy_ || strokeLive_) return;
    if (!sculpt_->redo().isNull()) {sculpt_->padAlbedo();viewport_->meshTextureChanged();}
    sculptChanged();
}
void SgsWindow::syncPaintColour() {
    if (!paintColourButton_) return;
    paintColourButton_->setStyleSheet(QStringLiteral("QPushButton#paintColour { background: %1; }").arg(paintColour_.name()));
    paintColourButton_->setText(paintColour_.name().toUpper());
    paintColourButton_->setStyleSheet(QStringLiteral("QPushButton#paintColour { background: %1; color: %2; }")
        .arg(paintColour_.name(),paintColour_.lightnessF()>0.6 ? QStringLiteral("#202020") : QStringLiteral("#f0f0f0")));
}
bool SgsWindow::sculptDirty() const {return sculpt_ && sculpt_->state()!=sculptSaved_;}
QString SgsWindow::editedObjPath() const {
    const QFileInfo generated(project_.generatedObj());
    return generated.absolutePath()+"/"+generated.completeBaseName()+"_edited.obj";
}
bool SgsWindow::saveSculpt() {
    if (!sculptDirty()) return true;
    const QString path=editedObjPath();const QFileInfo info(path);
    const QString mtl=info.absolutePath()+"/"+info.completeBaseName()+".mtl";
    try {
        const MeshData data=sculpt_->mesh();
        // Its own material, a copy of the generated one at first: rebaking writes its normal map there.
        if (!QFileInfo::exists(mtl) && !data.material.isEmpty() && QFileInfo::exists(data.material) && !QFile::copy(data.material,mtl))
            throw std::runtime_error(QStringLiteral("Cannot write %1.").arg(mtl).toStdString());
        // Painted: its albedo beside it, <name>_edited_albedo.png, named by its material.
        if (sculpt_->albedoPainted() && QFileInfo::exists(mtl)) {
            const QString albedo=info.absolutePath()+"/"+info.completeBaseName()+"_albedo.png";
            if (!sculpt_->albedo().save(albedo,"PNG")) throw std::runtime_error(QStringLiteral("Cannot write %1.").arg(albedo).toStdString());
            setMaterialAlbedo(mtl,QFileInfo(albedo).fileName());
            albedoCache_.remove(albedo);sculpt_->markAlbedoSaved();
        }
        writeObj(path,data,QFileInfo::exists(mtl) ? info.completeBaseName()+".mtl" : QString(),materialName(mtl));
    } catch (const std::exception &e) {QMessageBox::warning(this,tr("Save the edited mesh"),QString::fromUtf8(e.what()));return false;}
    const bool first=project_.meshEdited!=path;
    project_.meshEdited=path;sculptSaved_=sculpt_->state();
    if (first) {meshCache_.erase(project_.generatedObj());meshCache_[path]=sculpt_->view();}
    modified();syncMesh();
    return true;
}
void SgsWindow::revertSculpt(bool ask) {
    if (project_.meshEdited.isEmpty() && !sculptDirty()) return;
    if (ask && QMessageBox::question(this,tr("Revert to generated"),tr("Go back to the mesh as gs2mesh made it? The edits are lost."))!=QMessageBox::Yes) return;
    setSculptTool(Navigate);
    if (!project_.meshEdited.isEmpty()) {
        const QFileInfo info(project_.meshEdited);const QString base=info.absolutePath()+"/"+info.completeBaseName();
        meshCache_.erase(project_.meshEdited);
        for (const QString &f:{project_.meshEdited,base+".mtl",base+"_normal.png",base+"_ao.png",base+"_dense.ply",base+"_albedo.png"}) QFile::remove(f);
    }
    project_.meshEdited.clear();sculpt_.reset();
    if (showMesh_ && meshLevel_==0) loadMesh(0);
    modified();syncMesh();
    statusBar()->showMessage(tr("The mesh is as gs2mesh made it."),6000);
}
void SgsWindow::syncSculpt() {
    if (!sculptSection_) return;
    const bool on=canSculpt();
    if (!on && sculptTool_!=Navigate) setSculptTool(Navigate);
    sculptSection_->setVisible(on);if (sculptToolbar_) sculptToolbar_->setVisible(on);
    const bool undo=sculpt_ && sculpt_->canUndo() && !meshBusy_,redo=sculpt_ && sculpt_->canRedo() && !meshBusy_;
    sculptUndo_->setEnabled(undo);sculptRedo_->setEnabled(redo);
    if (undoAction_) {undoAction_->setEnabled(undo);redoAction_->setEnabled(redo);}
    const bool edited=!project_.meshEdited.isEmpty() || sculptDirty();
    sculptRevert_->setEnabled(edited && !meshBusy_);
    sculptInfo_->setText(sculptDirty() ? tr("Edited; written with the project (or the LODs, or an export).")
                         : edited ? tr("Edited: %1").arg(QFileInfo(project_.meshEdited).fileName())
                                  : tr("As gs2mesh made it. The LODs are made from what this becomes."));
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
    if (sculptDirty() && !saveSculpt()) throw std::runtime_error("The edited mesh could not be written.");
    if (project_.sgsIncludesMesh) {
        const QString obj=project_.sgsMeshFile();
        if (!obj.isEmpty() && QFileInfo::exists(obj)) {
            const MeshData mesh=readObj(obj);
            GlbOptions glb;glb.textureSize=project_.sgsTextureSize;
            options.glb=writeGlb(mesh,mesh.albedo.isEmpty() ? QImage() : QImage(mesh.albedo),mesh.normalMap.isEmpty() ? QImage() : QImage(mesh.normalMap),glb,
                                 mesh.occlusion.isEmpty() ? QImage() : QImage(mesh.occlusion));
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
    if (objs.size()==1) layout->addWidget(note(tr("⚠ No LODs: the main mesh, %1 triangles, would be carried. Generate LODs makes lighter ones.").arg(thousands(project_.mesh.triangles))));
    form->addRow(QString(),include);form->addRow(tr("Mesh"),which);form->addRow(tr("Its textures, longest side"),texture);layout->addLayout(form);
    layout->addWidget(note(tr("The .sgs holds the splats with their normals, a thumbnail of this view and, when carried, the mesh as one .glb for shadows, collision and a stand-in.")));
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Ok)->setText(tr("Export"));layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if (dialog.exec()!=QDialog::Accepted) return false;
    project_.sgsIncludesMesh=hasMesh ? include->isChecked() : project_.sgsIncludesMesh;
    if (hasMesh) {const int at=which->currentIndex();project_.sgsMesh=at==which->count()-1 ? -1 : at;project_.sgsTextureSize=texture->currentData().toInt();}
    modified();
    if (hasMesh && project_.sgsIncludesMesh && project_.sgsMesh!=0 && objs.size()>1 && (sculptDirty() || project_.lodsOutOfDate()) &&
        QMessageBox::question(this,tr("Export .sgs"),tr("The LODs are of the mesh before its last edits. Export one anyway?"))!=QMessageBox::Yes) return false;
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
