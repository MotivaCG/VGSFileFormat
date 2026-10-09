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
#include "exportcapture.h"
#include <QString>

// A queued export, written as a .vgstask: a copy of the project as it was when the task was
// made - later edits to the project do not change it - and where its export goes.
struct ExportTask {
    QString path;        // the .vgstask itself
    QString output;      // destination: .vgs, .pgs or .mint
    Project project;
    int frames = 0;      // how many frames it exports; 0 when not known
    QByteArray thumbnail; // JPEG of the viewport when the task was made; may be empty
};

// Paths are kept absolute and also relative to the task, so a folder of tasks moved together
// with its captures and outputs still runs: whichever of the two exists is used.
bool writeExportTask(const ExportTask &task, QString *error);
// Where the task for an output is written: beside it, named after the whole file name with
// its extension, so result.vgs and result.pgs get tasks of their own.
QString exportTaskPath(const QString &output);
bool readExportTask(const QString &path, ExportTask *task, QString *error);
// The frames a project's Start/End range exports at a frame rate, as the export counts them.
int exportFrameCount(const Project &project, double frameRate);
// Why a task cannot run, or empty: its capture exists, its output can be written, and the
// export accepts the combination (an animated transform cannot go to MINT, say).
QString checkExportTask(const ExportTask &task);
// Exports it, overwriting whatever is at the output.
ExportResult runExportTask(const ExportTask &task, const ExportProgress &progress = {});

// Keeps the computer from sleeping while a queue runs; released when it goes out of scope.
class StayAwake {
public:
    StayAwake();
    ~StayAwake();
    StayAwake(const StayAwake &) = delete;
    StayAwake &operator=(const StayAwake &) = delete;
};
