// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#pragma once
#include "exporttask.h"
#include <QDialog>
#include <QStringList>
#include <atomic>
#include <memory>
#include <QVector>
class QLabel;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QThread;

// Process tasks: a queue of .vgstask files run one after another. One bar follows the task
// being exported, the other the whole queue - by frames when every task says how many it
// exports, by task otherwise. A task that fails is reported and the queue goes on.
class TaskQueueDialog : public QDialog {
    Q_OBJECT
public:
    explicit TaskQueueDialog(QWidget *parent = nullptr);
    ~TaskQueueDialog() override;
    void addTasks(const QStringList &paths);
    void start();
    bool running() const { return job_ != nullptr; }
    QTreeWidget *list() const { return list_; }
protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    enum class State { Pending, Running, Done, Failed, Skipped, Cancelled };
    struct Entry { ExportTask task; State state = State::Pending; QString message; ExportResult result; double seconds = 0; };
    void runNext();
    void finished(const QString &failure);
    void refreshRow(int row);
    void refreshButtons();
    void showProgress(int percent, const QString &message);
    void writeLog();
    QTreeWidget *list_;
    QLabel *taskLabel_, *totalLabel_;
    QProgressBar *taskBar_, *totalBar_;
    QPushButton *add_, *remove_, *start_, *cancelTask_, *cancelAll_, *close_;
    QList<Entry> entries_;
    int current_ = -1;
    QThread *job_ = nullptr;
    std::atomic_bool stopTask_{false}, stopAll_{false};
    QVector<int> run_;          // the entries this run processes
    ExportResult jobResult_;    // written by the job before its thread finishes
    QString jobFailure_;
    std::unique_ptr<StayAwake> awake_;
    QString runStarted_;
};
