// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See ../LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#include "viewport.h"
#include "viewcube.h"
#include "editortheme.h"
#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QSurfaceFormat>
#include <QPainterPath>
#include <QtTest>
#include <cmath>
#include <memory>

class ViewportTests : public QObject {
    Q_OBJECT
    static void drag(Viewport &viewport,QPoint from,QPoint to,Qt::MouseButton button=Qt::LeftButton) {
        QTest::mousePress(&viewport,button,Qt::NoModifier,from);
        QMouseEvent move(QEvent::MouseMove,QPointF(to),QPointF(viewport.mapToGlobal(to)),Qt::NoButton,button,Qt::NoModifier);
        QApplication::sendEvent(&viewport,&move);
        QTest::mouseRelease(&viewport,button,Qt::NoModifier,to);
    }
    static QPoint colouredHandle(Viewport &viewport,int channel) {
        const QImage image = viewport.grabFramebuffer();
        const double dpr = viewport.devicePixelRatioF();
        const QPointF center(image.width()*0.5,image.height()*0.5);
        double best = 0; QPoint point;
        for (int y=0; y<image.height(); ++y) for (int x=0; x<image.width(); ++x) {
            const auto colour = image.pixelColor(x,y);
            const int values[] = {colour.red(),colour.green(),colour.blue()};
            if (values[channel]<180 || values[(channel+1)%3]>155 || values[(channel+2)%3]>155) continue;
            // The crop wire is pink/red; identify the red gizmo by equal G/B.
            if (channel==0 && std::abs(colour.green()-colour.blue())>12) continue;
            const double distance = QLineF(center,QPointF(x,y)).length();
            if (distance>best) { best = distance; point = QPoint(qRound(x/dpr),qRound(y/dpr)); }
        }
        return point;
    }
private slots:
    void brushAndLassoPickWhatTheyCover() {
        // A row of five points across the view; the camera looks at the middle one.
        Viewport viewport;viewport.resize(400,300);auto frame=std::make_shared<RenderFrame>();
        for (int i=0;i<5;++i) {Splat s{};s.position[0]=(i-2)*.5f;s.rotation[3]=1;s.scale[0]=s.scale[1]=s.scale[2]=.01f;s.color[3]=1;s.id=float(i);frame->records.push_back(s);frame->active.push_back(1);
            PointVertex p{};p.position[0]=s.position[0];p.id=s.id;frame->points.push_back(p);}
        viewport.setFrame(frame);Camera camera;camera.target={0,0,0};camera.yaw=0;camera.pitch=0;camera.distance=4;viewport.setCamera(camera);
        QPointF centre(200,150);
        QPainterPath lasso;lasso.addRect(QRectF(centre.x()-60,centre.y()-20,120,40));QCOMPARE(viewport.recordsInside(lasso),(std::vector<uint32_t>{1,2,3}));
        QPainterPath dab(centre);dab.lineTo(centre);QCOMPARE(viewport.recordsInside(Viewport::brushRegion(dab,10)),(std::vector<uint32_t>{2}));
        QPainterPath sweep(QPointF(10,150));sweep.lineTo(QPointF(390,150));QCOMPARE(viewport.recordsInside(Viewport::brushRegion(sweep,8)),(std::vector<uint32_t>{0,1,2,3,4}));
        // A stroke with the mouse: Ctrl adds, Alt subtracts, nothing replaces.
        viewport.setSelectTool(Viewport::SelectTool::Brush,10);std::vector<uint32_t> got;Viewport::SelectMode mode{};
        connect(&viewport,&Viewport::selectionStroke,this,[&](std::vector<uint32_t> r,Viewport::SelectMode m) {got=std::move(r);mode=m;});
        QTest::mousePress(&viewport,Qt::LeftButton,Qt::ControlModifier,centre.toPoint());QTest::mouseRelease(&viewport,Qt::LeftButton,Qt::ControlModifier,centre.toPoint());
        QCOMPARE(got,(std::vector<uint32_t>{2}));QCOMPARE(mode,Viewport::SelectMode::Add);
        QTest::mousePress(&viewport,Qt::LeftButton,Qt::AltModifier,centre.toPoint());QTest::mouseRelease(&viewport,Qt::LeftButton,Qt::AltModifier,centre.toPoint());QCOMPARE(mode,Viewport::SelectMode::Subtract);
        QTest::mousePress(&viewport,Qt::LeftButton,Qt::NoModifier,centre.toPoint());QTest::mouseRelease(&viewport,Qt::LeftButton,Qt::NoModifier,centre.toPoint());QCOMPARE(mode,Viewport::SelectMode::Replace);
        // Picking does not orbit with the left button; the right one does.
        const auto before=viewport.camera();
        QTest::mousePress(&viewport,Qt::RightButton,Qt::NoModifier,{100,100});
        QMouseEvent move(QEvent::MouseMove,QPointF(150,100),QPointF(viewport.mapToGlobal(QPoint(150,100))),Qt::NoButton,Qt::RightButton,Qt::NoModifier);QApplication::sendEvent(&viewport,&move);
        QTest::mouseRelease(&viewport,Qt::RightButton,Qt::NoModifier,{150,100});QVERIFY(viewport.camera().yaw!=before.yaw);
        viewport.setSelectTool(Viewport::SelectTool::None,10);
    }
    void doubleClickGlidesThePivotToWhatIsSeen() {
        // Two splats on the line of sight: an opaque one in front and one behind it.
        Viewport viewport;viewport.resize(400,300);auto frame=std::make_shared<RenderFrame>();
        auto add=[&](float z,float opacity) {Splat s{};s.position[2]=z;s.rotation[3]=1;s.scale[0]=s.scale[1]=s.scale[2]=.05f;s.color[3]=opacity;s.id=float(frame->records.size());
            frame->records.push_back(s);frame->active.push_back(1);PointVertex p{};p.position[2]=z;p.id=s.id;frame->points.push_back(p);};
        add(.5f,.9f);add(-.5f,.9f);viewport.setFrame(frame);viewport.setSplatRendering(true);
        Camera camera;camera.target={0,0,0};camera.yaw=0;camera.pitch=0;camera.distance=4;viewport.setCamera(camera);
        const QPointF centre(200,150);QVector3D point;
        QVERIFY(viewport.pickPoint(centre,&point));QVERIFY((point-QVector3D(0,0,.5f)).length()<1e-3f);
        // A faint splat in front lets the eye through to the one behind.
        frame->records[0].color[3]=.05f;QVERIFY(viewport.pickPoint(centre,&point));QVERIFY((point-QVector3D(0,0,-.5f)).length()<1e-3f);
        frame->records[0].color[3]=.9f;
        // Empty space picks nothing.
        QVERIFY(!viewport.pickPoint({10,10},&point));
        // As points: the nearest point under the cursor.
        viewport.setSplatRendering(false);QVERIFY(viewport.pickPoint(centre,&point));QVERIFY((point-QVector3D(0,0,.5f)).length()<1e-3f);viewport.setSplatRendering(true);
        // A double-click glides the pivot there, keeping the angle and the distance; a click on
        // nothing leaves it alone.
        camera.target={.01f,.005f,0};viewport.setCamera(camera);const QPoint below=centre.toPoint();
        QVector3D target;QVERIFY(viewport.pickPoint(below,&target));
        QTest::mouseDClick(&viewport,Qt::LeftButton,Qt::NoModifier,below);QVERIFY(viewport.focusing());
        QTRY_VERIFY_WITH_TIMEOUT(!viewport.focusing(),2000);
        QVERIFY((viewport.camera().target-target).length()<1e-4f);QCOMPARE(viewport.camera().distance,4.f);QCOMPARE(viewport.camera().yaw,0.f);
        const auto before=viewport.camera().target;QTest::mouseDClick(&viewport,Qt::LeftButton,Qt::NoModifier,{5,5});QVERIFY(!viewport.focusing());QCOMPARE(viewport.camera().target,before);
        // Navigating cancels a glide in progress.
        viewport.focusOn({1,1,1});QVERIFY(viewport.focusing());QTest::mousePress(&viewport,Qt::LeftButton,Qt::NoModifier,{5,5});QVERIFY(!viewport.focusing());
        QTest::mouseRelease(&viewport,Qt::LeftButton,Qt::NoModifier,{5,5});
    }
    void focusOnlyIncludesVisibleWorldPointsAndPreservesModes() {
        Viewport viewport;viewport.resize(500,500);auto frame=std::make_shared<RenderFrame>();
        frame->points={{{0,1,0},{1,0,0},0},{{100,1,0},{0,1,0},1},{{-100,1,0},{1,0,0},2,0}};viewport.setFrame(frame);
        Modifier crop;crop.crop.enabled=true;crop.crop.shape=CropShape::Box;crop.crop.width=crop.crop.depth=2;crop.crop.height=2;viewport.setModifiers({crop});viewport.setTransformMode(TransformMode::Scale);
        QVERIFY(viewport.focusVisible());QCOMPARE(viewport.camera().target,QVector3D(0,1,0));QVERIFY(viewport.camera().distance<1);QCOMPARE(viewport.transformMode(),TransformMode::Scale);
        Transform moved;moved.position={2,0,0};viewport.setTransform(moved);const auto camera=viewport.camera();QVERIFY(!viewport.focusVisible());QCOMPARE(viewport.camera().target,camera.target);
        crop.crop.transform.position={2,0,0};viewport.setModifiers({crop});QVERIFY(viewport.focusVisible());QCOMPARE(viewport.camera().target,QVector3D(2,1,0));
        Modifier green;green.type=ModifierType::RemoveGreen;Modifier purge;purge.type=ModifierType::PurgeIsolated;viewport.setModifiers({green,purge});QVERIFY(viewport.focusVisible());QCOMPARE(viewport.camera().target,QVector3D(2,1,0));
        qRegisterMetaType<ViewPreset>();QSignalSpy requested(&viewport,&Viewport::frameRequested);viewport.show();QVERIFY(QTest::qWaitForWindowExposed(&viewport));viewport.setFocus();
        QTest::keyClick(&viewport,Qt::Key_Delete,Qt::KeypadModifier);QCOMPARE(requested.size(),1);QTest::keyClick(&viewport,Qt::Key_Period,Qt::KeypadModifier);QCOMPARE(requested.size(),2);QTest::keyClick(&viewport,Qt::Key_Delete);QCOMPARE(requested.size(),2);
    }
    void ghostFreezesWorldPoseAndBlendsOncePerPixel() {
        Viewport viewport;viewport.resize(600,450);viewport.setGrid(false);viewport.setPointSize(9);Camera camera;camera.yaw=camera.pitch=0;camera.distance=4;viewport.setCamera(camera);
        auto first=std::make_shared<RenderFrame>();first->seconds=.25;first->points={{{-.7f,0,0},{1,0,0},0},{{-.7f,0,0},{1,0,0},1},{{-.7f,0,0},{1,0,0},2}};viewport.setFrame(first);viewport.show();QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        QVERIFY(!viewport.ghostEnabled());QCOMPARE(viewport.ghostPointCount(),size_t(0));QVERIFY(viewport.setGhost(true));QCOMPARE(viewport.ghostPointCount(),size_t(3));QCOMPARE(viewport.ghostTime(),.25);
        auto second=std::make_shared<RenderFrame>();second->seconds=.75;second->points={{{.7f,0,0},{0,0,1},0}};viewport.setFrame(second);Transform moved;moved.position={.1f,0,0};viewport.setTransform(moved);
        const auto image=viewport.grabFramebuffer();int ghostPixels=0,bluePixels=0;int brightest=0;
        for (int y=int(image.height()*.2);y<int(image.height()*.8);++y) for (int x=int(image.width()*.1);x<int(image.width()*.7);++x) {const auto c=image.pixelColor(x,y);if (c.blue()>200 && c.red()<20 && c.green()<20) ++bluePixels;
            if (x<image.width()/2 && c.red()>35 && std::abs(c.red()-c.green())<6 && std::abs(c.red()-c.blue())<6) {++ghostPixels;brightest=std::max(brightest,c.red());}}
        QVERIFY(ghostPixels>10);QVERIFY(bluePixels>10);QVERIFY2(brightest<140,"Overlapping ghost points must remain translucent rather than accumulating opacity.");QCOMPARE(viewport.ghostTime(),.25);
        QCOMPARE(viewport.ghostOpacity(),.15f);
        auto ghostBrightness=[&] {const auto rendered=viewport.grabFramebuffer();int peak=0;for (int y=int(rendered.height()*.2);y<int(rendered.height()*.8);++y) for (int x=int(rendered.width()*.1);x<rendered.width()/2;++x) peak=std::max(peak,rendered.pixelColor(x,y).red());return peak;};
        viewport.setGhostOpacity(.6f);QVERIFY(ghostBrightness()>brightest+40);QCOMPARE(viewport.ghostTime(),.25);QCOMPARE(viewport.ghostPointCount(),size_t(3));
        viewport.setGhostOpacity(0);QVERIFY(ghostBrightness()<30);QVERIFY(viewport.ghostEnabled());viewport.setGhostOpacity(.15f);
        QVERIFY(viewport.focusVisible());QVERIFY(std::abs(viewport.camera().target.x()-.05f)<1e-5f);
        QVERIFY(!viewport.setGhost(false));QCOMPARE(viewport.ghostPointCount(),size_t(0));QVERIFY(viewport.focusVisible());QVERIFY(std::abs(viewport.camera().target.x()-.8f)<1e-5f);
        const auto disabled=viewport.grabFramebuffer();int grey=0;for (int y=int(disabled.height()*.2);y<int(disabled.height()*.8);++y) for (int x=int(disabled.width()*.1);x<int(disabled.width()*.48);++x) {const auto c=disabled.pixelColor(x,y);if (c.red()>35 && std::abs(c.red()-c.green())<6 && std::abs(c.red()-c.blue())<6) ++grey;}QCOMPARE(grey,0);
        QVERIFY(viewport.setGhost(true));viewport.setFrame({});QVERIFY(!viewport.ghostEnabled());QCOMPARE(viewport.ghostPointCount(),size_t(0));
    }
    void modifierPreviewUsesCropUnionAndOriginalRgb() {
        Viewport viewport;viewport.resize(600,450);viewport.setGrid(false);auto frame=std::make_shared<RenderFrame>();frame->coefficients=0;
        for (int i=0;i<3;++i) {PointVertex point{};point.position[0]=i==0 ? -1.5f : 1.5f;point.position[1]=1;point.color[i]=1;point.id=float(i);frame->points.push_back(point);}
        frame->records.resize(3);viewport.setFrame(frame);Camera camera;camera.target={0,1,0};camera.yaw=camera.pitch=0;camera.distance=6;viewport.setCamera(camera);
        frame->points[1].color[0]=frame->points[1].color[2]=.35f;frame->points[1].color[1]=.6f; // Desaturated sRGB green matches the linear-RGB default.
        Modifier first;first.crop.enabled=true;first.crop.radius=.5f;first.crop.height=2;first.crop.transform.position={-1.5f,0,0};Modifier second=first;second.crop.transform.position={1.5f,0,0};
        Modifier green;green.type=ModifierType::RemoveGreen;viewport.setModifiers({first,second,green});viewport.show();QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        auto count=[&](int channel) {const auto image=viewport.grabFramebuffer();int n=0;for (int y=160;y<image.height();++y) for (int x=0;x<image.width();++x) {const auto c=image.pixelColor(x,y);const int rgb[3]={c.red(),c.green(),c.blue()};if (rgb[channel]>150 && rgb[(channel+1)%3]<100 && rgb[(channel+2)%3]<100) ++n;}return n;};
        QVERIFY(count(0)>0);QCOMPARE(count(1),0);QVERIFY(count(2)>0);
        first.enabled=false;viewport.setModifiers({first,second,green});QCOMPARE(count(0),0);QVERIFY(count(2)>0);
        second.enabled=false;green.enabled=false;viewport.setModifiers({first,second,green});QVERIFY(count(0)>0);QVERIFY(count(1)>0);
        QCOMPARE(frame->points.size(),size_t(3));
    }
    void orthographicBackgroundHasNoColouredAxes() {
        Viewport viewport;viewport.resize(600,450);viewport.show();QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        auto coloured=[&] {const auto image=viewport.grabFramebuffer();int count=0;for (int y=0;y<image.height();++y) for (int x=0;x<400;++x) {const auto c=image.pixelColor(x,y);if (std::max({c.red(),c.green(),c.blue()})-std::min({c.red(),c.green(),c.blue()})>70) ++count;}return count;};
        QVERIFY(coloured()>10);viewport.setViewPreset(ViewPreset::Top,true);QCOMPARE(coloured(),0);viewport.setViewPreset(ViewPreset::Front,true);QCOMPARE(coloured(),0);
    }
    void modeShortcutsAndNavigation() {
        Viewport viewport; viewport.resize(500,500); viewport.setGrid(false);
        viewport.setFrame(std::make_shared<RenderFrame>()); viewport.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewport)); viewport.setFocus();
        for (int group=0;group<3;++group) {
            const auto mode=TransformMode(group+1);const auto key=group==0 ? Qt::Key_G : group==1 ? Qt::Key_R : Qt::Key_S;
            QTest::keyClick(&viewport,key);QCOMPARE(viewport.transformMode(),mode);QCOMPARE(viewport.coordinateSpace(mode),CoordinateSpace::Global);
            QTest::keyClick(&viewport,key);QCOMPARE(viewport.transformMode(),mode);QCOMPARE(viewport.coordinateSpace(mode),CoordinateSpace::Local);
            QTest::keyClick(&viewport,key);QCOMPARE(viewport.transformMode(),mode);QCOMPARE(viewport.coordinateSpace(mode),CoordinateSpace::Global);
            QKeyEvent repeat(QEvent::KeyPress,key,Qt::NoModifier,QString(),true);QApplication::sendEvent(&viewport,&repeat);QCOMPARE(viewport.coordinateSpace(mode),CoordinateSpace::Global);
        }
        QTest::keyClick(&viewport,Qt::Key_Escape); QCOMPARE(viewport.transformMode(),TransformMode::None);
        QTest::keyClick(&viewport,Qt::Key_W);QTest::keyClick(&viewport,Qt::Key_E);QCOMPARE(viewport.transformMode(),TransformMode::None);
        for (auto mode : {TransformMode::Move,TransformMode::Rotate,TransformMode::Scale}) {
            viewport.setTransformMode(mode); const Transform before = viewport.transform();
            const Camera camera = viewport.camera();
            drag(viewport,{25,150},{55,170});
            QVERIFY(viewport.camera().yaw!=camera.yaw); QCOMPARE(viewport.transformMode(),mode);
            QCOMPARE(viewport.transform().position,before.position); QCOMPARE(viewport.transform().rotation,before.rotation);
            QCOMPARE(viewport.transform().scale,before.scale);
            const auto target = viewport.camera().target;
            drag(viewport,{25,150},{45,160},Qt::RightButton); QVERIFY(viewport.camera().target!=target);
            const float distance = viewport.camera().distance;
            QWheelEvent wheel(QPointF(25,150),QPointF(viewport.mapToGlobal(QPoint(25,150))),QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
            QApplication::sendEvent(&viewport,&wheel); QVERIFY(viewport.camera().distance<distance);
            QCOMPARE(viewport.transform().position,before.position); QCOMPARE(viewport.transform().rotation,before.rotation);
            QCOMPARE(viewport.transform().scale,before.scale);
        }
    }
    void viewCubeAndNumpadViews() {
        Viewport viewport; viewport.resize(600,500); viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        auto *cube = viewport.findChild<ViewCube *>(); QVERIFY(cube);
        const ViewPreset presets[] = {ViewPreset::Front,ViewPreset::Back,ViewPreset::Left,ViewPreset::Right,ViewPreset::Top,ViewPreset::Bottom};
        // Aim a free camera at each face, then click the cube centre: that face is the one under it.
        const float aims[][2] = {{0,0},{180,0},{-90,0},{90,0},{0,80},{0,-80}};
        const QPoint centre(cube->width()/2,45);
        for (int i=0; i<6; ++i) {
            Camera camera = viewport.camera(); camera.preset = ViewPreset::Free; camera.orthographic = false;
            camera.yaw = aims[i][0]; camera.pitch = aims[i][1]; viewport.setCamera(camera);
            QTest::mouseClick(cube,Qt::LeftButton,Qt::NoModifier,centre);
            QCOMPARE(viewport.camera().preset,presets[i]); QVERIFY(viewport.camera().orthographic);
            const auto view = viewport.camera().viewMatrix();
            for (int row=0; row<4; ++row) for (int col=0; col<4; ++col) QVERIFY(std::isfinite(view(row,col)));
        }
        QTest::keyClick(&viewport,Qt::Key_1,Qt::KeypadModifier); QCOMPARE(viewport.camera().preset,ViewPreset::Front);
        QTest::keyClick(&viewport,Qt::Key_End,Qt::KeypadModifier); QCOMPARE(viewport.camera().preset,ViewPreset::Front);
        QTest::keyClick(&viewport,Qt::Key_3,Qt::ControlModifier | Qt::KeypadModifier); QCOMPARE(viewport.camera().preset,ViewPreset::Left);
        QTest::keyClick(&viewport,Qt::Key_7,Qt::KeypadModifier); QCOMPARE(viewport.camera().preset,ViewPreset::Top);
        QTest::keyClick(&viewport,Qt::Key_9,Qt::KeypadModifier); QCOMPARE(viewport.camera().preset,ViewPreset::Bottom);
        QTest::keyClick(&viewport,Qt::Key_5,Qt::KeypadModifier); QVERIFY(!viewport.camera().orthographic);
        QTest::keyClick(&viewport,Qt::Key_5,Qt::KeypadModifier); QVERIFY(viewport.camera().orthographic);
        drag(viewport,{20,220},{50,240});
        QCOMPARE(viewport.camera().preset,ViewPreset::Free); QVERIFY(!viewport.camera().orthographic);
        QTest::keyClick(&viewport,Qt::Key_5,Qt::KeypadModifier); QVERIFY(!viewport.camera().orthographic);
        viewport.setViewPreset(ViewPreset::Front);
        QTest::keyClick(&viewport,Qt::Key_6,Qt::KeypadModifier); QCOMPARE(viewport.camera().preset,ViewPreset::Free); QVERIFY(!viewport.camera().orthographic);
        QVERIFY(!viewport.handleViewKey(Qt::Key_1,Qt::NoModifier));
    }
    void walkingSlidesTheFloorInASeamlessLoop() {
        Viewport viewport; viewport.resize(600,500); viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        auto grab = [&](bool walking, double distance) { viewport.setFloorScroll(walking, distance); return viewport.grabFramebuffer(); };
        const auto still = grab(false, 0), start = grab(true, 0);
        QVERIFY(start != still); // the quarter-metre grid appears
        QCOMPARE(grab(true, 5), start); // one major period later the floor looks the same
        QVERIFY(grab(true, 1.3) != start);
        QCOMPARE(grab(false, 2), still); // and it goes away again
    }
    void backgroundToggleIsDisplayOnly() {
        Viewport viewport; viewport.resize(600,500); viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        auto *cube = viewport.findChild<ViewCube *>(); QVERIFY(cube); QVERIFY(!viewport.lightBackground());
        auto corner = [&] { return viewport.grabFramebuffer().pixelColor(4,viewport.height()/2).lightness(); };
        QVERIFY(corner()<60);
        QTest::mouseClick(cube,Qt::LeftButton,Qt::NoModifier,{120,100}); QVERIFY(viewport.lightBackground()); QVERIFY(corner()>150);
        QTest::mouseClick(cube,Qt::LeftButton,Qt::NoModifier,{40,100}); QVERIFY(!viewport.lightBackground()); QVERIFY(corner()<60);
    }
    void viewCubeHoverClearsWhenOrbitingAway() {
        Viewport viewport; viewport.resize(600,500); viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        auto *cube = viewport.findChild<ViewCube *>(); QVERIFY(cube);
        // Sample the front face below its label.
        const QPoint face(cube->width()/2,60);
        auto background = [&] { const auto image = cube->grab().toImage(); const double dpr = cube->devicePixelRatioF(); return image.pixelColor(qRound(face.x()*dpr),qRound(face.y()*dpr)); };
        viewport.setViewPreset(ViewPreset::Front);
        QTest::mouseMove(cube,face); auto selected = background(); QVERIFY(selected.green()>selected.red()+20);
        QEvent leave(QEvent::Leave); QApplication::sendEvent(cube,&leave);
        drag(viewport,{20,220},{50,240}); QCOMPARE(viewport.camera().preset,ViewPreset::Free);
        auto free = background(); QCOMPARE(free,EditorTheme::panelBorder());
        QMouseEvent hover(QEvent::MouseMove,QPointF(face),QPointF(cube->mapToGlobal(face)),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(cube,&hover); auto hovered = background(); QVERIFY(hovered.green()>hovered.red());
        QCOMPARE(viewport.camera().preset,ViewPreset::Free); QApplication::sendEvent(cube,&leave);
        free = background(); QCOMPARE(free,EditorTheme::panelBorder());
    }
    void localMoveAndCropTargetIsolation() {
        Viewport viewport; viewport.resize(500,500); viewport.setGrid(false); viewport.setFrame(std::make_shared<RenderFrame>());
        Camera camera; camera.yaw = 30; camera.pitch = 20; viewport.setCamera(camera);
        viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        Transform rotated; rotated.rotation.setZ(90); viewport.setTransform(rotated); viewport.setTransformMode(TransformMode::Move);
        QCOMPARE(viewport.coordinateSpace(TransformMode::Move),CoordinateSpace::Global);
        QPoint globalHandle = colouredHandle(viewport,0);
        QPoint globalOutward = globalHandle-QPoint(250,250); globalOutward = QPoint(qRound(globalOutward.x()*0.4),qRound(globalOutward.y()*0.4));
        drag(viewport,globalHandle,globalHandle+globalOutward);
        QVERIFY(viewport.transform().position.x()>0.01f); QVERIFY(std::abs(viewport.transform().position.y())<1e-5f);
        viewport.setTransform(rotated);
        viewport.setCoordinateSpace(TransformMode::Move,CoordinateSpace::Local);
        QPoint handle = colouredHandle(viewport,0); QVERIFY(!handle.isNull());
        QPoint outward = handle-QPoint(250,250); outward = QPoint(qRound(outward.x()*0.4),qRound(outward.y()*0.4));
        drag(viewport,handle,handle+outward);
        QVERIFY(viewport.transform().position.y()>0.01f); QVERIFY(std::abs(viewport.transform().position.x())<1e-5f);
        viewport.setTransform({}); CropVolume crop; crop.enabled = true; crop.radius = crop.radiusZ = 0.4f; crop.height = 1;
        viewport.setCrop(crop); viewport.setCropEditing(true);
        qRegisterMetaType<CropVolume>(); QSignalSpy edited(&viewport,&Viewport::cropEdited);
        handle = colouredHandle(viewport,0); outward = handle-QPoint(250,250); outward = QPoint(qRound(outward.x()*0.4),qRound(outward.y()*0.4));
        drag(viewport,handle,handle+outward);
        QVERIFY(viewport.crop().transform.position.length()>0.01f); QCOMPARE(viewport.transform().position,QVector3D()); QVERIFY(!edited.isEmpty());
        viewport.setCropEditing(false); QCOMPARE(viewport.crop().enabled,true);
    }
    void cropEditingIsLocalAndScalingResizes() {
        Viewport viewport; viewport.resize(500,500); viewport.setGrid(false);
        Camera camera; camera.yaw = 30; camera.pitch = 20; camera.distance = 4; viewport.setCamera(camera);
        Transform capture; capture.position = {0.5f,0.3f,-0.2f}; capture.rotation = {15,20,25}; capture.scale = {1.2f,0.9f,1.4f};
        viewport.setTransform(capture);
        auto frame = std::make_shared<RenderFrame>(); frame->records.push_back(Splat{}); viewport.setFrame(frame);
        CropVolume crop; crop.enabled = true; crop.radius = 0.5f; crop.radiusZ = 0.8f; crop.height = 1.5f;
        viewport.setCrop(crop); viewport.setCropEditing(true);
        viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        // Scale is always local while editing a crop, whatever the scene chose; Rotate keeps it.
        viewport.setCoordinateSpace(TransformMode::Scale,CoordinateSpace::Global);
        QCOMPARE(viewport.coordinateSpace(TransformMode::Scale),CoordinateSpace::Local);
        QCOMPARE(viewport.chosenCoordinateSpace(TransformMode::Scale),CoordinateSpace::Global);
        QCOMPARE(viewport.coordinateSpace(TransformMode::Rotate),CoordinateSpace::Global);
        viewport.setDisplayedComponent(1,0,36);
        QVERIFY(std::abs(viewport.crop().transform.rotation.x()-36)<0.001f);
        QCOMPARE(viewport.crop().transform.position,crop.transform.position); // the base stays put
        viewport.setDisplayedComponent(1,0,0);
        // Scaling with the gizmo resizes the volume and leaves its scale at 1.
        viewport.setTransformMode(TransformMode::Scale);
        drag(viewport,{250,250},{285,220});
        const auto resized = viewport.crop();
        QVERIFY(resized.radius>crop.radius*1.5f);
        QVERIFY(std::abs(resized.radiusZ/resized.radius-crop.radiusZ/crop.radius)<1e-4f);
        QVERIFY(std::abs(resized.height/resized.radius-crop.height/crop.radius)<1e-4f);
        QCOMPARE(resized.transform.scale,QVector3D(1,1,1));
        QCOMPARE(viewport.transform().position,capture.position); QCOMPARE(viewport.transform().scale,capture.scale);
    }
    void movingCaptureLeavesCropFixedAndChangesWorldMembership() {
        Viewport viewport; viewport.resize(600,400); viewport.setGrid(false); viewport.setPointSize(10);
        Camera camera; camera.yaw = camera.pitch = 0; viewport.setCamera(camera);
        auto frame = std::make_shared<RenderFrame>(); frame->points = {{{0,0.3f,0},{1,0,0},0}}; viewport.setFrame(frame);
        CropVolume crop; crop.enabled = true; crop.radius = crop.radiusZ = 0.4f; crop.height = 1; viewport.setCrop(crop);
        viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        auto redPixels = [&] { const auto image = viewport.grabFramebuffer(); int count = 0;
            for (int y=0; y<image.height(); ++y) for (int x=0; x<image.width(); ++x) { const auto c = image.pixelColor(x,y); if (c.red()>240 && c.green()<10 && c.blue()<10) ++count; } return count;
        };
        // Reddish: the red point itself, or what editing tints red because a crop removes it.
        auto reddish = [&] { const auto image = viewport.grabFramebuffer(); int count = 0;
            for (int y=0; y<image.height(); ++y) for (int x=0; x<image.width(); ++x) { const auto c = image.pixelColor(x,y); if (c.red()>220 && c.green()<60 && c.blue()<80) ++count; } return count;
        };
        QVERIFY(redPixels()>30); Transform capture; capture.position = {1,0,0}; viewport.setTransform(capture);
        QCOMPARE(viewport.crop().transform.position,crop.transform.position); QCOMPARE(reddish(),0);
        // Editing shows what the crops would remove, in red, by default.
        viewport.setCropEditing(true); QVERIFY(reddish()>30); QCOMPARE(redPixels(),0); QCOMPARE(viewport.transform().position,capture.position);
    }
    void cropClipsOnlyWhenNotEditing() {
        Viewport viewport; viewport.resize(600,400); viewport.setGrid(false); viewport.setPointSize(10);
        Camera camera; camera.yaw = camera.pitch = 0; viewport.setCamera(camera);
        auto frame = std::make_shared<RenderFrame>(); frame->total = 2;
        frame->points = {{{0,0,0},{0,1,0},0},{{0.9f,0,0},{1,0,0},1}}; viewport.setFrame(frame);
        CropVolume crop; crop.enabled = true; crop.radius = crop.radiusZ = 0.4f; crop.height = 1;
        viewport.setCrop(crop); viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        auto countRed = [&] {
            const QImage image = viewport.grabFramebuffer(); int red = 0;
            for (int y=0; y<image.height(); ++y) for (int x=0; x<image.width(); ++x) {
                const auto c = image.pixelColor(x,y); if (c.red()>240 && c.green()<10 && c.blue()<10) ++red;
            }
            return red;
        };
        auto reddish = [&] { const auto image = viewport.grabFramebuffer(); int count = 0;
            for (int y=0; y<image.height(); ++y) for (int x=0; x<image.width(); ++x) { const auto c = image.pixelColor(x,y); if (c.red()>220 && c.green()<60 && c.blue()<80) ++count; } return count;
        };
        // Outside editing what a crop removes is hidden; while editing it is shown in red, or
        // hidden when the crop asks for that. The normal view never changes with it.
        QCOMPARE(reddish(),0); viewport.setCropEditing(true); QVERIFY(reddish()>30);
        crop.showRemovedInRed = false; viewport.setCrop(crop); QCOMPARE(reddish(),0);
        viewport.setCropEditing(false); QCOMPARE(reddish(),0);
        crop.showRemovedInRed = true; viewport.setCrop(crop); QCOMPARE(reddish(),0);
        crop.enabled = false; viewport.setCrop(crop); QVERIFY(countRed()>30);
        QCOMPARE(frame->points.size(),size_t(2)); // Crop preview never removes source points.
    }
    void boxAndCylinderUseTheirOwnGpuBoundary() {
        Viewport viewport; viewport.resize(600,400); viewport.setGrid(false); viewport.setPointSize(10);
        Camera camera; camera.yaw = camera.pitch = 0; viewport.setCamera(camera);
        auto frame = std::make_shared<RenderFrame>(); frame->points = {{{0.35f,0,0.35f},{1,0,0},0}}; viewport.setFrame(frame);
        CropVolume crop; crop.enabled = true; crop.radius = crop.radiusZ = 0.4f; crop.height = 1; crop.width = crop.depth = 0.8f;
        viewport.setCrop(crop); viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        auto redPixels = [&] { const auto image = viewport.grabFramebuffer(); int count = 0;
            for (int y=0; y<image.height(); ++y) for (int x=0; x<image.width(); ++x) { const auto c = image.pixelColor(x,y); if (c.red()>240 && c.green()<10 && c.blue()<10) ++count; } return count;
        };
        QCOMPARE(redPixels(),0); crop.shape = CropShape::Box; viewport.setCrop(crop); QVERIFY(redPixels()>30);
        // While editing, the colour code lightens what is kept half way to white.
        auto lightened = [&] { const auto image = viewport.grabFramebuffer(); int count = 0;
            for (int y=0; y<image.height(); ++y) for (int x=0; x<image.width(); ++x) { const auto c = image.pixelColor(x,y); if (c.red()>240 && c.green()>100 && c.green()<160 && std::abs(c.green()-c.blue())<12) ++count; } return count; };
        viewport.setCropEditing(true); QVERIFY(lightened()>30); QCOMPARE(redPixels(),0); QCOMPARE(frame->points.size(),size_t(1));
    }
    void movePlaneHandlesMoveAlongTwoAxes() {
        Viewport viewport; viewport.resize(500,500); viewport.setGrid(false);
        Camera camera; camera.yaw = 30; camera.pitch = 35; camera.distance = 4; viewport.setCamera(camera);
        auto frame = std::make_shared<RenderFrame>(); frame->records.push_back(Splat{});
        viewport.setFrame(frame); viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        viewport.setTransformMode(TransformMode::Move);
        // The square normal to Y is drawn in the Y arrow's green at 70%: find its middle.
        const QImage image = viewport.grabFramebuffer(); const double dpr = viewport.devicePixelRatioF();
        double sx = 0, sy = 0; int n = 0;
        for (int y=0; y<image.height(); ++y) for (int x=0; x<image.width(); ++x) {
            const auto c = image.pixelColor(x,y);
            if (std::abs(c.red()-54)<6 && std::abs(c.green()-161)<6 && std::abs(c.blue()-54)<6) { sx += x; sy += y; ++n; }
        }
        QVERIFY(n>20);
        const QPoint square(qRound(sx/n/dpr),qRound(sy/n/dpr));
        drag(viewport,square,square+QPoint(40,25));
        const auto p = viewport.transform().position;
        QVERIFY(std::abs(p.x())>0.01f); QVERIFY(std::abs(p.z())>0.01f); QCOMPARE(p.y(),0.0f);
    }
    void gizmoChangesTransformsAndEscapeCancelsDrag() {
        qRegisterMetaType<Transform>();
        Viewport viewport; viewport.resize(500,500); viewport.setGrid(false);
        Camera camera; camera.yaw = 30; camera.pitch = 20; camera.distance = 4; viewport.setCamera(camera);
        auto frame = std::make_shared<RenderFrame>();
        Splat source{}; source.scale[0] = 0.123f; frame->records.push_back(source);
        viewport.setFrame(frame); viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        QSignalSpy edited(&viewport,&Viewport::transformEdited);
        viewport.setTransformMode(TransformMode::Move);
        QPoint handle = colouredHandle(viewport,0); QVERIFY(!handle.isNull());
        QPoint outward = handle-QPoint(250,250); outward = QPoint(qRound(outward.x()*0.4),qRound(outward.y()*0.4));
        drag(viewport,handle,handle+outward);
        QVERIFY(viewport.transform().position.x()>0.01f); QCOMPARE(viewport.transform().position.y(),0.0f);
        QCOMPARE(viewport.transform().position.z(),0.0f); QVERIFY(!edited.isEmpty());
        QCOMPARE(viewport.camera().yaw,camera.yaw); QCOMPARE(viewport.camera().pitch,camera.pitch);

        viewport.setTransform({}); viewport.setTransformMode(TransformMode::Rotate);
        handle = colouredHandle(viewport,2); QVERIFY(!handle.isNull());
        const QPointF offset = handle-QPoint(250,250); const double angle = 0.45;
        const QPoint end = QPoint(250,250)+QPoint(qRound(offset.x()*std::cos(angle)-offset.y()*std::sin(angle)),qRound(offset.x()*std::sin(angle)+offset.y()*std::cos(angle)));
        drag(viewport,handle,end); QVERIFY(viewport.transform().rotation.length()>5);
        QCOMPARE(viewport.transform().position,QVector3D()); QCOMPARE(viewport.transform().scale,QVector3D(1,1,1));

        viewport.setTransform({}); viewport.setTransformMode(TransformMode::Scale);
        drag(viewport,{250,250},{285,220});
        QVERIFY(viewport.transform().scale.x()>1.5f);
        QCOMPARE(viewport.transform().scale.x(),viewport.transform().scale.y());
        QCOMPARE(viewport.transform().scale.x(),viewport.transform().scale.z());
        QCOMPARE(frame->records[0].scale[0],0.123f); // Scene manipulation preserves source Gaussian data.

        viewport.setTransform({}); viewport.setTransformMode(TransformMode::Move);
        handle = colouredHandle(viewport,0); QTest::mousePress(&viewport,Qt::LeftButton,Qt::NoModifier,handle);
        QMouseEvent move(QEvent::MouseMove,QPointF(handle+outward),QPointF(viewport.mapToGlobal(handle+outward)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&viewport,&move); QVERIFY(viewport.transform().position.length()>0.01f);
        QTest::keyClick(&viewport,Qt::Key_Escape);
        QCOMPARE(viewport.transformMode(),TransformMode::None); QCOMPARE(viewport.transform().position,QVector3D());
        QTest::mouseRelease(&viewport,Qt::LeftButton,Qt::NoModifier,handle+outward);
    }
    void splatsFollowTheirCovarianceAndOpacity() {
        Viewport viewport; viewport.resize(500,500); viewport.setGrid(false);
        Camera camera; camera.target = {0,0,0}; camera.yaw = camera.pitch = 0; camera.distance = 3; viewport.setCamera(camera);
        auto make = [&](float qz,float qw,float opacity) {
            auto frame = std::make_shared<RenderFrame>(); frame->total = 1;
            Splat s{{0,0,0},{0,0,qz,qw},{0.3f,0.02f,0.02f},{1,0,0,opacity},0};
            frame->records = {s}; frame->active = {1}; frame->points = {{{0,0,0},{1,0,0},0}}; return frame; };
        // Width and height of the reddish footprint, and its brightest red.
        auto footprint = [&] { const auto image = viewport.grabFramebuffer(); int left=image.width(),right=-1,top=image.height(),bottom=-1,peak=0;
            for (int y=0;y<image.height();++y) for (int x=0;x<image.width();++x) { const auto c = image.pixelColor(x,y);
                if (c.red()>c.green()+40) {left=std::min(left,x);right=std::max(right,x);top=std::min(top,y);bottom=std::max(bottom,y);peak=std::max(peak,c.red());} }
            return std::array<int,3>{right-left+1,bottom-top+1,peak}; };
        viewport.setFrame(make(0,1,1)); viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        const auto point = footprint(); QVERIFY(point[0]<20 && point[1]<20);
        viewport.setSplatRendering(true); QVERIFY(viewport.splatRendering());
        const auto along = footprint(); QVERIFY2(along[0]>8*along[1], qPrintable(QString("%1x%2").arg(along[0]).arg(along[1]))); QVERIFY(along[2]>200);
        // A quarter turn about Z stands it up; half the opacity halves its strength.
        viewport.setFrame(make(std::sqrt(0.5f),std::sqrt(0.5f),1)); const auto upright = footprint(); QVERIFY(upright[1]>8*upright[0]);
        viewport.setFrame(make(0,1,0.5f)); const auto faint = footprint(); QVERIFY(faint[2]<along[2]-60);
        viewport.setSplatRendering(false); const auto back = footprint(); QVERIFY(back[0]<20 && back[1]<20);
    }
    void needleSplatsFadeWithTheirFootprint() {
        // A red needle far thinner than a pixel, on black: with the anti-aliasing compensation
        // it is a faint trace, not the solid line it would be at full opacity.
        Viewport viewport; viewport.resize(400,400); viewport.setGrid(false);
        Camera camera; camera.target = {0,0,0}; camera.yaw = camera.pitch = 0; camera.distance = 3; viewport.setCamera(camera);
        auto frame = std::make_shared<RenderFrame>(); frame->total = 1;
        frame->records = {Splat{{0,0,0},{0,0,0,1},{0.0001f,0.4f,0.0001f},{1,0,0,1},0}}; frame->active = {1}; frame->points = {{{0,0,0},{1,0,0},0}};
        viewport.setFrame(frame); viewport.setSplatRendering(true); viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        auto peak = [&] { const auto image = viewport.grabFramebuffer(); int best = 0;
            for (int y=0;y<image.height();++y) for (int x=0;x<image.width();++x) best = std::max(best,image.pixelColor(x,y).red()-image.pixelColor(x,y).green());
            return best; };
        // A capture without the hint is plain 3DGS: the needle is drawn at full strength.
        QVERIFY(!viewport.splatAntialiasing()); const int plain = peak(); QVERIFY2(plain>150, qPrintable(QString::number(plain)));
        viewport.setSplatAntialiasing(true); const int faint = peak(); QVERIFY2(faint>0 && faint<80, qPrintable(QString::number(faint)));
        // Baked for plain renderers, the needle fades by itself and the compensation is off.
        Modifier bake; bake.id = Project::newId(); bake.type = ModifierType::BakeAntialiasing; bake.bakeDistance = 3; bake.bakeScreenHeight = 400;
        viewport.setModifiers({bake}); const int baked = peak(); QVERIFY2(baked>0 && baked<80, qPrintable(QString::number(baked)));
        viewport.setSplatAntialiasing(false); QCOMPARE(peak(), baked);
    }
    void splatOrderFollowsTheCameraFromTheSortingThread() {
        // Red in front of blue seen from one side; from the other, blue must end up in front once
        // the background sort lands. Every paint in between draws a valid (older) order.
        auto viewport = std::make_unique<Viewport>(); viewport->resize(300,300); viewport->setGrid(false);
        Camera camera; camera.target = {0,0,0}; camera.yaw = camera.pitch = 0; camera.distance = 4; viewport->setCamera(camera);
        // Along the line of sight: red nearer the camera, blue behind it.
        const auto v = camera.viewMatrix(); const QVector3D near = QVector3D(v(2,0),v(2,1),v(2,2)).normalized()*0.3f;
        auto frame = std::make_shared<RenderFrame>(); frame->total = 2;
        const Splat red{{near.x(),near.y(),near.z()},{0,0,0,1},{0.2f,0.2f,0.2f},{1,0,0,0.95f},0}, blue{{-near.x(),-near.y(),-near.z()},{0,0,0,1},{0.2f,0.2f,0.2f},{0,0,1,0.95f},1};
        frame->records = {red,blue}; frame->active = {1,1};
        frame->points = {{{red.position[0],red.position[1],red.position[2]},{1,0,0},0},{{blue.position[0],blue.position[1],blue.position[2]},{0,0,1},1}};
        viewport->setSplatRendering(true); viewport->setFrame(frame); viewport->show(); QVERIFY(QTest::qWaitForWindowExposed(viewport.get()));
        auto centre = [&] { const auto image = viewport->grabFramebuffer(); return image.pixelColor(image.width()/2,image.height()/2); };
        const auto first = centre(); const bool redFirst = first.red()>first.blue(); QVERIFY(redFirst);
        QVERIFY(std::abs(first.red()-first.blue())>100);
        camera.yaw = 180; viewport->setCamera(camera);
        QTRY_VERIFY_WITH_TIMEOUT([&] { const auto c = centre(); return (c.red()>c.blue())!=redFirst && std::abs(c.red()-c.blue())>100; }(), 3000);
        // Destroying it with a sort just requested must neither crash nor hang.
        camera.yaw = 90; viewport->setCamera(camera); centre(); viewport.reset();
    }
    void opaquePointsAndFixedSize() {
        Viewport viewport; viewport.resize(400,400); viewport.setGrid(false); viewport.setPointSize(10);
        Camera camera; camera.yaw = camera.pitch = 0; camera.distance = 4; viewport.setCamera(camera);
        auto frame = std::make_shared<RenderFrame>(); frame->total = 2;
        Splat transparent{}; transparent.position[0] = -0.4f; transparent.color[0] = 1;
        transparent.color[3] = 0; transparent.scale[0] = 1000; transparent.scale[1] = 0.01f; transparent.scale[2] = 1;
        Splat small{}; small.position[0] = 0.4f; small.color[1] = small.color[3] = 1;
        small.scale[0] = small.scale[1] = small.scale[2] = 0.0001f; small.id = 1;
        frame->records = {transparent,small}; frame->active = {1,1};
        frame->points = {{{-0.4f,0,0},{1,0,0},0},{{0.4f,0,0},{0,1,0},1}};
        viewport.setFrame(frame); viewport.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        QTest::qWait(100);
        QVERIFY2(viewport.renderError().isEmpty(),qPrintable(viewport.renderError()));
        const QImage image = viewport.grabFramebuffer(); QVERIFY(!image.isNull());
        int red = 0, green = 0;
        for (int y=0; y<image.height(); ++y) for (int x=0; x<image.width(); ++x) {
            const auto pixel = image.pixelColor(x,y);
            if (pixel.red()>240 && pixel.green()<10 && pixel.blue()<10) ++red;
            if (pixel.green()>240 && pixel.red()<10 && pixel.blue()<10) ++green;
        }
        QVERIFY2(red>30,"A source record with zero opacity must still draw an opaque point.");
        QVERIFY(green>30); QVERIFY(std::abs(red-green)<=10); // Equal footprint despite different Gaussian scales.
        QCOMPARE(frame->records[0].color[3],0.0f); QCOMPARE(frame->records[0].scale[0],1000.0f);
    }
};
int main(int argc, char **argv) {
    QSurfaceFormat format; format.setVersion(3,3); format.setProfile(QSurfaceFormat::CoreProfile); format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format); QApplication app(argc,argv);
    ViewportTests test; return QTest::qExec(&test,argc,argv);
}
#include "viewport_tests.moc"
