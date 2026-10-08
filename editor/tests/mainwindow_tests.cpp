#include "mainwindow.h"
#include "viewport.h"
#include "viewcube.h"
#include "rangeslider.h"
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
#include <QGroupBox>
#include <QCheckBox>
#include <QTableWidget>
#include <QLabel>
#include <QSlider>
#include <QtTest>

class MainWindowTests : public QObject {
    Q_OBJECT
private slots:
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
        QTest::keyClick(viewport,Qt::Key_Tab);QVERIFY(viewport->cropEditing());const auto sourceBefore=viewport->transform();const auto cropBefore=viewport->crop();
        QTest::keyClick(viewport,Qt::Key_G);QCOMPARE(viewport->transformMode(),TransformMode::Move);QVERIFY(viewport->cropEditing());QCOMPARE(viewport->coordinateSpace(TransformMode::Move),CoordinateSpace::Global);
        QTest::keyClick(viewport,Qt::Key_G);QCOMPARE(viewport->coordinateSpace(TransformMode::Move),CoordinateSpace::Local);QCOMPARE(viewport->transform().position,sourceBefore.position);QCOMPARE(viewport->crop().transform.position,cropBefore.transform.position);QTest::keyClick(viewport,Qt::Key_Tab);
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
