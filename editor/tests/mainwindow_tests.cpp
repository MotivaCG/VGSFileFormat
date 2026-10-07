#include "mainwindow.h"
#include "viewport.h"
#include "rangeslider.h"
#include "exportcapture.h"
#include "vgssign.h"
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
        auto *button=window.findChild<QPushButton *>("editCropVolume");QVERIFY(button);
        QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(),5000);
        auto *modifiers=window.findChild<QTreeWidget *>("modifierTree");QVERIFY(modifiers);QCOMPARE(modifiers->topLevelItemCount(),1);QCOMPARE(modifiers->topLevelItem(0)->childCount(),0);
        QCOMPARE(modifiers->topLevelItem(0)->checkState(0),Qt::Checked);QVERIFY(modifiers->visualItemRect(modifiers->topLevelItem(0)).height()>=30);
        const QPoint firstEye(18,modifiers->visualItemRect(modifiers->topLevelItem(0)).center().y());
        QTest::mouseClick(modifiers->viewport(),Qt::LeftButton,Qt::NoModifier,firstEye);QCOMPARE(modifiers->topLevelItem(0)->checkState(0),Qt::Unchecked);
        QTest::mouseClick(modifiers->viewport(),Qt::LeftButton,Qt::NoModifier,firstEye);QCOMPARE(modifiers->topLevelItem(0)->checkState(0),Qt::Checked);
        QCOMPARE(modifiers->topLevelItem(0)->text(2),QString());
        auto *add=window.findChild<QToolButton *>("addModifier"),*remove=window.findChild<QToolButton *>("removeModifier"),*duplicate=window.findChild<QToolButton *>("duplicateModifier");QVERIFY(add);QVERIFY(remove);QVERIFY(duplicate);
        auto *newType=window.findChild<QComboBox *>("newModifierType");QVERIFY(newType);newType->setCurrentIndex(newType->findData(1));add->click();
        QCOMPARE(modifiers->topLevelItemCount(),2);QCOMPARE(modifiers->currentItem()->text(1),QString("Crop box"));
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
        auto *pointSize=window.findChild<QDoubleSpinBox *>("displayPointSize");QVERIFY(pointSize);QCOMPARE(pointSize->value(),5.);
        auto *ghost=window.findChild<QToolButton *>("ghostComparison");QVERIFY(ghost);QVERIFY(!ghost->isChecked());QVERIFY(!viewport->ghostEnabled());
        QVERIFY(std::abs(ghost->mapTo(display,QPoint(ghost->width()/2,0)).x()-display->width()/2)<=1);
        const auto ghostOff=ghost->icon().pixmap({24,24},QIcon::Normal,QIcon::Off).toImage();
        ghost->click();QVERIFY(ghost->isChecked());QVERIFY(viewport->ghostEnabled());QVERIFY(ghost->icon().pixmap({24,24},QIcon::Normal,QIcon::On).toImage()!=ghostOff);
        ghost->click();QVERIFY(!viewport->ghostEnabled());QVERIFY(!ghost->isChecked());
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
        auto *grid=display->findChild<QCheckBox *>();QVERIFY(grid);const bool gridBefore=grid->isChecked();
        QTest::keyClick(viewport,Qt::Key_G);QCOMPARE(grid->isChecked(),gridBefore);QTest::keyClick(viewport,Qt::Key_Escape);
        QTest::keyClick(viewport,Qt::Key_G,Qt::ShiftModifier);QCOMPARE(grid->isChecked(),!gridBefore);QTest::keyClick(viewport,Qt::Key_G,Qt::ShiftModifier);QCOMPARE(grid->isChecked(),gridBefore);
        QTest::keyClick(viewport,Qt::Key_Tab);QVERIFY(viewport->cropEditing());const auto sourceBefore=viewport->transform();const auto cropBefore=viewport->crop();
        QTest::keyClick(viewport,Qt::Key_G);QCOMPARE(viewport->transformMode(),TransformMode::Move);QVERIFY(viewport->cropEditing());QCOMPARE(viewport->coordinateSpace(TransformMode::Move),CoordinateSpace::Global);
        QTest::keyClick(viewport,Qt::Key_G);QCOMPARE(viewport->coordinateSpace(TransformMode::Move),CoordinateSpace::Local);QCOMPARE(viewport->transform().position,sourceBefore.position);QCOMPARE(viewport->crop().transform.position,cropBefore.transform.position);QTest::keyClick(viewport,Qt::Key_Tab);
        QTest::keyClick(viewport,Qt::Key_Tab);QVERIFY(button->isChecked());duplicate->click();QCOMPARE(modifiers->topLevelItemCount(),2);QVERIFY(!button->isChecked());QVERIFY(!viewport->cropEditing());QCOMPARE(viewport->transformMode(),TransformMode::None);
        QTest::keyClick(viewport,Qt::Key_Tab);QVERIFY(button->isChecked());modifiers->setCurrentItem(modifiers->topLevelItem(0));QVERIFY(!button->isChecked());QVERIFY(!viewport->cropEditing());
        modifiers->setCurrentItem(modifiers->topLevelItem(1));remove->click();
        auto *position=window.findChild<QDoubleSpinBox *>("transform_0_0");QVERIFY(position);position->setFocus();
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
        editorCombo->setCurrentIndex(editorCombo->findData(editorPath));
        QVERIFY(QMetaObject::invokeMethod(editorCombo,"activated",Qt::DirectConnection,Q_ARG(int,editorCombo->currentIndex())));
        QCOMPARE(viewport->transform().position,setup.transform.position);QCOMPARE(viewport->camera().target,camera.target);
        QCOMPARE(viewport->camera().yaw,camera.yaw);QCOMPARE(viewport->camera().pitch,camera.pitch);QCOMPARE(viewport->camera().distance,camera.distance);
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
    QApplication app(argc,argv);QCoreApplication::setOrganizationName("Editor Tests");QCoreApplication::setApplicationName("MainWindow Tests");
    MainWindowTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "mainwindow_tests.moc"
