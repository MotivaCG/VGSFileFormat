// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Victor M. Feliz. All rights reserved.
#pragma once
#include "captureworker.h"
#include "exporttask.h"
#include <QObject>
#include <QTcpServer>
#include <QJsonObject>
#include <QSet>
#include <atomic>
#include <mutex>
#include <thread>

class MainWindow;
class QTcpSocket;
class QProgressDialog;
class QTimer;
enum class EditorCloseResult;
// Private authenticated loopback bridge. Only the Python adapter speaks MCP.
// Never compiled into the standalone viewer. All scene operations run on Qt's UI thread.
class EditorAutomation final : public QObject {
public:
    // `adapterDirectory` is where the MCP adapter assistants launch is installed; by default
    // the `mcp` folder beside the program. Without it the bridge cannot be enabled.
    explicit EditorAutomation(MainWindow *window, const QString &sessionDirectory = {}, const QString &adapterDirectory = {});
    ~EditorAutomation() override;
    bool start(QString *error);
    void stop();
    bool isEnabled() const { return server_.isListening(); }
    bool adapterInstalled() const { return adapterMissing_.isEmpty(); }
    // The first required adapter file absent from `directory`, or empty when it is complete:
    // its scripts, and the embedded Python when the adapter ships one (a deployed editor).
    static QString missingAdapterFile(const QString &directory);
    QString sessionFile() const { return sessionFile_; }
    QJsonObject state() const;
    QJsonObject execute(const QString &method, const QJsonObject &arguments);
private:
    struct Job {
        std::atomic<bool> cancel{false};
        std::atomic<bool> finished{false};
        std::mutex mutex;
        QJsonObject status;
        std::thread thread;
        std::unique_ptr<ExportLease> lease;
        quint64 sequence = 0;
        ~Job() { if (thread.joinable()) thread.join(); }
    };
    MainWindow *window_;
    QSet<QTcpSocket *> activeSockets_;
    QTcpServer server_;
    QString directory_, sessionFile_, sessionId_, token_, adapterMissing_;
    quint64 connectionRevision_ = 0;
    quint64 jobSequence_ = 0;
    bool closing_ = false, closeReady_ = false;
    bool resumeMcpAfterClose_ = false;
    QTimer *reaper_ = nullptr;
    QProgressDialog *closingDialog_ = nullptr;
    std::map<QString,std::shared_ptr<Job>> jobs_;
    QString connectionId() const {return sessionId_+":"+QString::number(connectionRevision_);}
    void acceptConnections();
    void updateAction();
    void reapJobs();
    EditorCloseResult closeWhenIdle();
};
