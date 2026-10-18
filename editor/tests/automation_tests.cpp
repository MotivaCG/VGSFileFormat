// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Victor M. Feliz. All rights reserved.
#include "editorautomation.h"
#include "mainwindow.h"
#include "exportcapture.h"
#include "vgssign.h"
#include "editortheme.h"
#include "viewport.h"
#include "taskqueuedialog.h"
#include "modifierpanel.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QMessageBox>
#include <QAbstractButton>
#include <QProgressDialog>
#include <QStatusBar>
#include <QApplication>
#include <QAction>
#include <QFile>
#include <QPainter>
#include <QDir>
#include <QFileDialog>
#include <QJsonDocument>
#include <QSettings>
#include <QProcess>
#include <QSurfaceFormat>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolTip>
#include <QTreeWidget>
#include <QtTest>

class AutomationTests : public QObject {
    Q_OBJECT
    static QString capture(const QString &directory, int frames=4,const vgs::Bytes &audio={}) {
        const QString path=directory+"/source.pgs";
        vgs::Frame f;f.count=3;f.active={1,1,1};f.position={-.2f,1,0,.2f,1,0,0,1.3f,0};
        f.rotation={0,0,0,1,0,0,0,1,0,0,0,1};f.scale={.01f,.02f,.03f,.01f,.02f,.03f,.01f,.02f,.03f};
        f.colorDc={.5f,.5f,.5f,.5f,.5f,.5f,.5f,.5f,.5f};f.opacity={1,1,1};
        vgs::Header h;h.shDegree=0;h.frameCount=h.durationTicks=frames;h.chunks.resize(frames);
        for (size_t i=0;i<size_t(frames);++i) {h.chunks[i].startTick=i;h.chunks[i].intervals=1;}
        vgs::EncodeOptions options;options.signer=vgs::authoringSigner();options.shDegree=0;options.compression=vgs::Compression::None;
        if (!audio.empty()) options.extras.push_back({vgs::AudioExtra,vgs::Wav,audio});
        QFile file(path);if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot create capture.");
        vgs::encodeSequence(h,[&](size_t) {return packExportFrame(f,0);},[&](uint64_t offset,const uint8_t *data,size_t count) {
            if (!file.seek(qint64(offset)) || file.write(reinterpret_cast<const char *>(data),qint64(count))!=qint64(count)) throw std::runtime_error("Cannot write capture.");
        },options);
        return path;
    }
    static void settings(const QString &dir) {
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir);
    }
