// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Victor M. Feliz. All rights reserved.
#include "editorautomation.h"
#include "mainwindow.h"
#include "exporttask.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace {
// Fixed-size credentials are compared without an early exit on differing characters.
bool tokenMatches(const QString &candidate, const QString &expected) {
    const QByteArray a=candidate.toUtf8(),b=expected.toUtf8();
    if (a.size()!=b.size() || b.isEmpty()) return false;
    volatile unsigned char difference=0;
    for (qsizetype i=0;i<b.size();++i) difference=static_cast<unsigned char>(difference | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    return difference==0;
}

[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
QString text(const QJsonObject &a,const char *key) {
    if (!a[key].isString() || a[key].toString().trimmed().isEmpty()) fail(QString("%1 must be a nonempty string.").arg(key));
    return a[key].toString();
}
bool flag(const QJsonObject &a,const char *key,bool fallback=false) {
    if (!a.contains(key)) return fallback;
    if (!a[key].isBool()) fail(QString("%1 must be a boolean.").arg(key));return a[key].toBool();
}
}

QString EditorAutomation::missingAdapterFile(const QString &directory) {
    const QDir dir(directory);
    for (const char *name:{"server.py","http_transport.py","connections.py","workflows.md","tools.md"})
        if (!QFileInfo(dir.filePath(name)).isFile()) return QString::fromLatin1(name);
    // A source build uses the developer's own environment; a deployed one ships python/.
    if (QFileInfo(dir.filePath("python")).isDir() && !QFileInfo(dir.filePath("python/python.exe")).isFile()) return "python/python.exe";
    return {};
}

EditorAutomation::EditorAutomation(MainWindow *window,const QString &directory,const QString &adapterDirectory)
    : QObject(window),window_(window),directory_(directory) {
    sessionId_=QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (directory_.isEmpty()) directory_=qEnvironmentVariable("VGS_EDITOR_MCP_DIR");
    if (directory_.isEmpty()) directory_=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/mcp";
    adapterMissing_=missingAdapterFile(adapterDirectory.isEmpty() ? QCoreApplication::applicationDirPath()+"/mcp" : adapterDirectory);
    connect(&server_,&QTcpServer::newConnection,this,&EditorAutomation::acceptConnections);
    connect(window_,&MainWindow::mcpEnabledRequested,this,[this](bool enabled) {
        if (enabled) {
            QString error;
            if (!start(&error)) {
                window_->statusBar()->showMessage(tr("Cannot enable MCP: %1").arg(error),8000);
                return;
            }
        } else {
            stop();window_->statusBar()->showMessage(tr("MCP disabled."),3000);
        }
        window_->rememberMcpEnabled(enabled);
    });
    window_->setMcpAvailable(adapterInstalled(),adapterInstalled() ? QString()
        : tr("MCP is not installed with this editor (%1 is missing). Reinstall with the MCP option.").arg("mcp/"+adapterMissing_));
    updateAction();
    window_->setMcpCloseHandler([this] {return closeWhenIdle();});
    connect(window_,&MainWindow::closeAborted,this,[this] {
        if (!closing_) return;
        closing_=closeReady_=false;
        if (closingDialog_) {closingDialog_->close();closingDialog_->deleteLater();closingDialog_=nullptr;}
        if (resumeMcpAfterClose_) {QString error;if (!start(&error)) window_->statusBar()->showMessage(error,8000);}
        reapJobs();
    });
    reaper_=new QTimer(this);reaper_->setObjectName("mcpJobReaper");reaper_->setInterval(100);
    connect(reaper_,&QTimer::timeout,this,[this] {
        reapJobs();
        if (!closing_ || closeReady_) return;
        for (const auto &[id,job]:jobs_) if (!job->finished) return;
        closeReady_=true;
        reaper_->stop();
        if (closingDialog_) {closingDialog_->close();closingDialog_->deleteLater();closingDialog_=nullptr;}
        QTimer::singleShot(0,window_,[window=window_] {window->close();});
    });
}
EditorAutomation::~EditorAutomation() {
    stop();window_->setMcpAvailable(false);window_->setMcpCloseHandler({});
    for (auto &[id,job]:jobs_) job->cancel=true;
    for (auto &[id,job]:jobs_) if (job->thread.joinable()) job->thread.join();
    jobs_.clear();
}
void EditorAutomation::updateAction() {
    window_->setMcpConnectionState(isEnabled());
}
void EditorAutomation::stop() {
    server_.close();
    while (auto *socket=server_.nextPendingConnection()) {socket->abort();socket->deleteLater();}
    const auto sockets=activeSockets_;
    for (auto *socket:sockets) {socket->abort();socket->deleteLater();}
    if (!sessionFile_.isEmpty()) QFile::remove(sessionFile_);
    token_.clear();updateAction();
}
bool EditorAutomation::start(QString *error) {
    if (closing_) {if (error) *error=tr("The editor is closing.");return false;}
    if (server_.isListening()) return true;
    if (!adapterInstalled()) {if (error) *error=tr("MCP is not installed (mcp/%1 is missing).").arg(adapterMissing_);return false;}
    auto failed=[this,error](const QString &message) {if (error) *error=message;stop();return false;};
    if (!QDir().mkpath(directory_)) return failed("Cannot create MCP session directory.");
    if (!server_.listen(QHostAddress::LocalHost,0)) return failed(server_.errorString());
    token_=QUuid::createUuid().toString(QUuid::WithoutBraces)+QUuid::createUuid().toString(QUuid::WithoutBraces);
    sessionFile_=QDir(directory_).absoluteFilePath(sessionId_+".json");
    QSaveFile file(sessionFile_);
    if (!file.open(QIODevice::WriteOnly)) return failed(file.errorString());
    file.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
    const QJsonObject descriptor{{"format","vgs-editor-automation"},{"version",1},{"session_id",sessionId_},
        {"pid",double(QCoreApplication::applicationPid())},{"host","127.0.0.1"},{"port",server_.serverPort()},
        {"token",token_},{"created",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    if (file.write(QJsonDocument(descriptor).toJson())<0 || !file.commit()) return failed(file.errorString());
    ++connectionRevision_;updateAction();
    window_->statusBar()->showMessage(tr("MCP enabled."),3000);
    return true;
}
void EditorAutomation::acceptConnections() {
    while (auto *socket=server_.nextPendingConnection()) {
        activeSockets_.insert(socket);
        connect(socket,&QObject::destroyed,this,[this,socket] {activeSockets_.remove(socket);});
        socket->setReadBufferSize(1024*1024+1);
        auto *timeout=new QTimer(socket);timeout->setSingleShot(true);timeout->start(15000);
        connect(timeout,&QTimer::timeout,socket,&QTcpSocket::abort);
        connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        connect(socket,&QTcpSocket::readyRead,this,[this,socket,timeout] {
            if (!isEnabled()) {socket->abort();return;}
            if (socket->property("handled").toBool()) return;
            if (socket->bytesAvailable()>1024*1024) {socket->abort();return;}
            if (!socket->canReadLine()) return;
            socket->setProperty("handled",true);timeout->stop();QJsonObject response;
            // An inspection answers once its frames have been visited; the connection waits.
            const QPointer<QTcpSocket> guarded(socket);
            auto send=[guarded](const QJsonObject &answer) {
                if (!guarded) return;
                guarded->write(QJsonDocument(answer).toJson(QJsonDocument::Compact)+'\n');guarded->disconnectFromHost();
            };
            try {
                QJsonParseError error;const auto doc=QJsonDocument::fromJson(socket->readLine(),&error);
                if (error.error!=QJsonParseError::NoError || !doc.isObject()) fail("Invalid JSON request.");
                const auto request=doc.object();
                if (!tokenMatches(request["token"].toString(),token_)) fail("Unauthorised connection.");
                if (!request["arguments"].isObject()) fail("arguments must be an object.");
                const QString method=text(request,"method");
                if (method=="inspect") {
                    if (closing_) fail("The editor is closing.");
                    window_->automationInspect(request["arguments"].toObject(),send);return;
                }
                response={{"ok",true},{"result",execute(method,request["arguments"].toObject())}};
            } catch (const std::exception &e) {response={{"ok",false},{"error",QString::fromUtf8(e.what())}};}
            send(response);
        });
    }
}

QJsonObject EditorAutomation::state() const {return window_->automationState(connectionId());}
void EditorAutomation::reapJobs() {
    for (const auto &[id,job]:jobs_) if (job->finished && job->thread.joinable()) job->thread.join();
    // Keep a bounded history for status polling, without retaining completed thread handles.
    while (jobs_.size()>32) {
        auto oldest=jobs_.end();
        for (auto it=jobs_.begin();it!=jobs_.end();++it)
            if (!it->second->thread.joinable() && it->second->finished
                    && (oldest==jobs_.end() || it->second->sequence<oldest->second->sequence)) oldest=it;
        if (oldest==jobs_.end()) break;
        jobs_.erase(oldest);
    }
    if (!closing_ && std::none_of(jobs_.begin(),jobs_.end(),[](const auto &entry) {return entry.second->thread.joinable();})) reaper_->stop();
}
EditorCloseResult EditorAutomation::closeWhenIdle() {
    reapJobs();
    bool busy=false;
    for (const auto &[id,job]:jobs_) if (!job->finished) busy=true;
    if (!busy) return EditorCloseResult::Ready;
    if (!closing_) {
        const auto answer=QMessageBox::question(window_,tr("Export in progress"),
            tr("An MCP export is still running. Cancel the export and close the editor?"),
            QMessageBox::Yes|QMessageBox::No,QMessageBox::No);
        if (answer!=QMessageBox::Yes) return EditorCloseResult::Cancelled;
        for (const auto &[id,job]:jobs_) if (!job->finished) job->cancel=true;
        resumeMcpAfterClose_=isEnabled();closing_=true;stop();reaper_->start();
        closingDialog_=new QProgressDialog(tr("Stopping export before closing..."),QString(),0,0,window_);
        closingDialog_->setWindowTitle(tr("Closing VGS Editor"));closingDialog_->setWindowModality(Qt::WindowModal);
        closingDialog_->setCancelButton(nullptr);closingDialog_->setWindowFlag(Qt::WindowCloseButtonHint,false);
        closingDialog_->setMinimumDuration(0);closingDialog_->show();
    }
    return EditorCloseResult::Waiting;
}
QJsonObject EditorAutomation::execute(const QString &method,const QJsonObject &a) {
    if (closing_) fail("The editor is closing.");
    reapJobs();
    if (method=="export_status" || method=="cancel_export") {
        const QString id=text(a,"job_id");const auto it=jobs_.find(id);
        if (it==jobs_.end()) fail("Unknown or expired export job (the latest 32 jobs are retained).");
        if (method=="cancel_export") it->second->cancel=true;
        std::lock_guard<std::mutex> lock(it->second->mutex);return it->second->status;
    }
    if (method!="export") return window_->automationCommand(method,a,connectionId());
    auto lease=ExportLease::tryAcquire();
    if (!lease) fail("An export is already running in the editor or task queue. Wait for it to finish.");
    const ExportTask task=window_->prepareAutomationExport(text(a,"path"),flag(a,"overwrite"));
    const QString id=QUuid::createUuid().toString(QUuid::WithoutBraces);auto job=std::make_shared<Job>();
    job->sequence=++jobSequence_;job->lease=std::move(lease);
    job->status={{"job_id",id},{"status","running"},{"percent",0},{"path",task.output}};jobs_[id]=job;
    try {
        job->thread=std::thread([job,task] {
            QJsonObject terminal;
            try {
                StayAwake awake;const auto result=runExportTask(task,[job](int percent,const QString &message) {
                    {std::lock_guard<std::mutex> lock(job->mutex);job->status["percent"]=percent;job->status["message"]=message;}return !job->cancel;
                });
                terminal={{"status","done"},{"percent",100},{"frames",result.frames},{"kept",double(result.kept)},{"removed",double(result.removed)}};
                QJsonArray notes;for (const auto &note:result.notes) notes.append(note);terminal["notes"]=notes;
            } catch (const std::exception &e) {
                terminal={{"status",job->cancel ? "cancelled" : "failed"},{"error",QString::fromUtf8(e.what())}};
            }
            job->lease.reset();
            std::lock_guard<std::mutex> lock(job->mutex);
            for (auto it=terminal.begin();it!=terminal.end();++it) job->status[it.key()]=it.value();
            job->finished=true;
        });
    } catch (...) {jobs_.erase(id);throw;}
    reaper_->start();
    std::lock_guard<std::mutex> lock(job->mutex);return job->status;
}
