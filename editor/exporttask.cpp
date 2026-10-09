// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#include "exporttask.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryFile>
#include <cmath>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
constexpr const char *Format = "vgs-editor-task";
constexpr int Version = 1;
QString relativeTo(const QString &task,const QString &path) {return QDir(QFileInfo(task).absolutePath()).relativeFilePath(path);}
QString resolved(const QString &task,const QString &absolute,const QString &relative) {
    if (!absolute.isEmpty() && QFileInfo::exists(absolute)) return absolute;
    if (!relative.isEmpty()) {
        const QString beside=QDir::cleanPath(QDir(QFileInfo(task).absolutePath()).absoluteFilePath(relative));
        if (QFileInfo::exists(beside)) return beside;
    }
    return absolute;
}
} // namespace

QString exportTaskPath(const QString &output) {
    const QFileInfo info(output);return info.absolutePath()+"/"+info.fileName()+".vgstask";
}
int exportFrameCount(const Project &project,double frameRate) {
    if (!(frameRate>0)) return 0;
    return std::max(1,int(std::lround(project.out*frameRate))-int(std::lround(project.in*frameRate))+1);
}

bool writeExportTask(const ExportTask &task,QString *error) {
    const QString output=QFileInfo(task.output).absoluteFilePath(),asset=QFileInfo(task.project.asset).absoluteFilePath();
    const QJsonObject root{{"format",Format},{"version",Version},
        {"created",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"output",output},{"outputRelative",relativeTo(task.path,output)},
        {"asset",asset},{"frames",task.frames},
        // The project as its own file would hold it, its capture relative to the task.
        {"project",task.project.json(task.path)},
        {"thumbnail",QString::fromLatin1(task.thumbnail.toBase64())}};
    QSaveFile file(task.path);const auto bytes=QJsonDocument(root).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) {*error=file.errorString();return false;}
    return true;
}

bool readExportTask(const QString &path,ExportTask *task,QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {*error=file.errorString();return false;}
    const auto document=QJsonDocument::fromJson(file.readAll());
    const auto root=document.object();
    if (!document.isObject() || root["format"]!=Format || !root["project"].isObject() || !root["output"].isString()) {
        *error=QStringLiteral("Not a VGS Editor task.");return false;
    }
    if (root["version"].toInt()!=Version) {*error=QStringLiteral("This task was made by a newer VGS Editor.");return false;}
    ExportTask result;result.path=QFileInfo(path).absoluteFilePath();
    result.thumbnail=QByteArray::fromBase64(root["thumbnail"].toString().toLatin1());
    if (!Project::fromJson(root["project"].toObject(),QFileInfo(path).absolutePath(),&result.project,error)) return false;
    result.project.asset=resolved(result.path,root["asset"].toString(),root["project"].toObject()["asset"].toString());
    result.output=resolved(result.path,root["output"].toString(),root["outputRelative"].toString());
    // A moved folder: no output exists yet, so follow the task when the old folder is gone.
    if (!QFileInfo(QFileInfo(result.output).absolutePath()).isDir() && !root["outputRelative"].toString().isEmpty())
        result.output=QDir::cleanPath(QDir(QFileInfo(path).absolutePath()).absoluteFilePath(root["outputRelative"].toString()));
    result.frames=std::max(0,root["frames"].toInt());
    *task=result;return true;
}

QString checkExportTask(const ExportTask &task) {
    if (!QFileInfo(task.project.asset).isFile()) return QStringLiteral("The capture is missing: %1").arg(QDir::toNativeSeparators(task.project.asset));
    const QString extension=QFileInfo(task.output).suffix().toLower();
    if (extension!="vgs" && extension!="pgs" && extension!="mint") return QStringLiteral("The output must be a .vgs, .pgs or .mint file.");
    if (QFileInfo(task.output).absoluteFilePath().compare(QFileInfo(task.project.asset).absoluteFilePath(),Qt::CaseInsensitive)==0)
        return QStringLiteral("The output would overwrite its own capture.");
    if (extension=="mint" && task.project.hasAnimatedMotion()) return QStringLiteral("MINT cannot store an animated transform. Export to .vgs or .pgs.");
    QString error;if (!task.project.captureSettings.validate(&error)) return error;
    // Writable: the folder exists (or can be made) and takes a file.
    const QString folder=QFileInfo(task.output).absolutePath();
    if (!QDir().mkpath(folder)) return QStringLiteral("The output folder cannot be created: %1").arg(QDir::toNativeSeparators(folder));
    QTemporaryFile probe(folder+"/.vgstask-check-XXXXXX");
    if (!probe.open()) return QStringLiteral("The output folder is not writable: %1").arg(QDir::toNativeSeparators(folder));
    return {};
}

ExportResult runExportTask(const ExportTask &task,const ExportProgress &progress) {
    const QString problem=checkExportTask(task);
    if (!problem.isEmpty()) throw std::runtime_error(problem.toStdString());
    return exportCaptureFile(task.project,task.output,progress,task.thumbnail);
}

StayAwake::StayAwake() {
#ifdef Q_OS_WIN
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
#endif
}
StayAwake::~StayAwake() {
#ifdef Q_OS_WIN
    SetThreadExecutionState(ES_CONTINUOUS);
#endif
}
