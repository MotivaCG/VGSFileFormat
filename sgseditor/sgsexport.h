// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#pragma once
#include "sgscapture.h"
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <functional>

// The splats as a 3DGS .ply, with their normals in nx, ny, nz when they have them.
void exportSgsPly(const SgsCapture &, const QString &destination);

struct SgsExportOptions {
    int shDegree = -1;     // -1: the PLY's own
    QByteArray glb;        // the mesh carried with the splats (FORMAT.md, proxy mesh); empty for none
    QByteArray thumbnailJpeg;
    QString title, author, copyright;
    QJsonObject provenance; // into the second metadata block, with the editor's own
};
struct SgsExportResult {
    quint64 bytes = 0;
    QStringList notes;
};
using SgsProgress = std::function<void(int percent, const QString &stage)>;

// A static capture, .sgs (FORMAT.md, "Static captures"): the VGS container with one frame of
// one interval, every splat alive for it, signed as the editor's exports are; with its
// normals, and the mesh as its proxy mesh extra when there is one. Read back and checked
// before it replaces the destination. Throws std::runtime_error.
SgsExportResult exportSgs(const SgsCapture &, const SgsExportOptions &, const QString &destination, const SgsProgress & = {});
