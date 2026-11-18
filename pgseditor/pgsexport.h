// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of PGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#pragma once
#include "pgscapture.h"
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <functional>

// The splats as a 3DGS .ply, with their normals in nx, ny, nz when they have them.
void exportPgsPly(const PgsCapture &, const QString &destination);

struct PgsExportOptions {
    int shDegree = -1;     // -1: the PLY's own
    QByteArray glb;        // the mesh carried with the splats (FORMAT.md, proxy mesh); empty for none
    QByteArray thumbnailJpeg;
    QString title, author, copyright;
    QJsonObject provenance; // into the second metadata block, with the editor's own
    bool uncompressed = false; // pages stored as they are: quicker to read, larger (the header says so)
};
struct PgsExportResult {
    quint64 bytes = 0;
    QStringList notes;
};
using PgsProgress = std::function<void(int percent, const QString &stage)>;

// A static capture, .pgs (FORMAT.md, "Static captures"): the VGS container with one frame of
// one interval, every splat alive for it, signed as the editor's exports are; with its
// normals, and the mesh as its proxy mesh extra when there is one. Read back and checked
// before it replaces the destination. Throws std::runtime_error.
PgsExportResult exportPgs(const PgsCapture &, const PgsExportOptions &, const QString &destination, const PgsProgress & = {});
