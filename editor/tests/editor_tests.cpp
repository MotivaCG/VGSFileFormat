#include "captureworker.h"
#include "project.h"
#include "filehistory.h"
#include "presetstore.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

class EditorTests : public QObject {
    Q_OBJECT
private slots:
    void reusablePresetStorage() {
        QTemporaryDir directory; PresetStore store(directory.filePath("presets")); Project settings;
        settings.asset = "D:/private-shoot/capture.mint"; settings.time = 12; settings.in = 10; settings.out = 20;
        settings.transform.position = {1,2,3}; settings.transform.rotation = {10,20,30}; settings.transform.scale = {2,3,4}; settings.transform.shear = {0.1f,0.2f,0.3f};
        settings.crop.enabled = true; settings.crop.radius = 1.5f; settings.crop.height = 2.5f;
        settings.crop.shape = CropShape::Box; settings.crop.width = 2; settings.crop.depth = 3;
        settings.captureSettings.title = "Reusable settings"; settings.captureSettings.despill = true;
        settings.crop.transform.position = {4,5,6}; settings.crop.transform.rotation = {30,20,10}; settings.crop.transform.scale = {0.5f,2,3};
        settings.spaces[1] = CoordinateSpace::Local; settings.pointSize = 5; settings.grid = false; settings.speed = 0.5; settings.loop = false;
        settings.camera.preset = ViewPreset::Front; settings.camera.orthographic = true; settings.camera.yaw = settings.camera.pitch = 0;
        QString path,error; QVERIFY2(store.save("Reusable: ../T4DS?",settings,&path,&error,PresetScope::Editor),qPrintable(error));
        QCOMPARE(QFileInfo(path).absolutePath(),store.directory()); QCOMPARE(store.list().size(),1);
        PresetStore reloaded(store.directory()); EditorPreset preset;
        QVERIFY2(reloaded.read(path,&preset,&error),qPrintable(error)); QCOMPARE(preset.name,QString("Reusable: ../T4DS?"));
        QCOMPARE(preset.settings.transform.position,settings.transform.position); QCOMPARE(preset.settings.transform.shear,settings.transform.shear);
        QCOMPARE(preset.settings.crop.transform.position,settings.crop.transform.position); QCOMPARE(preset.settings.crop.transform.scale,settings.crop.transform.scale);
        QCOMPARE(preset.settings.crop.radius,settings.crop.radius); QCOMPARE(preset.settings.crop.enabled,true);
        QCOMPARE(preset.settings.crop.shape,CropShape::Box); QCOMPARE(preset.settings.crop.depth,3.0f); QCOMPARE(preset.settings.captureSettings.despill,false);
        QCOMPARE(preset.settings.spaces[1],CoordinateSpace::Local); QCOMPARE(preset.settings.camera.preset,ViewPreset::Front);
        QCOMPARE(preset.settings.pointSize,5.0); QCOMPARE(preset.settings.speed,0.5); QCOMPARE(preset.settings.time,0.0);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto bytes = file.readAll(); file.close();
        QVERIFY(!bytes.contains(settings.asset.toUtf8())); const auto config = QJsonDocument::fromJson(bytes).object()["configuration"].toObject();
        QVERIFY(!config.contains("asset")); QVERIFY(!config.contains("timeline"));
        QVERIFY(!config.contains("captureSettings"));
        settings.transform.position = {9,8,7}; QString overwritten;
        QVERIFY(store.save("REUSABLE: ../T4DS?",settings,&overwritten,&error,PresetScope::Editor)); QCOMPARE(overwritten,path); QCOMPARE(store.list().size(),1);
        QVERIFY(store.read(path,&preset,&error)); QCOMPARE(preset.settings.transform.position,settings.transform.position);
        QVERIFY(!store.save("   ",settings,nullptr,&error,PresetScope::Editor));
        QFile broken(directory.filePath("presets/broken.json")); QVERIFY(broken.open(QIODevice::WriteOnly)); broken.write("{}"); broken.close();
        QCOMPARE(store.list().size(),1); QVERIFY(!store.read(broken.fileName(),&preset,&error));
        QCOMPARE(preset.settings.transform.position,settings.transform.position);
    }
    void presetScopesAllowSameNameWithoutCrossingData() {
        QTemporaryDir directory; PresetStore store(directory.path()); Project settings; settings.asset = "source.mint";
        settings.transform.position = {1,2,3}; settings.crop.enabled = true; settings.crop.shape = CropShape::Box;
        settings.captureSettings.title = "Metadata title"; settings.captureSettings.despill = true;
        QString editorPath,metadataPath,error;
        QVERIFY(store.save("T4DS",settings,&editorPath,&error,PresetScope::Editor));
        QVERIFY(store.save("T4DS",settings,&metadataPath,&error,PresetScope::Metadata));
        QVERIFY(editorPath!=metadataPath); QCOMPARE(store.list(PresetScope::Editor).size(),1); QCOMPARE(store.list(PresetScope::Metadata).size(),1);
        QCOMPARE(QFileInfo(editorPath).suffix(),QString("preset")); QCOMPARE(QFileInfo(metadataPath).suffix(),QString("presetmetadata"));
        EditorPreset editor,metadata; QVERIFY(store.read(editorPath,&editor,&error)); QVERIFY(store.read(metadataPath,&metadata,&error));
        QCOMPARE(editor.scope,PresetScope::Editor); QCOMPARE(metadata.scope,PresetScope::Metadata);
        QCOMPARE(editor.settings.transform.position,settings.transform.position); QCOMPARE(editor.settings.captureSettings.despill,false);
        QCOMPARE(metadata.settings.transform.position,QVector3D()); QCOMPARE(metadata.settings.crop.enabled,false);
        QCOMPARE(metadata.settings.captureSettings.title,settings.captureSettings.title);
        settings.captureSettings.title = "Updated metadata"; QString updated;
        QVERIFY(store.save("t4ds",settings,&updated,&error,PresetScope::Metadata)); QCOMPARE(updated,metadataPath);
        QVERIFY(store.read(editorPath,&editor,&error)); QCOMPARE(editor.settings.transform.position,QVector3D(1,2,3));
        QFile file(metadataPath); QVERIFY(file.open(QIODevice::ReadOnly)); const auto root = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(root["scope"].toString(),QString("capture-metadata")); QVERIFY(root["appliesTo"].isArray());
        QCOMPARE(root["format"].toString(),QString("vgs-editor-metadata-preset")); QCOMPARE(root["version"].toInt(),3);
        QCOMPARE(root["configuration"].toObject().size(),1);
        QVERIFY(!store.read(metadataPath,PresetScope::Editor,&editor,&error));
        QVERIFY(!store.read(editorPath,PresetScope::Metadata,&metadata,&error));
        QCOMPARE(metadata.settings.captureSettings.title,QString("Metadata title"));
        const QString renamed = directory.filePath("pretend-editor.preset");
        QVERIFY(QFile::copy(metadataPath,renamed));
        QCOMPARE(store.list(PresetScope::Editor).size(),1);
        QVERIFY(!store.read(renamed,PresetScope::Editor,&editor,&error));
    }
    void mixedAndScopedLegacyPresetsMigrateWithoutOverwriting() {
        QTemporaryDir directory; PresetStore store(directory.path()); Project settings;
        settings.transform.position = {1,2,3}; settings.captureSettings.author = "Legacy author"; settings.captureSettings.despill = true;
        auto mixed = settings.json(directory.filePath("old.json"));
        mixed.remove("format"); mixed.remove("version"); mixed.remove("asset"); mixed.remove("timeline");
        mixed["playback"] = QJsonObject{{"speed",settings.speed},{"loop",settings.loop}};
        QJsonObject legacy{{"format","vgs-editor-preset"},{"version",1},{"name","Shared name"},{"configuration",mixed}};
        const auto original = QJsonDocument(legacy).toJson(); QFile old(directory.filePath("old.json"));
        QVERIFY(old.open(QIODevice::WriteOnly)); QCOMPARE(old.write(original),qint64(original.size())); old.close();
        auto editorEntries = store.list(PresetScope::Editor), metadataEntries = store.list(PresetScope::Metadata);
        QCOMPARE(editorEntries.size(),1); QCOMPARE(metadataEntries.size(),1); QCOMPARE(editorEntries[0].name,metadataEntries[0].name);
        QVERIFY(editorEntries[0].path.endsWith(".preset")); QVERIFY(metadataEntries[0].path.endsWith(".presetmetadata"));
        EditorPreset editor,metadata; QString error;
        QVERIFY(store.read(editorEntries[0].path,PresetScope::Editor,&editor,&error));
        QVERIFY(store.read(metadataEntries[0].path,PresetScope::Metadata,&metadata,&error));
        QCOMPARE(editor.settings.transform.position,settings.transform.position); QCOMPARE(editor.settings.captureSettings.despill,false);
        QCOMPARE(metadata.settings.transform.position,QVector3D()); QCOMPARE(metadata.settings.captureSettings.author,QString("Legacy author"));
        QVERIFY(old.open(QIODevice::ReadOnly)); QCOMPARE(old.readAll(),original); old.close();
        settings.captureSettings.author = "Edited author"; QString saved;
        QVERIFY(store.save("SHARED NAME",settings,&saved,&error,PresetScope::Metadata)); QCOMPARE(saved,metadataEntries[0].path);
        QCOMPARE(store.list(PresetScope::Metadata).size(),1);
        QVERIFY(store.read(saved,PresetScope::Metadata,&metadata,&error)); QCOMPARE(metadata.settings.captureSettings.author,QString("Edited author"));
        QVERIFY(store.read(editorEntries[0].path,PresetScope::Editor,&editor,&error)); QCOMPARE(editor.settings.transform.position,QVector3D(1,2,3));

        // Version-2 JSON presets have one scope; they must create only that new file type.
        legacy["version"] = 2; legacy["name"] = "Metadata only"; legacy["scope"] = "capture-metadata";
        legacy["configuration"] = QJsonObject{{"captureSettings",settings.captureSettings.json()}};
        QFile scoped(directory.filePath("old-metadata.json")); QVERIFY(scoped.open(QIODevice::WriteOnly)); scoped.write(QJsonDocument(legacy).toJson()); scoped.close();
        QCOMPARE(store.list(PresetScope::Editor).size(),1); QCOMPARE(store.list(PresetScope::Metadata).size(),2);

        // A newer scoped JSON template wins over the mixed original with the same name.
        QTemporaryDir priorityDirectory; PresetStore priority(priorityDirectory.path());
        QVERIFY(QFile::copy(old.fileName(),priorityDirectory.filePath("a-mixed.json")));
        legacy["name"] = "Shared name";
        QFile newer(priorityDirectory.filePath("z-scoped.json")); QVERIFY(newer.open(QIODevice::WriteOnly)); newer.write(QJsonDocument(legacy).toJson()); newer.close();
        auto imported = priority.list(PresetScope::Metadata); QCOMPARE(imported.size(),1);
        QVERIFY(priority.read(imported[0].path,PresetScope::Metadata,&metadata,&error)); QCOMPARE(metadata.settings.captureSettings.author,QString("Edited author"));
    }
    void metadataConfigurationAndBoxContainment() {
        CaptureSettings settings; settings.title = "Title"; settings.catalogueId = "catalogue"; settings.author = "Studio";
        settings.plain = true; settings.despill = true; settings.despillStrength = 0.75; settings.shDegree = 2; settings.tags = {"scene","actor"};
        settings.extraJson = "{\"take\": 7}"; QString error; CaptureSettings restored;
        QVERIFY2(CaptureSettings::fromJson(settings.json(),&restored,&error),qPrintable(error)); QCOMPARE(restored.json(),settings.json());
        auto invalid = settings.json(); invalid["extraJson"] = "invalid JSON";
        QVERIFY(!CaptureSettings::fromJson(invalid,&restored,&error)); QCOMPARE(restored.title,settings.title);
        CropVolume crop; crop.enabled = true; crop.radius = 0.4f; crop.height = 1; crop.width = crop.depth = 0.8f;
        QVERIFY(!crop.contains({0.35f,0.5f,0.35f})); crop.shape = CropShape::Box; QVERIFY(crop.contains({0.35f,0.5f,0.35f}));
        QVERIFY(!crop.contains({0.41f,0.5f,0})); QVERIFY(!crop.contains({0,1.01f,0}));
    }
    void fileHistoryPersistsAndDeduplicates() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const QString store = dir.filePath("settings.ini");
        const QString capture = dir.filePath("capture.mint"), project = dir.filePath("project.vgsproj");
        {
            QSettings settings(store,QSettings::IniFormat); FileHistory history(settings);
            for (int i=0; i<12; ++i) history.remember(dir.filePath(QString("capture%1.vgs").arg(i)));
            QCOMPARE(history.recent().size(),FileHistory::MaximumRecentFiles);
            QVERIFY(!history.recent().contains(dir.filePath("capture0.vgs")));
            history.remember(capture); history.remember(project);
            history.remember(dir.filePath("./capture.mint"));
            QCOMPARE(history.recent().first(),capture); QCOMPARE(history.recent().count(capture),1);
#ifdef Q_OS_WIN
            history.remember(capture.toUpper());
            QCOMPARE(history.recent().size(),FileHistory::MaximumRecentFiles);
            history.remember(capture);
#endif
        }
        QSettings restored(store,QSettings::IniFormat); FileHistory history(restored);
        QCOMPARE(history.recent().first(),capture); QCOMPARE(history.recent()[1],project);
        QCOMPARE(restored.value("Files/LastCapture").toString(),capture);
        QCOMPARE(restored.value("Files/LastProject").toString(),project);
        // Missing files retain their recent entry but the dialog starts in a valid directory.
        QCOMPARE(history.openPath("Capture"),dir.path());
        QFile file(capture); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        QCOMPARE(history.openPath("Capture"),capture);
        QCOMPARE(history.savePath("Project","new.vgsproj"),dir.filePath("new.vgsproj"));
        history.rememberImage(dir.filePath("preview.png"));
        QCOMPARE(history.recent().size(),FileHistory::MaximumRecentFiles);
        QCOMPARE(history.savePath("Image","next.png"),dir.filePath("next.png"));
        history.clearRecent(); QVERIFY(history.recent().isEmpty());
        QCOMPARE(history.openPath("Capture"),capture); // Clearing the menu keeps dialog locations.
    }
    void projectRoundtrip() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        Project p; p.asset = dir.filePath("capture with spaces.mint");
        p.transform.position = {1,2,3}; p.transform.rotation = {10,20,30}; p.transform.scale = {0.5f,2,3};
        p.transform.shear = {0.1f,0.2f,0.3f}; p.spaces[0] = CoordinateSpace::Local; p.spaces[2] = CoordinateSpace::Local;
        p.camera.target = {1,2,3}; p.camera.distance = 8; p.time = 2; p.in = 1; p.out = 5; p.pointSize = 4;
        p.camera.preset = ViewPreset::Top; p.camera.pitch = 90; p.camera.orthographic = true;
        p.crop.enabled = true; p.crop.radius = 0.8f; p.crop.height = 2.4f;
        p.crop.transform.position = {1,2,3}; p.crop.transform.rotation = {20,30,40}; p.crop.transform.scale = {1,2,0.5f};
        QString error; const auto path = dir.filePath("sample.vgsproj");
        QVERIFY2(p.write(path,&error),qPrintable(error));
        Project restored; QVERIFY2(Project::read(path,&restored,&error),qPrintable(error));
        QCOMPARE(restored.asset,p.asset); QCOMPARE(restored.transform.position,p.transform.position);
        QCOMPARE(restored.transform.rotation,p.transform.rotation); QCOMPARE(restored.transform.scale,p.transform.scale);
        QCOMPARE(restored.transform.shear,p.transform.shear); QCOMPARE(restored.spaces[0],CoordinateSpace::Local); QCOMPARE(restored.spaces[2],CoordinateSpace::Local);
        QCOMPARE(restored.time,p.time); QCOMPARE(restored.pointSize,p.pointSize);
        QCOMPARE(restored.crop.enabled,p.crop.enabled); QCOMPARE(restored.crop.radius,p.crop.radius);
        QCOMPARE(restored.crop.height,p.crop.height); QCOMPARE(restored.crop.transform.rotation,p.crop.transform.rotation);
        QCOMPARE(restored.camera.preset,p.camera.preset); QCOMPARE(restored.camera.orthographic,true); QCOMPARE(restored.camera.pitch,90.0f);
        QCOMPARE(restored.json(path)["view"].toObject()["sh"].toBool(),true);
        auto legacy = p.json(path); legacy["version"] = 1; legacy.remove("crop");
        auto legacyCamera = legacy["camera"].toObject(); legacyCamera.remove("preset"); legacyCamera.remove("orthographic"); legacyCamera["pitch"] = 12; legacy["camera"] = legacyCamera;
        auto legacyView = legacy["view"].toObject(); legacyView["sh"] = false; legacy["view"] = legacyView;
        QFile legacyFile(path); QVERIFY(legacyFile.open(QIODevice::WriteOnly)); legacyFile.write(QJsonDocument(legacy).toJson()); legacyFile.close();
        QVERIFY(Project::read(path,&restored,&error)); QCOMPARE(restored.json(path)["view"].toObject()["sh"].toBool(),true);
        QCOMPARE(restored.crop.enabled,false); QCOMPARE(restored.camera.preset,ViewPreset::Free); QCOMPARE(restored.camera.orthographic,false);
        // Relocate a complete project directory: relative capture paths remain portable.
        QTemporaryDir relocated; QFile::copy(path,relocated.filePath("copy.vgsproj"));
        QVERIFY(Project::read(relocated.filePath("copy.vgsproj"),&restored,&error));
        QCOMPARE(restored.asset,relocated.filePath("capture with spaces.mint"));
    }
    void invalidProject() {
        QTemporaryDir dir; Project p; p.asset = "x.mint"; p.out = 5;
        auto j = p.json(dir.filePath("bad.vgsproj"));
        auto transform = j["transform"].toObject(); transform["scale"] = QJsonArray{0,1,1}; j["transform"] = transform;
        QFile file(dir.filePath("bad.vgsproj")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(j).toJson()); file.close();
        QString error; Project restored; restored.asset = "unchanged";
        QVERIFY(!Project::read(file.fileName(),&restored,&error)); QCOMPARE(restored.asset,QString("unchanged"));
    }
    void transformOrder() {
        Transform t; t.position = {10,0,0}; t.rotation = {0,0,90}; t.scale = {2,1,1};
        const auto result = t.matrix().map(QVector3D(1,0,0));
        QVERIFY((result-QVector3D(10,2,0)).length()<1e-5f);
    }
    void localRotationCompositionAndCropContainment() {
        Transform t; t.rotation = {25,50,70};
        for (int axis=0; axis<3; ++axis) {
            QVector3D direction; direction[axis] = 1;
            QMatrix4x4 expected = t.rotationMatrix(); expected.rotate(37,direction);
            const auto actual = t.rotatedLocal(axis,37).rotationMatrix();
            for (int i=0; i<3; ++i) for (int j=0; j<3; ++j) QVERIFY(std::abs(expected(i,j)-actual(i,j))<1e-5f);
        }
        CropVolume crop; crop.enabled = true; crop.radius = 0.5f; crop.height = 2;
        crop.transform.position = {2,3,4}; crop.transform.rotation = {45,20,30}; crop.transform.scale = {2,0.5f,1};
        const auto m = crop.transform.matrix();
        QVERIFY(crop.contains(m.map({0.3f,0.5f,0.2f})));
        QVERIFY(!crop.contains(m.map({0.6f,0,0}))); QVERIFY(!crop.contains(m.map({0,2.2f,0})));
        crop.enabled = false; QVERIFY(crop.contains({100,100,100}));
    }
    void affineScalingRoundtripAndLegacyBaseMigration() {
        Transform transform; transform.position = {1,2,3}; transform.rotation = {20,35,60}; transform.scale = {2,3,0.5f};
        QMatrix4x4 stretch; stretch.scale(1.7f,0.8f,1.2f);
        const auto matrix = stretch*transform.matrix(); const auto restored = Transform::fromMatrix(matrix).matrix();
        for (int i=0; i<4; ++i) for (int j=0; j<4; ++j) QVERIFY(std::abs(matrix(i,j)-restored(i,j))<1e-5f);
        QTemporaryDir dir; Project project; project.asset = dir.filePath("capture.mint"); project.out = 1;
        project.crop.enabled = true; project.crop.height = 2; project.crop.transform = transform;
        auto legacy = project.json(dir.filePath("legacy.vgsproj")); legacy["version"] = 2;
        const auto expectedBase = transform.matrix().map(QVector3D(0,-1,0));
        QFile file(dir.filePath("legacy.vgsproj")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(legacy).toJson()); file.close();
        QString error; Project migrated; QVERIFY2(Project::read(file.fileName(),&migrated,&error),qPrintable(error));
        QVERIFY((migrated.crop.transform.position-expectedBase).length()<1e-5f);
        QVERIFY(migrated.spaces[0]==CoordinateSpace::Global);
    }
    void captureIntegration() {
        const QString path = qEnvironmentVariable("EDITOR_TEST_CAPTURE");
        if (path.isEmpty()) QSKIP("Set EDITOR_TEST_CAPTURE to exercise a real MINT/VGS capture.");
        qRegisterMetaType<FramePtr>(); qRegisterMetaType<CaptureInfo>();
        CaptureWorker worker; QSignalSpy opened(&worker,&CaptureWorker::opened);
        QSignalSpy decoded(&worker,&CaptureWorker::decoded); QSignalSpy failed(&worker,&CaptureWorker::failed);
        worker.open(path,1,true);
        QVERIFY2(failed.isEmpty(),failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(opened.size(),1);
        const auto info = qvariant_cast<CaptureInfo>(opened.first()[0]);
        const auto frame = qvariant_cast<FramePtr>(opened.first()[1]);
        QVERIFY(info.duration>0); QVERIFY(info.frames>0); QVERIFY(!frame->points.empty());
        QCOMPARE(frame->records.size(),size_t(frame->total)); QCOMPARE(frame->active.size(),size_t(frame->total));
        QCOMPARE(frame->sh.size(),size_t(frame->total)*size_t(frame->coefficients)*3);
        // Attributes survive the preview unchanged, including opacity and individual scale.
        bool hasTransparency = false, hasScale = false;
        for (const auto &point : frame->points) {
            const auto &record = frame->records[size_t(point.id)];
            for (int j=0; j<3; ++j) {
                QCOMPARE(point.position[j],record.position[j]);
                QCOMPARE(point.color[j],record.color[j]);
            }
            hasTransparency |= record.color[3] < 0.99f;
            hasScale |= record.scale[0] != record.scale[1];
        }
        QVERIFY(hasTransparency); QVERIFY(hasScale);
        size_t expectedPoints = 0;
        for (size_t i=0; i<frame->records.size(); ++i) {
            const auto &record = frame->records[i];
            bool finite = true;
            for (int j=0; j<3; ++j) finite &= std::isfinite(record.position[j]) && std::isfinite(record.color[j]);
            if (frame->active[i] && finite) ++expectedPoints;
        }
        QCOMPARE(frame->points.size(),expectedPoints); // No opacity or individual scale threshold.
        worker.decode(info.duration*0.5,1,true); QCOMPARE(decoded.size(),1);
        worker.decode(info.duration,1,false); QCOMPARE(decoded.size(),2);
        const auto last = qvariant_cast<FramePtr>(decoded.last()[0]); QVERIFY(last->seconds<info.duration);
        QVERIFY(last->sh.empty());
        worker.open(path+".missing",2,false); QCOMPARE(failed.size(),1);
        worker.decode(0,1,false); QCOMPARE(decoded.size(),3); // Failed opens preserve the old capture.
        worker.clear(); worker.decode(0,1,false); QCOMPARE(failed.size(),2);
    }
};
QTEST_GUILESS_MAIN(EditorTests)
#include "editor_tests.moc"
