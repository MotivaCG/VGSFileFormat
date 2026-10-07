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
        MainWindow window(nullptr,dir.filePath("presets"));window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));window.openPath(capture);
        auto *button=window.findChild<QPushButton *>("editCropVolume");QVERIFY(button);
        QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(),5000);
        auto *viewport=window.findChild<Viewport *>();QVERIFY(viewport);viewport->setFocus();
        QTest::keyClick(viewport,Qt::Key_Tab);QTRY_VERIFY(button->isChecked());QVERIFY(viewport->cropEditing());
        auto crop=viewport->crop();auto captureTransform=viewport->transform();
        QTest::keyClick(viewport,Qt::Key_Tab);QTRY_VERIFY(!button->isChecked());QVERIFY(!viewport->cropEditing());
        QCOMPARE(viewport->crop().transform.position,crop.transform.position);QCOMPARE(viewport->transform().position,captureTransform.position);
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
        window.setWindowModified(false);
    }
};
int main(int argc,char **argv) {
    QSurfaceFormat format;format.setVersion(3,3);format.setProfile(QSurfaceFormat::CoreProfile);format.setDepthBufferSize(24);QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv);QCoreApplication::setOrganizationName("Editor Tests");QCoreApplication::setApplicationName("MainWindow Tests");
    MainWindowTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "mainwindow_tests.moc"