private slots:
    void audioSourcesAndSettingsSurviveAutomationSaveAndOpen() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());
        // A valid mono WAV: the test exercises project persistence, not playback.
        const auto wav=QByteArray::fromHex("524946462c00000057415645666d74201000000001000100401f0000803e0000020010006461746108000000")+QByteArray(8,0);
        const auto asset=capture(dir.path(),4,vgs::Bytes(wav.begin(),wav.end()));
        QVERIFY(QDir().mkpath(dir.filePath("music")));QVERIFY(QDir().mkpath(dir.filePath("project")));
        const auto sound=dir.filePath("music/song.wav");QFile file(sound);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(wav),wav.size());file.close();
        MainWindow window(nullptr,dir.filePath("presets"));EditorAutomation bridge(&window,dir.filePath("sessions"));
        bridge.execute("open",{{"path",asset}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        auto readAudio=[&] {QJsonArray found;for (const auto &m:bridge.execute("project",{})["modifiers"].toArray()) if (m.toObject()["type"]=="audio") found.append(m);return found;};
        auto layers=readAudio();QCOMPARE(layers.size(),1);const auto original=layers[0].toObject();const auto id=original["id"].toString();QVERIFY(!id.isEmpty());
        QCOMPARE(original["audio"].toObject()["source"].toString(),QString("capture"));QVERIFY(original["enabled"].toBool());
        const QJsonObject audio{{"source","file"},{"file",sound},{"offset",-.25},{"volume",.35},{"loop",true}};
        bridge.execute("edit_modifier",{{"modifier_id",id},{"patch",QJsonObject{{"name","Music"},{"enabled",false},{"audio",audio}}}});
        QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        const auto saved=dir.filePath("project/edit.vgsproj");bridge.execute("save",{{"path",saved}});
        Project disk;QString error;QVERIFY2(Project::read(saved,&disk,&error),qPrintable(error));
        const Modifier *stored=nullptr;for (const auto &m:disk.modifiers) if (m.id==id) stored=&m;QVERIFY(stored);
        QCOMPARE(QFileInfo(stored->audioFile).canonicalFilePath(),QFileInfo(sound).canonicalFilePath());QCOMPARE(stored->audioOffset,-.25);QCOMPARE(stored->audioVolume,.35);QVERIFY(stored->audioLoop);QVERIFY(!stored->enabled);
        QFile json(saved);QVERIFY(json.open(QIODevice::ReadOnly));const auto root=QJsonDocument::fromJson(json.readAll()).object();json.close();
        QJsonObject savedAudio;for (const auto &m:root["modifiers"].toArray()) if (m.toObject()["id"]==id) savedAudio=m.toObject()["audio"].toObject();
        QCOMPARE(savedAudio["file"].toString(),QString("../music/song.wav"));
        bridge.execute("open",{{"path",saved}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        layers=readAudio();QCOMPARE(layers.size(),1);auto restored=layers[0].toObject();
        QCOMPARE(restored["id"].toString(),id);QCOMPARE(restored["name"].toString(),QString("Music"));QVERIFY(!restored["enabled"].toBool());
        auto settings=restored["audio"].toObject();QCOMPARE(settings,savedAudio);
        // The dropdown's capture choice discards the external reference and keeps
        // the other settings. Reopening must not add another embedded track.
        bridge.execute("edit_modifier",{{"modifier_id",id},{"patch",QJsonObject{{"audio",QJsonObject{{"source","capture"}}}}}});
        QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);bridge.execute("save",{{"path",saved}});
        bridge.execute("open",{{"path",saved}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        layers=readAudio();QCOMPARE(layers.size(),1);restored=layers[0].toObject();settings=restored["audio"].toObject();
        QCOMPARE(settings["source"].toString(),QString("capture"));QCOMPARE(settings["file"].toString(),QString());
        QCOMPARE(settings["offset"].toDouble(),-.25);QCOMPARE(settings["volume"].toDouble(),.35);QVERIFY(settings["loop"].toBool());QVERIFY(!restored["enabled"].toBool());
        window.setWindowModified(false);
    }
    void loopMorphIsReadSeekedAndEditedByAutomation() {try {loopMorphAutomation(0);} catch (const std::exception &e) {QFAIL(e.what());}}
    void loopMorphAutomation(int) { // a slot with an argument, so QTest does not run it on its own
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path(),6);
        MainWindow window(nullptr,dir.path()+"/presets");EditorAutomation bridge(&window,dir.path()+"/sessions");
        bridge.execute("open",{{"path",path}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        auto settle=[&] {QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),10000);};
        bridge.execute("range",{{"start",0},{"end",3}});settle();
        QVERIFY_EXCEPTION_THROWN(bridge.execute("loop_morph",{}),std::runtime_error); // none yet
        const auto added=bridge.execute("add_modifier",{{"type","loop-morph"}});const QString id=added["modifier_id"].toString();settle();
        // Two frames at the capture's 25 fps; with markers written by number.
        bridge.execute("edit_modifier",{{"modifier_id",id},{"patch",QJsonObject{{"morph",QJsonObject{{"seconds",.08},{"space","capture-local"},{"markers",QJsonArray{
            QJsonObject{{"number",1},{"start",QJsonArray{0,1,0}},{"startFrame",0},{"end",QJsonArray{0,1,0}},{"endFrame",3}},
            QJsonObject{{"number",2},{"start",QJsonArray{.2,1,0}},{"startFrame",0}}}}}}}}});settle();
        auto morph=bridge.execute("loop_morph",{});
        QCOMPARE(morph["frames"].toInt(),2);QCOMPARE(morph["morph_frames"].toArray(),QJsonArray({4,5}));QCOMPARE(morph["pairs"].toInt(),1);
        const auto markers=morph["markers"].toArray();QCOMPARE(markers.size(),2);QCOMPARE(markers[1].toObject()["end"].toString(),QString("missing"));
        // The morph's frames can be seeked, past End; not beyond them.
        bridge.execute("seek",{{"frame",5}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),10000);QCOMPARE(bridge.state()["frame"].toInt(),5);
        QVERIFY_EXCEPTION_THROWN(bridge.execute("seek",{{"frame",6}}),std::runtime_error);
        QCOMPARE(bridge.execute("loop_morph",{})["side_shown"].toString(),QString("none"));
        // A marker is placed on Start or End only, from a current view.
        QVERIFY_EXCEPTION_THROWN(bridge.execute("morph_marker",{{"view_id",bridge.state()["view_id"]},{"point",QJsonArray{.5,.5}}}),std::runtime_error);
        bridge.execute("seek",{{"frame",3}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),10000);
        QCOMPARE(bridge.execute("loop_morph",{})["side_shown"].toString(),QString("end"));
        QVERIFY_EXCEPTION_THROWN(bridge.execute("morph_marker",{{"view_id","stale"},{"point",QJsonArray{.5,.5}}}),std::runtime_error);
        bridge.execute("remove_morph_markers",{{"numbers",QJsonArray{2}}});settle();QCOMPARE(bridge.execute("loop_morph",{})["markers"].toArray().size(),1);
        bridge.execute("remove_morph_markers",{{"all",true}});settle();QCOMPARE(bridge.execute("loop_morph",{})["markers"].toArray().size(),0);
        bridge.execute("undo",{});settle();QCOMPARE(bridge.execute("loop_morph",{})["markers"].toArray().size(),1);
    }
    void choosingGuiExportDestinationDoesNotReserveTheExporter() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path());
        MainWindow window(nullptr,dir.path()+"/presets");EditorAutomation bridge(&window,dir.path()+"/sessions");
        bridge.execute("open",{{"path",path}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        int dialogs=0;bool allAvailable=true;QTimer dismiss;dismiss.setInterval(5);
        connect(&dismiss,&QTimer::timeout,this,[&] {
            for (auto *dialog:window.findChildren<QFileDialog *>()) if (dialog->isVisible()) {
                ++dialogs;auto lease=ExportLease::tryAcquire();allAvailable=allAvailable && bool(lease);dialog->reject();
            }
        });dismiss.start();
        for (const auto &shortcut:{QKeySequence("Ctrl+E"),QKeySequence("Ctrl+Alt+E")}) {
            QAction *action=nullptr;
            for (auto *candidate:window.findChildren<QAction *>()) if (candidate->shortcut()==shortcut) action=candidate;
            QVERIFY(action);action->trigger();
        }
        QCOMPARE(dialogs,2);QVERIFY(allAvailable);
    }
    void loadingSaveDisplayAndErrorOriginsUseSharedEditorOperations() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path());
        // Damage only the last chunk's payload, leaving the first frame loadable.
        QFile damaged(path);QVERIFY(damaged.open(QIODevice::ReadWrite));QVERIFY(damaged.seek(damaged.size()-1));
        auto byte=damaged.read(1);QCOMPARE(byte.size(),1);byte[0]=char(byte[0]^0x55);
        QVERIFY(damaged.seek(damaged.size()-1));QCOMPARE(damaged.write(byte),qint64(1));damaged.close();
        MainWindow window(nullptr,dir.path()+"/presets");EditorAutomation bridge(&window,dir.path()+"/sessions");
        QStringList dialogs;QTimer dismiss;dismiss.setInterval(10);
        connect(&dismiss,&QTimer::timeout,this,[&] {for (auto *box:window.findChildren<QMessageBox *>()) if (box->isVisible()) {dialogs.append(box->text());box->accept();}});dismiss.start();
        QSignalSpy requests(&window,&MainWindow::decodeRequested);
        bridge.execute("open",{{"path",path}});
        auto *overlay=window.findChild<QWidget *>("loadingOverlay");QVERIFY(overlay);QVERIFY(overlay->property("loading").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);QVERIFY(!overlay->property("loading").toBool());
        bridge.execute("display",{{"mode","points"},{"point_size",7}});
        bridge.execute("display",{{"mode","splats"}});QCOMPARE(bridge.state()["point_size"].toDouble(),7.);
        bridge.execute("save",{{"path",dir.path()+"/saved.vgsproj"}});QVERIFY(!window.isWindowModified());
        QVERIFY(window.statusBar()->currentMessage().contains("saved",Qt::CaseInsensitive));
        bridge.execute("seek",{{"frame",3}});
        // A GUI failure while the remote decode is active must still show its own dialog.
        window.openPath(dir.path()+"/missing.pgs");
        QTRY_VERIFY_WITH_TIMEOUT(!bridge.state()["processing"].toBool(),5000);
        QCOMPARE(dialogs.size(),1);QVERIFY(dialogs[0].contains("exist"));
        QVERIFY2(!bridge.state()["error"].toString().isEmpty(),"Corrupt chunk should fail the remote decode.");
        QVERIFY(bridge.state()["error"].toString()!=dialogs[0]);
        bridge.execute("seek",{{"frame",0}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        QVERIFY(bridge.state()["error"].toString().isEmpty());
        const auto spins=window.findChildren<QDoubleSpinBox *>();QDoubleSpinBox *frame=nullptr;
        for (auto *spin:spins) if (spin->objectName()=="timelineFrame") frame=spin;
        QVERIFY(frame);frame->setValue(3);
        QTRY_VERIFY_WITH_TIMEOUT(dialogs.size()==2,5000);QVERIFY(bridge.state()["error"].toString().isEmpty());
        QVERIFY(requests.size()>=3);
        QVERIFY(requests[requests.size()-1][4].toULongLong()!=requests[requests.size()-2][4].toULongLong());
    }
    void exportReservationCoversMcpAndTaskQueues() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path());
        MainWindow window(nullptr,dir.path()+"/presets");EditorAutomation bridge(&window,dir.path()+"/sessions");
        bridge.execute("open",{{"path",path}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        Project p;QString error;const auto root=bridge.execute("project",{});QVERIFY(Project::fromJson(root,dir.path(),&p,&error));
        ExportTask task;task.path=dir.path()+"/queued.vgstask";task.output=dir.path()+"/queued.pgs";task.project=p;
        QVERIFY2(writeExportTask(task,&error),qPrintable(error));
        TaskQueueDialog queue(&window);queue.addTasks({task.path});
        auto guiExport=ExportLease::tryAcquire();QVERIFY(guiExport);
        QVERIFY_EXCEPTION_THROWN(bridge.execute("export",{{"path",dir.path()+"/blocked.pgs"}}),std::runtime_error);
        queue.start();QVERIFY(!queue.running());QVERIFY(!QFile::exists(task.output));
        guiExport.reset();queue.start();QVERIFY(queue.running());
        QVERIFY_EXCEPTION_THROWN(bridge.execute("export",{{"path",dir.path()+"/still-blocked.pgs"}}),std::runtime_error);
        QTRY_VERIFY_WITH_TIMEOUT(!queue.running(),10000);QVERIFY(QFileInfo(task.output).size()>0);
        const auto started=bridge.execute("export",{{"path",dir.path()+"/mcp.pgs"}});
        const QString job=started["job_id"].toString();
        QTRY_COMPARE_WITH_TIMEOUT(bridge.execute("export_status",{{"job_id",job}})["status"].toString(),QString("done"),10000);
        auto available=ExportLease::tryAcquire();QVERIFY(available);
    }
    void closingDuringExportKeepsTheEventLoopResponsive() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path(),2048);
        MainWindow window(nullptr,dir.path()+"/presets");window.show();EditorAutomation bridge(&window,dir.path()+"/sessions");
        bridge.execute("open",{{"path",path}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        bridge.execute("save",{{"path",dir.path()+"/closing.vgsproj"}});
        const auto job=bridge.execute("export",{{"path",dir.path()+"/closing.vgs"}});
        QCOMPARE(job["status"].toString(),QString("running"));
        int confirmations=0;QTimer answer;answer.setInterval(5);
        connect(&answer,&QTimer::timeout,this,[&] {
            for (auto *box:window.findChildren<QMessageBox *>()) if (box->isVisible()) {++confirmations;box->button(QMessageBox::Yes)->click();}
        });answer.start();
        int ticks=0;QTimer heartbeat;heartbeat.setInterval(5);connect(&heartbeat,&QTimer::timeout,this,[&] {++ticks;});heartbeat.start();
        QElapsedTimer elapsed;elapsed.start();QVERIFY(!window.close());QVERIFY(elapsed.elapsed()<500);
        auto *progress=window.findChild<QProgressDialog *>();QVERIFY(progress);QVERIFY(progress->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(),10000);QVERIFY(ticks>0);
        QCOMPARE(confirmations,1);
        auto lease=ExportLease::tryAcquire();QVERIFY(lease);
    }
    void closingRechecksEditsMadeWhileExportIsStopping() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path(),2048);
        MainWindow window(nullptr,dir.path()+"/presets");window.show();EditorAutomation bridge(&window,dir.path()+"/sessions");
        QString error;QVERIFY(bridge.start(&error));
        bridge.execute("open",{{"path",path}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        bridge.execute("save",{{"path",dir.path()+"/closing.vgsproj"}});
        bridge.execute("export",{{"path",dir.path()+"/closing.vgs"}});
        bool askedToSave=false;QTimer answer;answer.setInterval(5);
        connect(&answer,&QTimer::timeout,this,[&] {
            for (auto *box:window.findChildren<QMessageBox *>()) if (box->isVisible()) {
                if (box->standardButtons().testFlag(QMessageBox::Save)) {askedToSave=true;box->button(QMessageBox::Cancel)->click();}
                else box->button(QMessageBox::Yes)->click();
            }
        });answer.start();
        QVERIFY(!window.close());
        // A late GUI/programmatic edit must not inherit approval to discard the old state.
        auto *size=window.findChild<QDoubleSpinBox *>("displayPointSize");QVERIFY(size);size->setValue(9);
        QTRY_VERIFY_WITH_TIMEOUT(askedToSave,10000);
        QVERIFY(window.isVisible());QVERIFY(window.isWindowModified());QVERIFY(bridge.isEnabled());
        QCOMPARE(bridge.execute("state",{})["point_size"].toDouble(),9.);
    }
    void decliningExportCancellationLeavesTheEditorOpen() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path(),2048);
        MainWindow window(nullptr,dir.path()+"/presets");window.show();EditorAutomation bridge(&window,dir.path()+"/sessions");
        bridge.execute("open",{{"path",path}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        bridge.execute("save",{{"path",dir.path()+"/keep-open.vgsproj"}});
        const QString id=bridge.execute("export",{{"path",dir.path()+"/keep-exporting.pgs"}})["job_id"].toString();
        QTimer answer;answer.setInterval(5);connect(&answer,&QTimer::timeout,this,[&] {
            for (auto *box:window.findChildren<QMessageBox *>()) if (box->isVisible()) box->button(QMessageBox::No)->click();
        });answer.start();
        QVERIFY(!window.close());QVERIFY(window.isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(bridge.execute("export_status",{{"job_id",id}})["status"].toString(),QString("done"),10000);
        QVERIFY(QFileInfo(dir.path()+"/keep-exporting.pgs").size()>0);
    }
    void completedExportsReleaseReservationsAndKeepBoundedHistory() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path());
        MainWindow window(nullptr,dir.path()+"/presets");EditorAutomation bridge(&window,dir.path()+"/sessions");
        auto *reaper=bridge.findChild<QTimer *>("mcpJobReaper");QVERIFY(reaper);QVERIFY(!reaper->isActive());
        bridge.execute("open",{{"path",path}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        QString oldest,newest;
        for (int i=0;i<34;++i) {
            const auto started=bridge.execute("export",{{"path",dir.path()+"/repeated.pgs"},{"overwrite",true}});
            newest=started["job_id"].toString();if (i==0) oldest=newest;
            QTRY_COMPARE_WITH_TIMEOUT(bridge.execute("export_status",{{"job_id",newest}})["status"].toString(),QString("done"),10000);
            auto lease=ExportLease::tryAcquire();QVERIFY(lease);
        }
        QCOMPARE(bridge.execute("export_status",{{"job_id",newest}})["status"].toString(),QString("done"));
        QVERIFY_EXCEPTION_THROWN(bridge.execute("export_status",{{"job_id",oldest}}),std::runtime_error);
        QTRY_VERIFY(!reaper->isActive());
    }
    void menuTogglePersistsAndClosesLiveConnections() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());
        {
            MainWindow window(nullptr,dir.path()+"/presets");EditorAutomation bridge(&window,dir.path()+"/sessions");
            auto *action=window.findChild<QAction *>("editMcp");QVERIFY(action);QVERIFY(action->isEnabled());
            QVERIFY(!window.mcpPreferredEnabled());QVERIFY(!action->isChecked());QVERIFY(!bridge.isEnabled());
            QVERIFY(!QDir(dir.path()+"/sessions").exists());
            action->trigger();QVERIFY(bridge.isEnabled());QVERIFY(action->isChecked());QVERIFY(window.mcpPreferredEnabled());
            QFile file(bridge.sessionFile());QVERIFY(file.open(QIODevice::ReadOnly));
            const auto before=QJsonDocument::fromJson(file.readAll()).object();file.close();
            const QString view=bridge.state()["view_id"].toString();
            QTcpSocket socket;socket.connectToHost("127.0.0.1",quint16(before["port"].toInt()));
            QVERIFY(socket.waitForConnected(2000));socket.write("{\"token\":");socket.flush();QTest::qWait(30);
            action->trigger();QVERIFY(!bridge.isEnabled());QVERIFY(!action->isChecked());QVERIFY(!window.mcpPreferredEnabled());
            QVERIFY(!QFile::exists(bridge.sessionFile()));QTRY_COMPARE(socket.state(),QAbstractSocket::UnconnectedState);
            QTcpSocket blocked;blocked.connectToHost("127.0.0.1",quint16(before["port"].toInt()));QVERIFY(!blocked.waitForConnected(2000));
            action->trigger();QVERIFY(bridge.isEnabled());QVERIFY(window.mcpPreferredEnabled());
            QVERIFY(file.open(QIODevice::ReadOnly));const auto after=QJsonDocument::fromJson(file.readAll()).object();file.close();
            QCOMPARE(after["session_id"],before["session_id"]);QVERIFY(after["token"]!=before["token"]);
            QVERIFY(bridge.state()["view_id"].toString()!=view);
            QTcpSocket resumed;resumed.connectToHost("127.0.0.1",quint16(after["port"].toInt()));QVERIFY(resumed.waitForConnected(2000));
            resumed.write(QJsonDocument(QJsonObject{{"token",after["token"]},{"method","state"},{"arguments",QJsonObject{}}}).toJson(QJsonDocument::Compact)+'\n');
            QTRY_VERIFY(resumed.canReadLine());QVERIFY(QJsonDocument::fromJson(resumed.readLine()).object()["ok"].toBool());
        }
        {
            MainWindow window(nullptr,dir.path()+"/presets");EditorAutomation bridge(&window,dir.path()+"/sessions");
            QVERIFY(window.mcpPreferredEnabled());QString error;QVERIFY2(bridge.start(&error),qPrintable(error));
            auto *action=window.findChild<QAction *>("editMcp");QVERIFY(action->isChecked());action->trigger();
            QVERIFY(!bridge.isEnabled());QVERIFY(!window.mcpPreferredEnabled());
        }
        MainWindow window(nullptr,dir.path()+"/presets");EditorAutomation bridge(&window,dir.path()+"/sessions");
        QVERIFY(!window.mcpPreferredEnabled());QVERIFY(!bridge.isEnabled());QVERIFY(!window.findChild<QAction *>("editMcp")->isChecked());
    }
    void displayOptionsAndCropEditingArePreviewOnly() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path());
        MainWindow window(nullptr,dir.path()+"/presets");EditorAutomation bridge(&window,dir.path()+"/sessions");
        // Commands run once the editor has settled, as the adapter does; a refusal fails the test.
        auto run=[&](const QString &method,const QJsonObject &arguments) -> QJsonObject {
            (void)QTest::qWaitFor([&] {return bridge.state()["ready"].toBool();},5000);
            try {return bridge.execute(method,arguments);} catch (const std::exception &e) {qWarning("%s: %s",qPrintable(method),e.what());return QJsonObject{{"refused",true}};}
        };
        bridge.execute("open",{{"path",path}});
        const bool modified=(run("state",{}),window.isWindowModified());
        auto display=[&] {return bridge.state()["display"].toObject();};
        QVERIFY(!run("display_options",{{"sh_degree",0},{"background","light"}}).contains("refused"));
        QCOMPARE(display()["sh_degree"].toInt(),0);QCOMPARE(display()["background"].toString(),QString("light"));
        QCOMPARE(window.findChild<QComboBox *>("displaySplatSh")->currentData().toInt(),0);
        QVERIFY(!run("display_options",{{"sh_degree","all"}}).contains("refused"));
        QCOMPARE(display()["sh_degree"].toString(),QString("all"));QCOMPARE(display()["background"].toString(),QString("light"));
        QVERIFY(run("display_options",{{"background","grey"}}).contains("refused"));
        QCOMPARE(window.isWindowModified(),modified);
        QString crop;for (const auto &m:run("state",{})["modifiers"].toArray()) if (m.toObject()["type"]=="crop") crop=m.toObject()["id"].toString();
        if (crop.isEmpty()) crop=run("add_modifier",{{"type","crop"}})["modifier_id"].toString();
        QVERIFY(!crop.isEmpty());
        QVERIFY(!run("add_modifier",{{"type","purge-isolated"}}).contains("refused"));
        QVERIFY(run("crop_editing",{{"enabled",true}}).contains("refused")); // Purge Isolated is selected
        QVERIFY(!run("select_modifier",{{"modifier_id",crop}}).contains("refused"));
        QVERIFY(run("crop_editing",{{"enabled",true}})["crop_editing"].toBool());
        QVERIFY(!run("crop_editing",{{"enabled",false}})["crop_editing"].toBool());
    }
    void inspectionRendersSurveyAndMotionWithoutMovingTheUsersView() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path());
        MainWindow window(nullptr,dir.path()+"/presets");window.resize(1000,700);window.show();EditorAutomation bridge(&window,dir.path()+"/sessions");
        bridge.execute("open",{{"path",path}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        const auto before=bridge.state();
        QJsonObject answer;bool answered=false;
        window.automationInspect({{"include_motion",true},{"show_removed",true},{"motion_frames",3}},[&](const QJsonObject &reply) {answer=reply;answered=true;});
        QTRY_VERIFY_WITH_TIMEOUT(answered,20000);
        QVERIFY2(answer["ok"].toBool(),qPrintable(answer["error"].toString()));
        const auto result=answer["result"].toObject();
        auto image=[&](const char *name) {return QImage::fromData(QByteArray::fromBase64(result[name].toString().toLatin1()),"PNG");};
        QCOMPARE(image("survey").size(),QSize(1080,810));
        QCOMPARE(image("motion").height(),420);QVERIFY(image("motion").width()>=2*280);
        QVERIFY(!image("current").isNull());
        const auto meta=result["meta"].toObject();
        QCOMPARE(meta["survey"].toObject()["cells"].toArray().size(),9);
        QVERIFY(meta["motion"].toObject()["pixels_per_metre"].toDouble()>0);
        QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        QCOMPARE(bridge.state()["frame"],before["frame"]);QCOMPARE(bridge.state()["camera"],before["camera"]);
        MainWindow::Inspection parts;parts.current=image("current");parts.survey=image("survey");parts.motion=image("motion");
        QVERIFY(window.composeInspection(parts).height()>810+420);
    }
    void guiInspectionExportClosesItsDialogsByItself() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path());
        MainWindow window(nullptr,dir.path()+"/presets");window.resize(1000,700);window.show();EditorAutomation bridge(&window,dir.path()+"/sessions");
        bridge.execute("open",{{"path",path}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        QStringList boxes;QTimer watch;watch.setInterval(5);
        connect(&watch,&QTimer::timeout,this,[&] {
            for (auto *box:window.findChildren<QMessageBox *>()) if (box->isVisible()) {boxes<<box->text();box->close();}
        });watch.start();
        // Asked for while a frame is still being decoded, as right after moving or playing.
        bridge.execute("seek",{{"frame",1}});QVERIFY(bridge.state()["processing"].toBool() || bridge.state()["ready"].toBool());
        const QString out=dir.path()+"/inspection.png";window.exportInspectionTo(out);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(out),20000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<QProgressDialog *>("inspectionProgress"),5000);
        QTest::qWait(200);
        for (auto *dialog:window.findChildren<QDialog *>()) QVERIFY2(!dialog->isVisible(),qPrintable(dialog->windowTitle()));
        QVERIFY2(boxes.isEmpty(),qPrintable(boxes.join("; ")));
    }
    void erasePicksShowAsChunkSpansThatSeekToTheirChunk() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path(),8); // one chunk per frame
        MainWindow window(nullptr,dir.path()+"/presets");window.resize(1200,800);window.show();EditorAutomation bridge(&window,dir.path()+"/sessions");
        bridge.execute("open",{{"path",path}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        bridge.execute("add_modifier",{{"type","erase"}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        bridge.execute("view",{{"preset","front"},{"focus",true}});
        bridge.execute("erase_editing",{{"enabled",true}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        bridge.execute("seek",{{"frame",5}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        const QJsonArray polygon{QJsonArray{0,0},QJsonArray{1,0},QJsonArray{1,1},QJsonArray{0,1}};
        QVERIFY(bridge.execute("stroke",{{"view_id",bridge.state()["view_id"]},{"tool","lasso"},{"mode","add"},{"points",polygon}})["picked_records"].toInt()>0);
        QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        bridge.execute("seek",{{"frame",1}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        auto *panel=window.findChild<ModifierPanel *>();QVERIFY(panel);auto *tree=panel->findChild<QTreeWidget *>();QVERIFY(tree);
        QTreeWidgetItem *erase=nullptr;for (int i=0;i<tree->topLevelItemCount();++i) if (tree->topLevelItem(i)->data(2,Qt::UserRole+3).toInt()==int(ModifierType::Erase)) erase=tree->topLevelItem(i);
        QVERIFY(erase);QCOMPARE(erase->data(2,Qt::UserRole+7).toList().size(),1);
        // Middle of frame 5 on the Erase bar: its chunk's span.
        const auto [left,width]=panel->trackSpan();const auto [first,last]=panel->view();
        const int x=tree->viewport()->mapFromGlobal(QPoint(int(left+width*(5.5-first)/(last-first)),0)).x();
        const QPoint at(x,tree->visualItemRect(erase).center().y());
        QHelpEvent help(QEvent::ToolTip,at,tree->viewport()->mapToGlobal(at));QApplication::sendEvent(tree->viewport(),&help);
        QTRY_VERIFY(QToolTip::isVisible());QVERIFY2(QToolTip::text().contains("Chunk 5"),qPrintable(QToolTip::text()));QToolTip::hideText();
        QTest::mouseClick(tree->viewport(),Qt::LeftButton,{},at);
        QTRY_COMPARE_WITH_TIMEOUT(bridge.state()["frame"].toInt(),5,5000);
        // Already in that chunk: a click selects the row and leaves the time where it is.
        QTest::mouseClick(tree->viewport(),Qt::LeftButton,{},at);QTest::qWait(100);QCOMPARE(bridge.state()["frame"].toInt(),5);
        // Elsewhere on the bar, with no picks, the time does not move either.
        const QPoint empty(tree->viewport()->mapFromGlobal(QPoint(int(left+width*(2.5-first)/(last-first)),0)).x(),at.y());
        QTest::mouseClick(tree->viewport(),Qt::LeftButton,{},empty);QTest::qWait(100);QCOMPARE(bridge.state()["frame"].toInt(),5);
    }
    void recentFilesAreListedWithoutALoadedCapture() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path());
        {QSettings settings;settings.setValue("RecentFiles",QStringList{path,dir.path()+"/gone.vgs"});settings.sync();}
        MainWindow window(nullptr,dir.path()+"/presets");EditorAutomation bridge(&window,dir.path()+"/sessions");
        const auto files=bridge.execute("recent_files",{})["files"].toArray();QCOMPARE(files.size(),2);
        QCOMPARE(QFileInfo(files[0]["path"].toString()),QFileInfo(path));QVERIFY(files[0]["exists"].toBool());
        QVERIFY(!files[1]["exists"].toBool());
    }
    void missingAdapterDisablesMcpAndKeepsThePreference() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());
        const QString adapter=dir.path()+"/mcp";QVERIFY(QDir().mkpath(adapter));
        QCOMPARE(EditorAutomation::missingAdapterFile(adapter),QString("server.py"));
        for (const char *name:{"server.py","http_transport.py","connections.py","workflows.md","tools.md"}) {
            QFile file(adapter+"/"+name);QVERIFY(file.open(QIODevice::WriteOnly));
        }
        QCOMPARE(EditorAutomation::missingAdapterFile(adapter),QString());          // a source build: no embedded Python
        QVERIFY(QDir().mkpath(adapter+"/python"));
        QCOMPARE(EditorAutomation::missingAdapterFile(adapter),QString("python/python.exe")); // a deployment missing its runtime
        MainWindow window(nullptr,dir.path()+"/presets");window.rememberMcpEnabled(true);
        EditorAutomation bridge(&window,dir.path()+"/sessions",dir.path()+"/absent");
        auto *action=window.findChild<QAction *>("editMcp");QVERIFY(action);
        QVERIFY(!bridge.adapterInstalled());QVERIFY(!action->isEnabled());QVERIFY(!action->isChecked());
        QVERIFY(action->text().contains("not installed"));QVERIFY(action->toolTip().contains("server.py"));
        QString error;QVERIFY(!bridge.start(&error));QVERIFY(error.contains("not installed"));QVERIFY(!bridge.isEnabled());
        QVERIFY(window.mcpPreferredEnabled()); // reinstalling with MCP restores the user's choice
    }
    void failedEnableLeavesConnectionAndPreferenceOff() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());
        const QString path=dir.path()+"/not-a-directory";QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.close();
        MainWindow window(nullptr,dir.path()+"/presets");EditorAutomation bridge(&window,path);
        auto *action=window.findChild<QAction *>("editMcp");QVERIFY(action);action->trigger();
        QVERIFY(!bridge.isEnabled());QVERIFY(!action->isChecked());QVERIFY(!window.mcpPreferredEnabled());
    }
    void discoveryAuthenticationAndFraming() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());MainWindow window(nullptr,dir.path()+"/presets");
        QString descriptorPath;
        {
            EditorAutomation bridge(&window,dir.path()+"/sessions");QString error;QVERIFY2(bridge.start(&error),qPrintable(error));
            descriptorPath=bridge.sessionFile();QFile file(descriptorPath);QVERIFY(file.open(QIODevice::ReadOnly));const auto d=QJsonDocument::fromJson(file.readAll()).object();
            QCOMPARE(d["host"].toString(),QString("127.0.0.1"));QVERIFY(d["token"].toString().size()>32);
            auto request=[&](QString token,bool split) {
                QTcpSocket socket;socket.connectToHost("127.0.0.1",quint16(d["port"].toInt()));
                if (!socket.waitForConnected(2000)) return QJsonObject();
                const auto bytes=QJsonDocument(QJsonObject{{"token",token},{"method","state"},{"arguments",QJsonObject{}}}).toJson(QJsonDocument::Compact)+'\n';
                if (split) {socket.write(bytes.left(8));socket.flush();QTest::qWait(10);socket.write(bytes.mid(8));}else socket.write(bytes);
                QElapsedTimer timer;timer.start();while (!socket.canReadLine() && timer.elapsed()<2000) QTest::qWait(10);
                return QJsonDocument::fromJson(socket.readLine()).object();
            };
            const auto bad=request("wrong-token",false);QCOMPARE(bad["ok"].toBool(),false);QVERIFY(bad["error"].toString().contains("Unauthorised"));
            const auto good=request(d["token"].toString(),true);QCOMPARE(good["ok"].toBool(),true);
            QCOMPARE(good["result"].toObject()["session_id"],d["session_id"]);
            QVERIFY_EXCEPTION_THROWN(bridge.execute("stroke",{}),std::runtime_error);
        }
        QVERIFY(!QFile::exists(descriptorPath));
    }
    void liveEditsValidationUndoAndChunkBoundaries() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path());
        MainWindow window(nullptr,dir.path()+"/presets");window.resize(1000,700);window.show();EditorAutomation bridge(&window,dir.path()+"/sessions");
        bridge.execute("open",{{"path",path}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        QCOMPARE(bridge.state()["chunk"].toInt(),0);QCOMPARE(bridge.state()["chunk_boundaries"].toArray().size(),3);
        QCOMPARE(bridge.state()["chunk_boundaries"].toArray()[0].toObject()["frame"].toDouble(),1.);
        QVERIFY_EXCEPTION_THROWN(bridge.execute("open",{{"path",path}}),std::runtime_error); // unsaved new project
        const auto added=bridge.execute("add_modifier",{{"type","purge-isolated"}});const QString id=added["modifier_id"].toString();QVERIFY(!id.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        const QJsonValue before=bridge.state()["modifiers"];
        QVERIFY_EXCEPTION_THROWN(bridge.execute("edit_modifier",{{"modifier_id",id},{"patch",QJsonObject{{"isolation",QJsonObject{{"neighbour",0}}}}}}),std::runtime_error);
        QCOMPARE(bridge.state()["modifiers"],before);
        QVERIFY_EXCEPTION_THROWN(bridge.execute("edit_modifier",{{"modifier_id",id},{"patch",QJsonObject{{"isolation",QJsonObject{{"neigbour",4}}}}}}),std::runtime_error);
        bridge.execute("edit_modifier",{{"modifier_id",id},{"patch",QJsonObject{{"isolation",QJsonObject{{"medianPercent",900}}}}}});
        QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        bridge.execute("undo",{});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);QCOMPARE(bridge.state()["modifiers"],before);
        bridge.execute("redo",{});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        bridge.execute("seek",{{"frame",2}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);QCOMPARE(bridge.state()["chunk"].toInt(),2);
        bridge.execute("save",{{"path",dir.path()+"/edited.vgsproj"}});QVERIFY(!bridge.state()["modified"].toBool());
        QVERIFY_EXCEPTION_THROWN(bridge.execute("open",{{"path",dir.path()+"/missing.pgs"},{"discard_unsaved",true}}),std::runtime_error);
    }
    void brushPicksAreChunkLocalAndStaleViewsAreRejected() {
        try {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path());
        MainWindow window(nullptr,dir.path()+"/presets");window.resize(1000,700);window.show();EditorAutomation bridge(&window,dir.path()+"/sessions");
        bridge.execute("open",{{"path",path}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        bridge.execute("display",{{"mode","points"},{"point_size",2}});
        bridge.execute("add_modifier",{{"type","erase"}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        bridge.execute("view",{{"preset","front"},{"focus",true}});
        bridge.execute("erase_editing",{{"enabled",true}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        const QJsonValue stale=bridge.state()["view_id"];
        bridge.execute("seek",{{"frame",2}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        const QJsonArray polygon{QJsonArray{0,0},QJsonArray{1,0},QJsonArray{1,1},QJsonArray{0,1}};
        QJsonObject stroke{{"view_id",stale},{"tool","lasso"},{"mode","add"},{"points",polygon}};
        QVERIFY_EXCEPTION_THROWN(bridge.execute("stroke",stroke),std::runtime_error);
        stroke["view_id"]=bridge.state()["view_id"];const auto result=bridge.execute("stroke",stroke);QVERIFY(result["picked_records"].toInt()>0);
        QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        const auto modifiers=bridge.state()["modifiers"].toArray();const auto picks=modifiers.last().toObject()["erase"].toObject()["chunks"].toArray();
        QCOMPARE(picks.size(),1);QCOMPARE(picks[0].toObject()["chunk"].toInt(),2);
        bridge.execute("undo",{});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        QVERIFY(bridge.state()["modifiers"].toArray().last().toObject()["erase"].toObject()["chunks"].toArray().isEmpty());
        QJsonObject brush{{"view_id",bridge.state()["view_id"]},{"tool","brush"},{"mode","add"},{"points",QJsonArray{QJsonValue(QJsonArray{.5,.5})}},{"radius",.25}};
        const auto brushed=bridge.execute("stroke",brush);QVERIFY(brushed["picked_records"].toInt()>0);
        QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        bridge.execute("clear_erased",{{"all_chunks",true}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        } catch (const std::exception &e) {QFAIL(e.what());}
    }
    void asynchronousExportUsesSnapshotAndReportsCompletion() {
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path());
        MainWindow window(nullptr,dir.path()+"/presets");EditorAutomation bridge(&window,dir.path()+"/sessions");
        bridge.execute("open",{{"path",path}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),5000);
        const QString output=dir.path()+"/result.pgs";
        const auto started=bridge.execute("export",{{"path",output}});const QString id=started["job_id"].toString();QVERIFY(!id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(bridge.execute("export_status",{{"job_id",id}})["status"].toString(),QString("done"),10000);
        QVERIFY(QFileInfo(output).size()>0);
        QVERIFY_EXCEPTION_THROWN(bridge.execute("export",{{"path",output}}),std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(bridge.execute("save",{{"path",path}}),std::runtime_error);
    }
    // A project's Loop morph as the viewport draws it, from the project's own camera: End, the
    // morph's frames at quarters, and Start, saved as PNGs in LOOP_MORPH_RENDERS, with the mean
    // brightness and saturation of the middle of the view for each.
    void loopMorphRenders() {
        const QString project=qEnvironmentVariable("LOOP_MORPH_PROJECT"),output=qEnvironmentVariable("LOOP_MORPH_RENDERS");
        if (project.isEmpty() || output.isEmpty()) QSKIP("Set LOOP_MORPH_PROJECT and LOOP_MORPH_RENDERS to render a project's Loop morph.");
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());QVERIFY(QDir().mkpath(output));
        MainWindow window(nullptr,dir.path()+"/presets");window.resize(1400,1000);window.show();
        EditorAutomation bridge(&window,dir.path()+"/sessions");auto *viewport=window.findChild<Viewport *>();QVERIFY(viewport);
        bridge.execute("open",{{"path",project}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),120000);
        bridge.execute("display",{{"mode","splats"}});
        const auto state=bridge.state();const double fps=state["fps"].toDouble();
        const auto range=bridge.execute("project",{})["timeline"].toObject();
        const int in=int(std::lround(range["in"].toDouble()*fps)),out=int(std::lround(range["out"].toDouble()*fps));
        int frames=0;for (const auto &m:bridge.execute("project",{})["modifiers"].toArray()) if (m.toObject()["type"]=="loop-morph" && m.toObject()["enabled"].toBool()) frames=std::max(1,int(std::lround(m.toObject()["morph"].toObject()["seconds"].toDouble()*fps)));
        QVERIFY(frames>0);
        const QVector<QPair<QString,int>> shots{{"0_end",out},{"1_quarter",out+std::max(1,(frames+1)/4)},{"2_half",out+std::max(1,(frames+1)/2)},{"3_three_quarters",out+std::max(1,3*(frames+1)/4)},{"4_start",in}};
        std::vector<QImage> strip;
        for (const auto &[name,frame]:shots) {
            bridge.execute("seek",{{"frame",frame}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),120000);
            QTest::qWait(200);const QImage image=viewport->grabFramebuffer();QVERIFY(!image.isNull());image.save(output+"/"+name+".png");
            // The middle of the view, where the project's camera was left on the subject.
            const QRect box(image.width()/5,image.height()/10,image.width()*3/5,image.height()*4/5);const QImage face=image.copy(box);
            strip.push_back(face);
            double light=0,saturation=0,sharp=0;int count=0;
            auto luma=[&](int x,int y) {const QColor c=face.pixelColor(x,y);return .2126*c.redF()+.7152*c.greenF()+.0722*c.blueF();};
            for (int y=1;y<face.height()-1;++y) for (int x=1;x<face.width()-1;++x) {
                const QColor c=face.pixelColor(x,y);const double hi=std::max({c.redF(),c.greenF(),c.blueF()}),lo=std::min({c.redF(),c.greenF(),c.blueF()});
                light+=luma(x,y);saturation+=hi>0 ? (hi-lo)/hi : 0;sharp+=std::abs(4*luma(x,y)-luma(x-1,y)-luma(x+1,y)-luma(x,y-1)-luma(x,y+1));++count;
            }
            qInfo("%s (frame %d): brightness %.4f, saturation %.4f, sharpness %.4f",qPrintable(name),frame,light/count,saturation/count,sharp/count);
        }
        QImage sheet(strip.front().width()*int(strip.size()),strip.front().height(),QImage::Format_RGB32);QPainter painter(&sheet);
        for (int i=0;i<int(strip.size());++i) painter.drawImage(i*strip.front().width(),0,strip[size_t(i)]);
        painter.end();sheet.save(output+"/faces.png");
    }
    // Placing markers on a project as a user does: Tab from Start, a click on the subject, and
    // on End the target where its pair probably is (LOOP_MORPH_PROJECT).
    void loopMorphSuggestsWhilePlacing() {
        const QString project=qEnvironmentVariable("LOOP_MORPH_PROJECT");if (project.isEmpty()) QSKIP("Set LOOP_MORPH_PROJECT.");
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());
        MainWindow window(nullptr,dir.path()+"/presets");window.resize(1400,1000);window.show();
        EditorAutomation bridge(&window,dir.path()+"/sessions");auto *viewport=window.findChild<Viewport *>();QVERIFY(viewport);
        bridge.execute("open",{{"path",project}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),120000);
        bridge.execute("display",{{"mode","splats"}});
        QString id;for (const auto &m:bridge.execute("project",{})["modifiers"].toArray()) if (m.toObject()["type"]=="loop-morph") id=m.toObject()["id"].toString();
        bridge.execute("select_modifier",{{"modifier_id",id}});QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),60000);
        viewport->setFocus();QTest::keyClick(viewport,Qt::Key_Tab);QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),60000);QTest::qWait(300);
        qInfo("after Tab: frame %d, picking %d, markers %d",bridge.state()["frame"].toInt(),int(viewport->pointPicking()),int(viewport->viewMarkers().size()));
        // A click on the middle of the face: marker on Start, then off to End.
        const QPoint at(viewport->width()/2,viewport->height()*2/5);QTest::mouseClick(viewport,Qt::LeftButton,Qt::NoModifier,at);
        QTRY_VERIFY_WITH_TIMEOUT(bridge.state()["ready"].toBool(),60000);QTest::qWait(500);
        int suggested=0;for (const auto &m:viewport->viewMarkers()) suggested+=m.suggested;
        qInfo("after the click: frame %d, picking %d, markers %d, suggested %d",bridge.state()["frame"].toInt(),int(viewport->pointPicking()),int(viewport->viewMarkers().size()),suggested);
        const auto morph=bridge.execute("loop_morph",{});qInfo("suggestion %s",QJsonDocument(morph["suggestion"].toObject()).toJson(QJsonDocument::Compact).constData());
        for (const auto &m:viewport->viewMarkers()) if (m.suggested) {QPointF p;const bool in=viewport->projectToView(m.world,&p);qInfo("target %d world %.3f %.3f %.3f -> %d (%.0f,%.0f) of %dx%d",m.number,m.world.x(),m.world.y(),m.world.z(),int(in),p.x(),p.y(),viewport->width(),viewport->height());}
        if (const QString shot=qEnvironmentVariable("LOOP_MORPH_RENDERS");!shot.isEmpty()) viewport->grabFramebuffer().save(shot+"/placing.png");
        QVERIFY(suggested==1);
        // The target is on the subject in view, where it was drawn first, and stays there once
        // the End frame has arrived and it is put on the splat nearest it.
        for (const auto &m:viewport->viewMarkers()) if (m.suggested) {QPointF p;QVERIFY(viewport->projectToView(m.world,&p));QVERIFY2(viewport->rect().contains(p.toPoint()),qPrintable(QString("%1,%2").arg(p.x()).arg(p.y())));}
    }
    void realMcpClientControlsTheNativeEditor() {
        const QString python=qEnvironmentVariable("VGS_MCP_TEST_PYTHON");
        if (python.isEmpty()) QSKIP("Set VGS_MCP_TEST_PYTHON to run the Python MCP client against this editor.");
        QTemporaryDir dir;QVERIFY(dir.isValid());settings(dir.path());const auto path=capture(dir.path());
        MainWindow window(nullptr,dir.path()+"/presets");window.resize(1000,700);window.show();
        EditorAutomation bridge(&window,dir.path()+"/sessions");QString error;QVERIFY2(bridge.start(&error),qPrintable(error));
        auto *viewport=window.findChild<Viewport *>();QVERIFY(viewport);
        // The test process may start hidden. A framebuffer read initializes the OpenGL
        // widget without requiring a visible interactive window.
        const QImage probe=viewport->grabFramebuffer();
        const bool images=viewport->isValid() && !probe.isNull();
        QString output=qEnvironmentVariable("VGS_MCP_TEST_OUTPUT");if (output.isEmpty()) output=dir.path()+"/results";
        const QString script=QString::fromUtf8(__FILE__).replace('\\','/').section('/',0,-3)+"/mcp/tests/live_smoke.py";
        for (const QString &transport:{QString("stdio"),QString("streamable-http")}) {
            const QString destination=output+"/"+transport;QVERIFY(QDir().mkpath(destination));
            QProcess process;process.start(python,{script,dir.path()+"/sessions",path,destination,images ? "yes" : "no",transport});
            QVERIFY(process.waitForStarted(5000));QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,60000);
            const auto diagnostic=process.readAllStandardOutput()+process.readAllStandardError();
            QVERIFY2(process.exitStatus()==QProcess::NormalExit && process.exitCode()==0,diagnostic.constData());
            qInfo("%s",diagnostic.constData());
        }
    }
};
int main(int argc,char **argv) {
    QSurfaceFormat format;format.setVersion(3,3);format.setProfile(QSurfaceFormat::CoreProfile);format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format);
    // Qt's own file dialogs: the tests find and dismiss them, and Windows' native one can fault
    // when it is closed while the shell is still building it.
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc,argv);app.setApplicationName("VGS Editor Tests");app.setOrganizationName("VGS");
    EditorTheme::install();AutomationTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "automation_tests.moc"
