#include "mainwindow.h"
#include "viewport.h"
#include "viewcube.h"
#include "rangeslider.h"
#include "modifierpanel.h"
#include "exportcapture.h"
#include "vgssign.h"
#include "editortheme.h"
#include "capturesettingsdialog.h"
#include <QApplication>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QFile>
#include <QComboBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QTimer>
#include <QTreeWidget>
#include <QToolButton>
#include <QMenu>
#include <QMenuBar>
#include <QGroupBox>
#include <QCheckBox>
#include <QTableWidget>
#include <QLabel>
#include <QFormLayout>
#include <QSlider>
#include <QWheelEvent>
#include <QtTest>

class MainWindowTests : public QObject {
    Q_OBJECT
private slots:
    void cropSwitchesBetweenStaticAndAnimatedKeepingKeys() {
        QTemporaryDir dir;QVERIFY(dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
        QString capture=dir.filePath("source.pgs");
        vgs::Frame f;f.count=1;f.active={1};f.position={0,1,0};f.rotation={0,0,0,1};f.scale={.01f,.02f,.03f};f.colorDc={.5f,.5f,.5f};f.opacity={1};
        vgs::Header h;h.shDegree=0;h.frameCount=h.durationTicks=5;h.chunks.resize(5);
        for (size_t i=0;i<5;++i) {h.chunks[i].startTick=i;h.chunks[i].intervals=1;}
        vgs::EncodeOptions options;options.signer=vgs::authoringSigner();options.shDegree=0;options.compression=vgs::Compression::None;
        QFile file(capture);QVERIFY(file.open(QIODevice::WriteOnly));
        vgs::encodeSequence(h,[&](size_t) {return packExportFrame(f,0);},[&](uint64_t offset,const uint8_t *data,size_t count) {
            if (!file.seek(qint64(offset)) || file.write(reinterpret_cast<const char *>(data),qint64(count))!=qint64(count)) throw std::runtime_error("Cannot create fixture.");
        },options);file.close();
        MainWindow window(nullptr,dir.filePath("presets"));window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openPath(capture);
        auto *edit=window.findChild<QPushButton *>("editCropVolume");QVERIFY(edit);QTRY_VERIFY_WITH_TIMEOUT(edit->isEnabled(),5000);
        auto *animation=window.findChild<QComboBox *>("cropAnimation");auto *keys=window.findChild<QWidget *>("cropKeys");auto *table=window.findChild<QTableWidget *>("cropKeyTable");
        auto *radius=window.findChild<QDoubleSpinBox *>("cropRadiusX");auto *slider=window.findChild<RangeSlider *>();auto *tree=window.findChild<QTreeWidget *>("modifierTree");
        QVERIFY(animation && keys && table && radius && slider && tree);
        auto choose=[&](int mode) {animation->setCurrentIndex(animation->findData(mode));QVERIFY(QMetaObject::invokeMethod(animation,"activated",Qt::DirectConnection,Q_ARG(int,animation->currentIndex())));};
        auto treeKeys=[&] {return tree->topLevelItem(0)->data(2,Qt::UserRole+4).toList().size();};
        QCOMPARE(animation->currentData().toInt(),0);QVERIFY(!keys->isVisible());QCOMPARE(treeKeys(),0);
        const double still=radius->value();
        // Animated starts from where the crop is, with a key at the current frame.
        choose(1);QVERIFY(keys->isVisible());QCOMPARE(table->rowCount(),1);QCOMPARE(treeKeys(),1);
        // Editing at another frame sets a key there; going back shows the first pose.
        emit slider->playheadChanged(4);radius->setValue(still+0.5);QCOMPARE(table->rowCount(),2);QCOMPARE(table->item(1,0)->text(),QString("4"));QCOMPARE(treeKeys(),2);
        emit slider->playheadChanged(0);QCOMPARE(radius->value(),still);
        emit slider->playheadChanged(2);QVERIFY(std::abs(radius->value()-(still+0.25))<1e-4);
        // Static hides the keys and restores the static size, at any frame; the keys survive.
        choose(0);QVERIFY(!keys->isVisible());QCOMPARE(radius->value(),still);QCOMPARE(treeKeys(),0);
        radius->setValue(still+2);emit slider->playheadChanged(4);QCOMPARE(radius->value(),still+2);
        choose(1);QVERIFY(keys->isVisible());QCOMPARE(table->rowCount(),2);QCOMPARE(radius->value(),still+0.5);
        choose(0);QCOMPARE(radius->value(),still+2);
        // The timeline's track lies exactly over the modifier bars, also after a resize.
        auto *panel=window.findChild<ModifierPanel *>();QVERIFY(panel);
        auto aligned=[&] {const auto span=panel->trackSpan();const auto track=slider->trackRect();
            return slider->mapToGlobal(track.topLeft()).x()==span.first && track.width()==span.second;};
        QTRY_VERIFY(aligned());window.resize(window.width()-120,window.height());QTRY_VERIFY(aligned());
        // Its name, Capture, starts where the modifier names do.
        QCOMPARE(slider->label(),QString("Capture"));QCOMPARE(slider->mapToGlobal(QPoint(slider->labelX(),0)).x(),panel->nameLeft());
        QVERIFY(slider->labelX()>0 && slider->labelX()<slider->trackRect().left());
        // Whole, not elided, even when every modifier name is shorter.
        QVERIFY(slider->trackRect().left()-12-slider->labelX()>=slider->fontMetrics().horizontalAdvance("Capture"));
        // Zoom and pan: one view for the timeline and the bars, never past the capture.
        QCOMPARE(slider->zoom(),1.);QCOMPARE(panel->view(),qMakePair(0.,4.));
        slider->zoomAt(10,slider->trackRect().left());QCOMPARE(slider->zoom(),2.);QCOMPARE(panel->view(),qMakePair(0.,2.));
        slider->panByPixels(-slider->trackRect().width()/2.);QCOMPARE(panel->view(),qMakePair(1.,3.));
        slider->panByPixels(-10000);QCOMPARE(panel->view(),qMakePair(2.,4.));
        // Over the bars Ctrl+wheel zooms, the plain wheel does not.
        auto wheel=[&](Qt::KeyboardModifiers modifiers,int delta) {auto *target=tree->viewport();const QPointF at=target->rect().center();
            QWheelEvent event(at,target->mapToGlobal(at),{},{0,delta},Qt::NoButton,modifiers,Qt::NoScrollPhase,false);QApplication::sendEvent(target,&event);};
        wheel(Qt::NoModifier,-120);QCOMPARE(slider->zoom(),2.);wheel(Qt::ControlModifier,-120*4);QVERIFY(slider->zoom()<2.);
        // The view pages to keep the playhead in sight; a double-click on Capture, or reopening, shows it whole.
        slider->zoomAt(10,slider->trackRect().left());QCOMPARE(panel->view(),qMakePair(0.,2.));
        emit slider->playheadChanged(1);QCOMPARE(panel->view(),qMakePair(0.,2.));emit slider->playheadChanged(4);QVERIFY(panel->view().second>=4.);
        QTest::mouseDClick(slider,Qt::LeftButton,{},QPoint(slider->labelX()+2,slider->height()/2));QCOMPARE(slider->zoom(),1.);
        slider->zoomAt(10,slider->trackRect().left());QCOMPARE(slider->zoom(),2.);
        choose(1);emit slider->playheadChanged(2);
        choose(0);
        // 1 and 2 on the main keyboard pick 3D points / Gaussian; the numpad keeps its views,
        // and a digit typed into a field stays in the field.
        auto *style=window.findChild<QComboBox *>("displayRenderStyle");auto *sh=window.findChild<QComboBox *>("displaySplatSh");auto *viewport=window.findChild<Viewport *>();
        QVERIFY(style && sh && viewport);QCOMPARE(style->itemText(0),QString("3D points"));QCOMPARE(style->itemText(1),QString("Gaussian"));
        QCOMPARE(sh->itemText(sh->count()-1),QString("All"));QCOMPARE(sh->currentText(),QString("All"));QCOMPARE(style->currentText(),QString("Gaussian"));QVERIFY(viewport->splatRendering());QCOMPARE(viewport->splatShDegree(),-1);
        sh->setCurrentIndex(sh->findData(2));QCOMPARE(viewport->splatShDegree(),2);sh->setCurrentIndex(sh->findData(-1));
        // Closed, each reads "Label: value" (also its accessible name); the open list keeps the plain choices.
        QCOMPARE(sh->accessibleName(),QString("SH: All"));QCOMPARE(sh->itemText(2),QString("SH2"));
        sh->setCurrentIndex(sh->findData(2));QCOMPARE(sh->accessibleName(),QString("SH: 2"));sh->setCurrentIndex(sh->findData(-1));
        QCOMPARE(style->accessibleName(),QString("Render: ")+style->currentText());
        viewport->setFocus();QTRY_VERIFY(viewport->hasFocus());
        QTest::keyClick(viewport,Qt::Key_2);QCOMPARE(style->currentIndex(),1);QVERIFY(viewport->splatRendering());
        QTest::keyClick(viewport,Qt::Key_1,Qt::KeypadModifier);QCOMPARE(style->currentIndex(),1);QCOMPARE(viewport->camera().preset,ViewPreset::Front);
        QTest::keyClick(viewport,Qt::Key_1);QCOMPARE(style->currentIndex(),0);QVERIFY(!viewport->splatRendering());
        radius->setFocus();QTRY_VERIFY(radius->hasFocus());radius->selectAll();QTest::keyClick(radius,Qt::Key_2);QCOMPARE(style->currentIndex(),0);
        QTest::keyClick(radius,Qt::Key_2,Qt::KeypadModifier);QCOMPARE(style->currentIndex(),0);
        // Reopening shows the timeline whole again: the zoom is not saved.
        slider->zoomAt(10,slider->trackRect().left());QCOMPARE(slider->zoom(),2.);window.setWindowModified(false);
        window.openPath(capture);QTRY_COMPARE(slider->zoom(),1.);QTRY_VERIFY(edit->isEnabled());QCOMPARE(panel->view(),qMakePair(0.,4.));
    }
    void timelineTicksFollowDurationAndZoom() {
        // A tick each second in a light green, one per frame halfway to the track's green, and
        // neither while they would sit closer than a few pixels.
        RangeSlider slider;slider.resize(640,38);slider.setTrackInsets(20,20);slider.setFrameRate(30);
        const QColor second(198,236,180),frame(131,206,102);
        auto count=[&](const QColor &colour) {
            const QImage image=slider.grab().toImage();const qreal ratio=image.devicePixelRatio();const int y=qRound(slider.trackRect().center().y()*ratio);
            int columns=0;bool inside=false;
            for (int x=0;x<image.width();++x) {const bool match=image.pixelColor(x,y)==colour;if (match && !inside) ++columns;inside=match;}
            return columns;
        };
        slider.setFrameRange(0,299);slider.setRangeValues(0,299);slider.setPlayheadValue(15); // off the ticks
        QCOMPARE(count(second),10);QCOMPARE(count(frame),0);          // 10 s: seconds only
        slider.zoomAt(4,slider.trackRect().left());QVERIFY(count(frame)>40);QVERIFY(count(second)>=2); // zoomed: frames too
        slider.setFrameRange(0,29999);QCOMPARE(count(second),0);QCOMPARE(count(frame),0);        // 1000 s: too dense for either
    }
    void tabTogglesCropFromViewportAndFields() {
        QTemporaryDir dir;QVERIFY(dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
        QString capture=dir.filePath("source.pgs");
        vgs::Frame f;f.count=1;f.active={1};f.position={0,1,0};f.rotation={0,0,0,1};f.scale={.01f,.02f,.03f};f.colorDc={.5f,.5f,.5f};f.opacity={1};
        vgs::Header h;h.shDegree=0;h.frameCount=h.durationTicks=5;h.chunks.resize(5);
        for (size_t i=0;i<5;++i) {h.chunks[i].startTick=i;h.chunks[i].intervals=1;}
        vgs::EncodeOptions options;options.signer=vgs::authoringSigner();options.shDegree=0;options.compression=vgs::Compression::None;
        QFile file(capture);QVERIFY(file.open(QIODevice::WriteOnly));
        vgs::encodeSequence(h,[&](size_t) {return packExportFrame(f,0);},[&](uint64_t offset,const uint8_t *data,size_t count) {
            if (!file.seek(qint64(offset)) || file.write(reinterpret_cast<const char *>(data),qint64(count))!=qint64(count)) throw std::runtime_error("Cannot create fixture.");
        },options);file.close();
        MainWindow window(nullptr,dir.filePath("presets"));window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *emptyTree=window.findChild<QTreeWidget *>("modifierTree");QVERIFY(emptyTree);QVERIFY(!emptyTree->isEnabled());QVERIFY(emptyTree->isColumnHidden(1));
        auto *startupDisplay=window.findChild<QWidget *>("viewportDisplayControls");QVERIFY(startupDisplay);QVERIFY(startupDisplay->isEnabled());
        const auto emptyImage=emptyTree->viewport()->grab().toImage();const auto rowRect=emptyTree->visualItemRect(emptyTree->topLevelItem(0));
        const auto inactiveColour=emptyImage.pixelColor(qRound(emptyTree->viewport()->width()*.8*emptyImage.devicePixelRatio()),qRound(rowRect.center().y()*emptyImage.devicePixelRatio()));
        QVERIFY(std::max({inactiveColour.red(),inactiveColour.green(),inactiveColour.blue()})-std::min({inactiveColour.red(),inactiveColour.green(),inactiveColour.blue()})<20);
        window.openPath(capture);
        auto *transformGroup=window.findChild<QGroupBox *>("transformProperties");QVERIFY(transformGroup);QCOMPARE(transformGroup->title(),QString::fromUtf8("Transform · Capture"));
        auto *button=window.findChild<QPushButton *>("editCropVolume");QVERIFY(button);
        QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(),5000);
        auto *modifiers=window.findChild<QTreeWidget *>("modifierTree");QVERIFY(modifiers);QCOMPARE(modifiers->topLevelItemCount(),1);QCOMPARE(modifiers->topLevelItem(0)->childCount(),0);
        QCOMPARE(modifiers->topLevelItem(0)->checkState(0),Qt::Checked);QVERIFY(modifiers->visualItemRect(modifiers->topLevelItem(0)).height()>=30);
        const QPoint firstEye(18,modifiers->visualItemRect(modifiers->topLevelItem(0)).center().y());
        QTest::mouseClick(modifiers->viewport(),Qt::LeftButton,Qt::NoModifier,firstEye);QCOMPARE(modifiers->topLevelItem(0)->checkState(0),Qt::Unchecked);
        QTest::mouseClick(modifiers->viewport(),Qt::LeftButton,Qt::NoModifier,firstEye);QCOMPARE(modifiers->topLevelItem(0)->checkState(0),Qt::Checked);
        QCOMPARE(modifiers->topLevelItem(0)->text(2),QString());
        auto *add=window.findChild<QToolButton *>("addModifier"),*remove=window.findChild<QToolButton *>("removeModifier"),*duplicate=window.findChild<QToolButton *>("duplicateModifier");QVERIFY(add);QVERIFY(remove);QVERIFY(duplicate);
        auto *newType=window.findChild<QComboBox *>("newModifierType");QVERIFY(newType);QCOMPARE(newType->findData(1),-1);newType->setCurrentIndex(newType->findData(0));QCOMPARE(newType->currentText(),QString("Crop"));add->click();
        QCOMPARE(modifiers->topLevelItem(0)->text(0),QString("Crop"));QCOMPARE(modifiers->topLevelItemCount(),2);QCOMPARE(modifiers->currentItem()->text(1),QString("Crop cylinder"));QCOMPARE(modifiers->currentItem()->text(0),QString("Crop 2"));
        auto *shape=window.findChild<QComboBox *>("cropShape");QVERIFY(shape);shape->setCurrentIndex(shape->findData(int(CropShape::Box)));
        QVERIFY(QMetaObject::invokeMethod(shape,"activated",Qt::DirectConnection,Q_ARG(int,shape->currentIndex())));QCOMPARE(modifiers->currentItem()->text(1),QString("Crop box"));
        {   // Modifier parameters scrub from their labels too: the box crop's Width.
            QLabel *widthLabel=nullptr;for (auto *label:window.findChildren<QLabel *>()) if (label->text()=="Width" && label->isVisible()) widthLabel=label;
            QVERIFY(widthLabel);QCOMPARE(widthLabel->cursor().shape(),Qt::SizeHorCursor);
            auto *form=qobject_cast<QFormLayout *>(widthLabel->parentWidget()->layout());QVERIFY(form);
            auto *width=qobject_cast<QDoubleSpinBox *>(form->itemAt(form->indexOf(widthLabel)+1)->widget());QVERIFY(width);
            const double before=width->value();const QPoint start=widthLabel->rect().center();
            QTest::mousePress(widthLabel,Qt::LeftButton,{},start);QTest::mouseMove(widthLabel,start+QPoint(20,0));QTest::mouseRelease(widthLabel,Qt::LeftButton,{},start+QPoint(20,0));
            QVERIFY(std::abs(width->value()-(before+0.1))<1e-6);
            width->setValue(before);
        }
        {   // Walk shows its speed in m/s or km/h; the value it keeps does not change.
            newType->setCurrentIndex(newType->findData(5));add->click();
            auto *speed=window.findChild<QDoubleSpinBox *>("walkSpeed");auto *units=window.findChild<QToolButton *>("walkUnits");QVERIFY(speed && units);
            speed->setValue(2);units->click();QVERIFY(units->isChecked());QCOMPARE(speed->suffix(),QString(" km/h"));QVERIFY(std::abs(speed->value()-7.2)<1e-9);
            units->click();QCOMPARE(speed->suffix(),QString(" m/s"));QVERIFY(std::abs(speed->value()-2)<1e-9);
            remove->click();
        }
        window.activateWindow();QApplication::setActiveWindow(&window);QVERIFY(QTest::qWaitForWindowActive(&window)); // popups only open in the active window
        for (auto *combo:{shape,newType}) { // Popups must show every item uncut despite the themed item padding.
            combo->showPopup();auto *view=combo->view();QTRY_VERIFY(view->isVisible());QTest::qWait(300); // let the popup settle its final size
            {const QRect comboRect(combo->mapToGlobal(QPoint(0,0)),combo->size());const QRect popupRect=view->window()->frameGeometry();
             QVERIFY2(!popupRect.intersects(comboRect),qPrintable(combo->objectName()+" popup covers the combo"));
             QCOMPARE(popupRect.left(),comboRect.left());QCOMPARE(popupRect.width(),comboRect.width());}
            for (int row=0;row<combo->count();++row) {const auto r=view->visualRect(view->model()->index(row,0));QVERIFY2(r.isValid() && view->viewport()->rect().contains(r),qPrintable(combo->objectName()+" row "+QString::number(row)));}
            combo->hidePopup();
        }
        duplicate->click();QCOMPARE(modifiers->topLevelItemCount(),3);remove->click();QCOMPARE(modifiers->topLevelItemCount(),2);
        const auto rowPosition=modifiers->visualItemRect(modifiers->currentItem()).center();bool menuSeen=false;
        QTimer::singleShot(0,&window,[&] {
            auto *menu=qobject_cast<QMenu *>(QApplication::activePopupWidget());if (!menu) return;
            for (auto *action:menu->actions()) if (action->text()=="Disable temporarily") {menuSeen=true;action->trigger();menu->close();break;}
        });
        QVERIFY(QMetaObject::invokeMethod(modifiers,"customContextMenuRequested",Qt::DirectConnection,Q_ARG(QPoint,rowPosition)));QVERIFY(menuSeen);
        QCOMPARE(modifiers->currentItem()->checkState(0),Qt::Unchecked);remove->click();QCOMPARE(modifiers->topLevelItemCount(),1);
        auto *sliderControl=window.findChild<RangeSlider *>("captureRangeSlider");QVERIFY(sliderControl);
        QVERIFY(modifiers->mapTo(&window,QPoint(0,0)).y()>sliderControl->mapTo(&window,QPoint(0,0)).y());
        auto *viewport=window.findChild<Viewport *>();QVERIFY(viewport);viewport->setFocus();
        auto *display=window.findChild<QWidget *>("viewportDisplayControls");QVERIFY(display);
        auto *cube=window.findChild<QWidget *>("viewCube");QVERIFY(cube);QCOMPARE(display->parentWidget(),cube);QVERIFY(cube->rect().contains(display->geometry()));
        auto *pointSize=window.findChild<QDoubleSpinBox *>("displayPointSize");QVERIFY(pointSize);QCOMPARE(pointSize->value(),5.);QCOMPARE(pointSize->prefix(),QString("Point size  "));QTRY_COMPARE(pointSize->height(),ViewCube::rowHeight);
        auto *ghost=window.findChild<QToolButton *>("ghostComparison");QVERIFY(ghost);QVERIFY(!ghost->isChecked());QVERIFY(!viewport->ghostEnabled());
        auto *opacity=window.findChild<QSlider *>("ghostOpacity");QVERIFY(opacity);QCOMPARE(opacity->value(),15);QVERIFY(!opacity->isEnabled());QVERIFY(std::abs(viewport->ghostOpacity()-.15f)<1e-6);
        QVERIFY(std::abs(ghost->mapTo(display,QPoint(ghost->width()/2,0)).x()-display->width()/2)<=1);
        const auto ghostOff=ghost->icon().pixmap({24,24},QIcon::Normal,QIcon::Off).toImage();
        ghost->click();QVERIFY(ghost->isChecked());QVERIFY(viewport->ghostEnabled());QVERIFY(opacity->isEnabled());QVERIFY(ghost->icon().pixmap({24,24},QIcon::Normal,QIcon::On).toImage()!=ghostOff);
        opacity->setValue(65);QVERIFY(std::abs(viewport->ghostOpacity()-.65f)<1e-6);
        ghost->click();QVERIFY(!viewport->ghostEnabled());QVERIFY(!ghost->isChecked());QVERIFY(!opacity->isEnabled());opacity->setValue(15);
        auto *frameField=window.findChild<QDoubleSpinBox *>("timelineFrame"),*inField=window.findChild<QDoubleSpinBox *>("timelineIn"),*outField=window.findChild<QDoubleSpinBox *>("timelineOut");
        auto *secondsButton=window.findChild<QToolButton *>("timelineSeconds");QVERIFY(frameField);QVERIFY(inField);QVERIFY(outField);QVERIFY(secondsButton);QVERIFY(!secondsButton->isChecked());
        QCOMPARE(frameField->prefix(),QString("Frame "));QCOMPARE(frameField->suffix(),QString(" of 5"));QCOMPARE(frameField->maximum(),4.);
        auto *transport=window.findChild<QWidget *>("timelinePlaybackControls");auto *fields=window.findChild<QWidget *>("timelineFrameControls");auto *speedControls=window.findChild<QWidget *>("timelineSpeedControls");QVERIFY(transport);QVERIFY(fields);QVERIFY(speedControls);
        QVERIFY(transport->mapTo(&window,QPoint()).y()<sliderControl->mapTo(&window,QPoint()).y());
        QVERIFY(fields->geometry().right()<transport->geometry().left());QVERIFY(transport->geometry().right()<speedControls->geometry().left());
        QVERIFY(std::abs(transport->geometry().center().x()-transport->parentWidget()->width()/2)<=1);
        for (auto *label:transport->parentWidget()->findChildren<QLabel *>()) QVERIFY(label->text()!="TIMELINE");
        auto enter=[&](QDoubleSpinBox *field,double value) {field->setFocus();field->selectAll();QTest::keyClicks(field,field->locale().toString(value,'f',3));QTest::keyClick(field,Qt::Key_Return);};
        secondsButton->click();QVERIFY(secondsButton->isChecked());QCOMPARE(frameField->prefix(),QString("Time "));QCOMPARE(frameField->suffix(),QString(" of 0.167 s"));
        QTRY_VERIFY(frameField->width()>=frameField->sizeHint().width());QVERIFY(inField->width()>=inField->sizeHint().width());QVERIFY(outField->width()>=outField->sizeHint().width());
        QVERIFY(secondsButton->geometry().right()<inField->geometry().left());QVERIFY(inField->geometry().right()<frameField->geometry().left());QVERIFY(frameField->geometry().right()<outField->geometry().left());
        enter(frameField,.045);QCOMPARE(sliderControl->playheadValue(),1);QCOMPARE(frameField->value(),.033);
        enter(frameField,.044);QCOMPARE(sliderControl->playheadValue(),1);QCOMPARE(frameField->value(),.033);
        enter(frameField,.061);QCOMPARE(sliderControl->playheadValue(),2);QCOMPARE(frameField->value(),.067);
        enter(inField,.025);enter(outField,.112);QCOMPARE(sliderControl->startValue(),1);QCOMPARE(sliderControl->endValue(),3);QCOMPARE(inField->value(),.033);QCOMPARE(outField->value(),.1);
        enter(inField,.125);QCOMPARE(sliderControl->startValue(),4);QCOMPARE(sliderControl->endValue(),4);QCOMPARE(sliderControl->playheadValue(),4);
        enter(outField,.021);QCOMPARE(sliderControl->startValue(),1);QCOMPARE(sliderControl->endValue(),1);QCOMPARE(sliderControl->playheadValue(),1);
        enter(frameField,.09);QCOMPARE(frameField->value(),.033);QCOMPARE(sliderControl->playheadValue(),1);
        secondsButton->click();QVERIFY(!secondsButton->isChecked());QCOMPARE(frameField->value(),1.);QCOMPARE(inField->value(),1.);QCOMPARE(outField->value(),1.);
        inField->setValue(0);outField->setValue(4);frameField->setValue(0);QCOMPARE(sliderControl->startValue(),0);QCOMPARE(sliderControl->endValue(),4);
        viewport->grabFramebuffer();auto *stats=viewport->findChild<QLabel *>("viewportStatistics");QVERIFY(stats);QVERIFY(stats->text().endsWith("0.000 s / 0.167 s"));
        viewport->setFocus();
        QTest::keyClick(viewport,Qt::Key_Tab);QTRY_VERIFY(button->isChecked());QVERIFY(viewport->cropEditing());
        auto crop=viewport->crop();auto captureTransform=viewport->transform();
        QTest::keyClick(viewport,Qt::Key_Tab);QTRY_VERIFY(!button->isChecked());QVERIFY(!viewport->cropEditing());
        QCOMPARE(viewport->crop().transform.position,crop.transform.position);QCOMPARE(viewport->transform().position,captureTransform.position);
        // Mouse and keyboard operations share the viewport state, including target changes.
        QTest::mouseClick(button,Qt::LeftButton);QVERIFY(button->isChecked());QVERIFY(viewport->cropEditing());
        QTest::mouseClick(button,Qt::LeftButton);QVERIFY(!button->isChecked());QVERIFY(!viewport->cropEditing());
        for (int mode=0;mode<3;++mode) {
            auto *modeButton=window.findChild<QToolButton *>(QString("transformMode_%1").arg(mode));auto *referenceSpace=window.findChild<QToolButton *>(QString("coordinateSpace_%1").arg(mode));QVERIFY(modeButton);QVERIFY(referenceSpace);
            QTest::mouseClick(modeButton,Qt::LeftButton);QCOMPARE(viewport->transformMode(),TransformMode(mode+1));QVERIFY(modeButton->isChecked());QVERIFY(referenceSpace->isEnabled());
            QTest::mouseClick(referenceSpace,Qt::LeftButton);QCOMPARE(viewport->coordinateSpace(TransformMode(mode+1)),CoordinateSpace::Local);
            QTest::mouseClick(modeButton,Qt::LeftButton);QCOMPARE(viewport->transformMode(),TransformMode::None);QVERIFY(!modeButton->isChecked());QVERIFY(!referenceSpace->isEnabled());
            const auto key=mode==0 ? Qt::Key_G : mode==1 ? Qt::Key_R : Qt::Key_S;
            QTest::keyClick(viewport,key);QVERIFY(modeButton->isChecked());QCOMPARE(viewport->coordinateSpace(TransformMode(mode+1)),CoordinateSpace::Local);
            QTest::keyClick(viewport,key);QVERIFY(modeButton->isChecked());QCOMPARE(viewport->transformMode(),TransformMode(mode+1));QCOMPARE(viewport->coordinateSpace(TransformMode(mode+1)),CoordinateSpace::Global);QCOMPARE(referenceSpace->accessibleName(),QString("Global reference space"));
            QTest::keyClick(viewport,key);QVERIFY(modeButton->isChecked());QCOMPARE(viewport->coordinateSpace(TransformMode(mode+1)),CoordinateSpace::Local);QCOMPARE(referenceSpace->accessibleName(),QString("Local reference space"));
            QTest::keyClick(viewport,Qt::Key_Escape);QVERIFY(!modeButton->isChecked());QVERIFY(!referenceSpace->isEnabled());
        }
        QVERIFY(!display->findChild<QCheckBox *>()); // the grid toggle lives in the View menu, not the view-cube panel
        auto *grid=window.findChild<QAction *>("gridAndAxes");QVERIFY(grid);QVERIFY(grid->isCheckable());const bool gridBefore=grid->isChecked();
        QTest::keyClick(viewport,Qt::Key_G);QCOMPARE(grid->isChecked(),gridBefore);QTest::keyClick(viewport,Qt::Key_Escape);
        QTest::keyClick(viewport,Qt::Key_G,Qt::ShiftModifier);QCOMPARE(grid->isChecked(),!gridBefore);QTest::keyClick(viewport,Qt::Key_G,Qt::ShiftModifier);QCOMPARE(grid->isChecked(),gridBefore);
        const auto moveBefore=viewport->coordinateSpace(TransformMode::Move),scaleBefore=viewport->coordinateSpace(TransformMode::Scale);
        QTest::keyClick(viewport,Qt::Key_Tab);QVERIFY(viewport->cropEditing());const auto sourceBefore=viewport->transform();const auto cropBefore=viewport->crop();
        // Move and Rotate keep the capture's reference space while a crop is edited; Scale is
        // always local there, with no space toggle and no scale fields (its size is its dimensions).
        QTest::keyClick(viewport,Qt::Key_S);QVERIFY(viewport->cropEditing());QCOMPARE(viewport->coordinateSpace(TransformMode::Scale),CoordinateSpace::Local);
        QVERIFY(!window.findChild<QToolButton *>("coordinateSpace_2")->isEnabled());QVERIFY(!window.findChild<QDoubleSpinBox *>("transform_2_0")->isEnabled());
        QTest::keyClick(viewport,Qt::Key_S);QCOMPARE(viewport->coordinateSpace(TransformMode::Scale),CoordinateSpace::Local);
        QTest::keyClick(viewport,Qt::Key_G);QCOMPARE(viewport->transformMode(),TransformMode::Move);QVERIFY(viewport->cropEditing());QCOMPARE(viewport->coordinateSpace(TransformMode::Move),moveBefore);
        QTest::keyClick(viewport,Qt::Key_G);QVERIFY(viewport->coordinateSpace(TransformMode::Move)!=moveBefore);QCOMPARE(viewport->transform().position,sourceBefore.position);QCOMPARE(viewport->crop().transform.position,cropBefore.transform.position);QTest::keyClick(viewport,Qt::Key_Tab);
        // Leaving the crop gives Scale back the capture's own space, untouched by the forced Local.
        QCOMPARE(viewport->coordinateSpace(TransformMode::Scale),scaleBefore);
        QTest::keyClick(viewport,Qt::Key_Tab);QVERIFY(button->isChecked());duplicate->click();QCOMPARE(modifiers->topLevelItemCount(),2);QVERIFY(!button->isChecked());QVERIFY(!viewport->cropEditing());QCOMPARE(viewport->transformMode(),TransformMode::None);
        QTest::keyClick(viewport,Qt::Key_Tab);QVERIFY(button->isChecked());modifiers->setCurrentItem(modifiers->topLevelItem(0));QVERIFY(!button->isChecked());QVERIFY(!viewport->cropEditing());
        modifiers->setCurrentItem(modifiers->topLevelItem(1));remove->click();
        auto *position=window.findChild<QDoubleSpinBox *>("transform_0_0");QVERIFY(position);
        auto *axisLabel=window.findChild<QLabel *>("transformAxis_0_0");QVERIFY(axisLabel);QCOMPARE(axisLabel->cursor().shape(),Qt::SizeHorCursor);
        {
            const double before=position->value();const QPoint start=axisLabel->rect().center();
            QTest::mousePress(axisLabel,Qt::LeftButton,{},start);QTest::mouseMove(axisLabel,start+QPoint(20,0));
            QTest::mouseRelease(axisLabel,Qt::LeftButton,{},start+QPoint(20,0));
            QVERIFY(std::abs(position->value()-(before+0.1))<1e-6);
            QTest::mousePress(axisLabel,Qt::LeftButton,{},start);QTest::mouseMove(axisLabel,start+QPoint(40,0));
            QTest::keyClick(axisLabel,Qt::Key_Escape);QVERIFY(std::abs(position->value()-(before+0.1))<1e-6);
            QTest::mouseRelease(axisLabel,Qt::LeftButton,{},start+QPoint(40,0));
            position->setValue(before);
        }
        position->setFocus();
        QTest::keyClick(position,Qt::Key_Tab);QTRY_VERIFY(button->isChecked());QVERIFY(viewport->cropEditing());
        auto *slider=window.findChild<RangeSlider *>("captureRangeSlider");QVERIFY(slider);slider->setRangeValues(1,3);
        QCOMPARE(slider->startValue(),1);QCOMPARE(slider->endValue(),3);

        PresetStore presets(dir.filePath("presets")); Project setup;
        setup.transform.position={2,3,4}; setup.captureSettings.title="Template title";setup.captureSettings.catalogueId="template-id";
        setup.captureSettings.author="Template author";
        QString editorPath,metadataPath,error;
        QVERIFY(presets.save("Shared name",setup,&editorPath,&error,PresetScope::Editor));
        QVERIFY(presets.save("Other metadata",setup,&metadataPath,&error,PresetScope::Metadata));
        QFile editorFile(editorPath);QVERIFY(editorFile.open(QIODevice::ReadOnly));const auto editorBytes=editorFile.readAll();editorFile.close();
        Project current;current.captureSettings.title="Capture title";current.captureSettings.catalogueId="capture-id";
        CaptureSettingsDialog dialog(current,presets,&window);dialog.setWindowModality(Qt::WindowModal);dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));dialog.activateWindow();QApplication::setActiveWindow(&dialog);
        auto *metadataCombo=dialog.findChild<QComboBox *>("metadataPresetCombo");QVERIFY(metadataCombo);
        QCOMPARE(metadataCombo->count(),2);QCOMPARE(metadataCombo->findData(editorPath),-1);
        auto *title=dialog.findChild<QLineEdit *>("title"),*id=dialog.findChild<QLineEdit *>("catalogueId");QVERIFY(title);QVERIFY(id);
        title->setText("Edited capture title");id->setText("edited-capture-id");
        metadataCombo->setCurrentIndex(metadataCombo->findData(metadataPath));
        QVERIFY(QMetaObject::invokeMethod(metadataCombo,"activated",Qt::DirectConnection,Q_ARG(int,metadataCombo->currentIndex())));
        QCOMPARE(dialog.settings().title,QString("Edited capture title"));QCOMPARE(dialog.settings().catalogueId,QString("edited-capture-id"));
        QCOMPARE(dialog.settings().author,QString("Template author"));
        title->setFocus();bool nameDialogSeen=false;
        QTimer::singleShot(0,&dialog,[&] {
            auto *input=qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (!input) return;
            nameDialogSeen=input->windowTitle()=="Save metadata preset";
            if (nameDialogSeen) {input->setTextValue("Shared name");input->accept();} else input->reject();
        });
        QTest::keyClick(title,Qt::Key_P,Qt::ControlModifier|Qt::ShiftModifier);
        QVERIFY(nameDialogSeen);
        QCOMPARE(presets.list(PresetScope::Editor).size(),1);QCOMPARE(presets.list(PresetScope::Metadata).size(),2);
        QVERIFY(editorFile.open(QIODevice::ReadOnly));QCOMPARE(editorFile.readAll(),editorBytes);editorFile.close();
        const QString saved=metadataCombo->currentData().toString();QVERIFY(saved.endsWith(".presetmetadata"));
        EditorPreset metadata;QVERIFY(presets.read(saved,PresetScope::Metadata,&metadata,&error));
        QCOMPARE(metadata.name,QString("Shared name"));QCOMPARE(metadata.settings.transform.position,QVector3D());
        dialog.close();window.activateWindow();QApplication::setActiveWindow(&window);
        auto *editorCombo=window.findChild<QComboBox *>("savedPresetCombo");QVERIFY(editorCombo);
        QTRY_COMPARE(editorCombo->count(),2);QCOMPARE(editorCombo->findData(saved),-1);
        Camera camera=viewport->camera();camera.target={4,5,6};camera.yaw=57;camera.pitch=23;camera.distance=8;viewport->setCamera(camera);
        pointSize->setValue(7);grid->setChecked(false);ghost->click();QVERIFY(ghost->isChecked());opacity->setValue(62);const auto ghostCount=viewport->ghostPointCount();const double ghostTime=viewport->ghostTime();
        editorCombo->setCurrentIndex(editorCombo->findData(editorPath));
        QVERIFY(QMetaObject::invokeMethod(editorCombo,"activated",Qt::DirectConnection,Q_ARG(int,editorCombo->currentIndex())));
        QCOMPARE(viewport->transform().position,setup.transform.position);QCOMPARE(viewport->camera().target,camera.target);
        QCOMPARE(viewport->camera().yaw,camera.yaw);QCOMPARE(viewport->camera().pitch,camera.pitch);QCOMPARE(viewport->camera().distance,camera.distance);
        QCOMPARE(pointSize->value(),7.);QVERIFY(!grid->isChecked());QVERIFY(ghost->isChecked());QVERIFY(opacity->isEnabled());QCOMPARE(opacity->value(),62);QVERIFY(std::abs(viewport->ghostOpacity()-.62f)<1e-6);
        QCOMPARE(viewport->ghostPointCount(),ghostCount);QCOMPARE(viewport->ghostTime(),ghostTime);ghost->click();QVERIFY(!opacity->isEnabled());
        newType->setCurrentIndex(newType->findData(2));add->click();QCOMPARE(modifiers->topLevelItemCount(),2);
        auto *saturation=window.findChild<QDoubleSpinBox *>("greenMinimumSaturation");QVERIFY(saturation);QVERIFY(saturation->isVisible());QCOMPARE(saturation->value(),50.);
        QVERIFY(!button->isVisible());
        for (int i=0;i<20;++i) {const QPoint eye(18,modifiers->visualItemRect(modifiers->currentItem()).center().y());QTest::mouseClick(modifiers->viewport(),Qt::LeftButton,Qt::NoModifier,eye);QCOMPARE(modifiers->currentItem()->checkState(0),i%2 ? Qt::Checked : Qt::Unchecked);QCOMPARE(modifiers->topLevelItemCount(),2);}
        modifiers->currentItem()->setText(0,"Renamed green filter");QCOMPARE(modifiers->currentItem()->text(0),QString("Renamed green filter"));
        newType->setCurrentIndex(newType->findData(3));add->click();QCOMPARE(modifiers->topLevelItemCount(),3);QCOMPARE(modifiers->currentItem()->text(1),QString("Animate transform"));
        auto *keyTable=window.findChild<QTableWidget *>("transformKeyTable");auto *setKey=window.findChild<QPushButton *>("setTransformKey");auto *removeKey=window.findChild<QPushButton *>("removeTransformKey");QVERIFY(keyTable);QVERIFY(setKey);QVERIFY(removeKey);QVERIFY(keyTable->isVisible());
        sliderControl->setRangeValues(0,4);
        QMetaObject::invokeMethod(sliderControl,"rangeChanged",Qt::DirectConnection,Q_ARG(int,0),Q_ARG(int,4),Q_ARG(int,0));
        auto seek=[&](int frame) {QMetaObject::invokeMethod(sliderControl,"playheadChanged",Qt::DirectConnection,Q_ARG(int,frame));};
        seek(0);setKey->click();QCOMPARE(keyTable->rowCount(),1);QCOMPARE(keyTable->item(0,0)->text(),QString("0"));
        const auto reference=viewport->transform();seek(4);position->setValue(reference.position.x()+4);QCOMPARE(keyTable->rowCount(),2);QCOMPARE(keyTable->item(1,0)->text(),QString("4"));
        seek(2);QVERIFY(std::abs(viewport->transform().position.x()-reference.position.x()-2)<1e-4);position->setValue(reference.position.x()+3);QCOMPARE(keyTable->rowCount(),3);
        keyTable->item(1,1)->setText("6, 0, 0");QVERIFY(std::abs(viewport->transform().position.x()-reference.position.x()-6)<1e-4);
        keyTable->item(1,0)->setText("3");QCOMPARE(keyTable->item(1,0)->text(),QString("3"));seek(3);QVERIFY(std::abs(viewport->transform().position.x()-reference.position.x()-6)<1e-4);
        QCOMPARE(modifiers->currentItem()->data(2,Qt::UserRole+4).toList(),QVariantList({0,3,4}));
        removeKey->click();QCOMPARE(keyTable->rowCount(),2);seek(2);QVERIFY(std::abs(viewport->transform().position.x()-reference.position.x()-2)<1e-4);
        modifiers->currentItem()->setCheckState(0,Qt::Unchecked);QVERIFY((viewport->transform().position-reference.position).length()<1e-4);QCOMPARE(keyTable->rowCount(),2);QTest::keyClick(viewport,Qt::Key_G);QCOMPARE(viewport->transformMode(),TransformMode::None);
        modifiers->currentItem()->setCheckState(0,Qt::Checked);QVERIFY(std::abs(viewport->transform().position.x()-reference.position.x()-2)<1e-4);
        const int keyX=modifiers->columnViewportPosition(2)+8,keyY=modifiers->visualItemRect(modifiers->currentItem()).center().y();QTest::mouseClick(modifiers->viewport(),Qt::LeftButton,Qt::NoModifier,{keyX,keyY});QVERIFY((viewport->transform().position-reference.position).length()<1e-4);
        seek(0);QVERIFY((viewport->transform().position-reference.position).length()<1e-4);
        newType->setCurrentIndex(newType->findData(4));add->click();QCOMPARE(modifiers->topLevelItemCount(),4);QCOMPARE(modifiers->currentItem()->text(1),QString("Purge Isolated"));
        auto *isolation=window.findChild<QGroupBox *>("isolationModifierProperties");QVERIFY(isolation);QVERIFY(isolation->isVisible());QVERIFY(!saturation->isVisible());QVERIFY(!button->isVisible());QVERIFY(!keyTable->isVisible());
        auto *neighbour=window.findChild<QSpinBox *>("isolationNeighbour");auto *percent=window.findChild<QDoubleSpinBox *>("isolationMedianPercent");QVERIFY(neighbour);QVERIFY(percent);QCOMPARE(neighbour->value(),4);QCOMPARE(percent->value(),700.);
        QVERIFY(modifiers->viewport()->rect().contains(modifiers->visualItemRect(modifiers->topLevelItem(0))));
        QVERIFY(modifiers->viewport()->rect().contains(modifiers->visualItemRect(modifiers->topLevelItem(3))));
        remove->click();remove->click();modifiers->setCurrentItem(modifiers->topLevelItem(1));
        remove->click();QCOMPARE(modifiers->topLevelItemCount(),1);QVERIFY(button->isVisible());
        window.setWindowModified(false);
    }
};
int main(int argc,char **argv) {
    QSurfaceFormat format;format.setVersion(3,3);format.setProfile(QSurfaceFormat::CoreProfile);format.setDepthBufferSize(24);QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv);EditorTheme::install();QCoreApplication::setOrganizationName("Editor Tests");QCoreApplication::setApplicationName("MainWindow Tests");
    MainWindowTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "mainwindow_tests.moc"
