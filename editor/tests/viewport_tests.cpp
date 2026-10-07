#include "viewport.h"
#include "viewcube.h"
#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QSurfaceFormat>
#include <QtTest>
#include <cmath>

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
            if (x<image.width()/2 && c.red()>35 && std::abs(c.red()-c.green())<2 && std::abs(c.red()-c.blue())<2) {++ghostPixels;brightest=std::max(brightest,c.red());}}
        QVERIFY(ghostPixels>10);QVERIFY(bluePixels>10);QVERIFY2(brightest<110,"Overlapping ghost points must remain translucent rather than accumulating opacity.");QCOMPARE(viewport.ghostTime(),.25);
        QVERIFY(viewport.focusVisible());QVERIFY(std::abs(viewport.camera().target.x()-.05f)<1e-5f);
        QVERIFY(!viewport.setGhost(false));QCOMPARE(viewport.ghostPointCount(),size_t(0));QVERIFY(viewport.focusVisible());QVERIFY(std::abs(viewport.camera().target.x()-.8f)<1e-5f);
        const auto disabled=viewport.grabFramebuffer();int grey=0;for (int y=int(disabled.height()*.2);y<int(disabled.height()*.8);++y) for (int x=int(disabled.width()*.1);x<int(disabled.width()*.48);++x) {const auto c=disabled.pixelColor(x,y);if (c.red()>35 && std::abs(c.red()-c.green())<2 && std::abs(c.red()-c.blue())<2) ++grey;}QCOMPARE(grey,0);
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
        for (int i=0; i<6; ++i) {
            QTest::mouseClick(cube,Qt::LeftButton,Qt::NoModifier,{20+(i%2)*73,97+(i/2)*23});
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
    void viewCubeHoverClearsWhenOrbitingAway() {
        Viewport viewport; viewport.resize(600,500); viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        auto *cube = viewport.findChild<ViewCube *>(); QVERIFY(cube);
        auto background = [&] { const auto image = cube->grab().toImage(); const double dpr = cube->devicePixelRatioF(); return image.pixelColor(qRound(14*dpr),qRound(96*dpr)); };
        viewport.setViewPreset(ViewPreset::Front);
        QTest::mouseMove(cube,{20,97}); auto selected = background(); QVERIFY(selected.green()>selected.red());
        QEvent leave(QEvent::Leave); QApplication::sendEvent(cube,&leave);
        drag(viewport,{20,220},{50,240}); QCOMPARE(viewport.camera().preset,ViewPreset::Free);
        auto free = background(); QCOMPARE(free.green(),free.red());
        QMouseEvent hover(QEvent::MouseMove,QPointF(20,97),QPointF(cube->mapToGlobal(QPoint(20,97))),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(cube,&hover); auto hovered = background(); QVERIFY(hovered.green()>hovered.red());
        QCOMPARE(viewport.camera().preset,ViewPreset::Free); QApplication::sendEvent(cube,&leave);
        free = background(); QCOMPARE(free.green(),free.red());
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
        viewport.setTransform({}); CropVolume crop; crop.enabled = true; crop.radius = 0.4f; crop.height = 1;
        viewport.setCrop(crop); viewport.setCropEditing(true);
        qRegisterMetaType<CropVolume>(); QSignalSpy edited(&viewport,&Viewport::cropEdited);
        handle = colouredHandle(viewport,0); outward = handle-QPoint(250,250); outward = QPoint(qRound(outward.x()*0.4),qRound(outward.y()*0.4));
        drag(viewport,handle,handle+outward);
        QVERIFY(viewport.crop().transform.position.length()>0.01f); QCOMPARE(viewport.transform().position,QVector3D()); QVERIFY(!edited.isEmpty());
        viewport.setCropEditing(false); QCOMPARE(viewport.crop().enabled,true);
    }
    void globalScalingAndRotationKeepCylinderBaseFixed() {
        Viewport viewport;
        Transform capture; capture.position = {0.5f,0.3f,-0.2f}; capture.rotation = {15,20,25}; capture.scale = {1.2f,0.9f,1.4f};
        viewport.setTransform(capture);
        CropVolume crop; crop.enabled = true; crop.height = 2.5f;
        crop.transform.position = {0.2f,0.1f,-0.1f}; crop.transform.rotation = {20,30,40};
        viewport.setCrop(crop); viewport.setCropEditing(true);
        const auto before = crop.transform.matrix(); const auto base = before.map(QVector3D());
        QMatrix4x4 delta; delta.translate(base); delta.scale(1.8f,1,1); delta.translate(-base);
        const auto expected = delta*before;
        viewport.setDisplayedComponent(2,0,viewport.displayedTransform().scale.x()*1.8f);
        const auto actual = viewport.crop().transform.matrix();
        for (int i=0; i<4; ++i) for (int j=0; j<4; ++j) QVERIFY(std::abs(actual(i,j)-expected(i,j))<1e-4f);
        QVERIFY((actual.map(QVector3D())-base).length()<1e-5f);
        viewport.setDisplayedComponent(1,0,36);
        QVERIFY(std::abs(viewport.displayedTransform().rotation.x()-36)<0.001f);
        QVERIFY((viewport.crop().transform.matrix().map(QVector3D())-base).length()<1e-5f);
        viewport.setCoordinateSpace(TransformMode::Scale,CoordinateSpace::Local);
        const auto local = viewport.crop().transform.scale;
        viewport.setDisplayedComponent(2,1,local.y()*1.6f);
        QVERIFY(std::abs(viewport.crop().transform.scale.y()-local.y()*1.6f)<1e-5f);
        QCOMPARE(viewport.crop().transform.position,crop.transform.position);
        QCOMPARE(viewport.transform().position,capture.position); QCOMPARE(viewport.transform().scale,capture.scale);
    }
    void movingCaptureLeavesCropFixedAndChangesWorldMembership() {
        Viewport viewport; viewport.resize(600,400); viewport.setGrid(false); viewport.setPointSize(10);
        Camera camera; camera.yaw = camera.pitch = 0; viewport.setCamera(camera);
        auto frame = std::make_shared<RenderFrame>(); frame->points = {{{0,0.3f,0},{1,0,0},0}}; viewport.setFrame(frame);
        CropVolume crop; crop.enabled = true; crop.radius = 0.4f; crop.height = 1; viewport.setCrop(crop);
        viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        auto redPixels = [&] { const auto image = viewport.grabFramebuffer(); int count = 0;
            for (int y=0; y<image.height(); ++y) for (int x=0; x<image.width(); ++x) { const auto c = image.pixelColor(x,y); if (c.red()>240 && c.green()<10 && c.blue()<10) ++count; } return count;
        };
        QVERIFY(redPixels()>30); Transform capture; capture.position = {1,0,0}; viewport.setTransform(capture);
        QCOMPARE(viewport.crop().transform.position,crop.transform.position); QCOMPARE(redPixels(),0);
        viewport.setCropEditing(true); QVERIFY(redPixels()>30); QCOMPARE(viewport.transform().position,capture.position);
    }
    void cropClipsOnlyWhenNotEditing() {
        Viewport viewport; viewport.resize(600,400); viewport.setGrid(false); viewport.setPointSize(10);
        Camera camera; camera.yaw = camera.pitch = 0; viewport.setCamera(camera);
        auto frame = std::make_shared<RenderFrame>(); frame->total = 2;
        frame->points = {{{0,0,0},{0,1,0},0},{{0.9f,0,0},{1,0,0},1}}; viewport.setFrame(frame);
        CropVolume crop; crop.enabled = true; crop.radius = 0.4f; crop.height = 1;
        viewport.setCrop(crop); viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        auto countRed = [&] {
            const QImage image = viewport.grabFramebuffer(); int red = 0;
            for (int y=0; y<image.height(); ++y) for (int x=0; x<image.width(); ++x) {
                const auto c = image.pixelColor(x,y); if (c.red()>240 && c.green()<10 && c.blue()<10) ++red;
            }
            return red;
        };
        QCOMPARE(countRed(),0); viewport.setCropEditing(true); QVERIFY(countRed()>30);
        viewport.setCropEditing(false); QCOMPARE(countRed(),0);
        crop.enabled = false; viewport.setCrop(crop); QVERIFY(countRed()>30);
        QCOMPARE(frame->points.size(),size_t(2)); // Crop preview never removes source points.
    }
    void boxAndCylinderUseTheirOwnGpuBoundary() {
        Viewport viewport; viewport.resize(600,400); viewport.setGrid(false); viewport.setPointSize(10);
        Camera camera; camera.yaw = camera.pitch = 0; viewport.setCamera(camera);
        auto frame = std::make_shared<RenderFrame>(); frame->points = {{{0.35f,0,0.35f},{1,0,0},0}}; viewport.setFrame(frame);
        CropVolume crop; crop.enabled = true; crop.radius = 0.4f; crop.height = 1; crop.width = crop.depth = 0.8f;
        viewport.setCrop(crop); viewport.show(); QVERIFY(QTest::qWaitForWindowExposed(&viewport));
        auto redPixels = [&] { const auto image = viewport.grabFramebuffer(); int count = 0;
            for (int y=0; y<image.height(); ++y) for (int x=0; x<image.width(); ++x) { const auto c = image.pixelColor(x,y); if (c.red()>240 && c.green()<10 && c.blue()<10) ++count; } return count;
        };
        QCOMPARE(redPixels(),0); crop.shape = CropShape::Box; viewport.setCrop(crop); QVERIFY(redPixels()>30);
        viewport.setCropEditing(true); QVERIFY(redPixels()>30); QCOMPARE(frame->points.size(),size_t(1));
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
