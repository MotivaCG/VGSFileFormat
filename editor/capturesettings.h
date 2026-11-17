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
#include <QJsonObject>
#include <QStringList>

struct CaptureSettings {
    QString catalogueId,title,author = "SMN|The4DSCanner",projectName,takeName,studio = "Valladolid",copyright;
    QString softwareName = "VGS Editor",softwareVersion = "0.1",extraJson;
    QStringList tags;
    // plain: uncompressed (.pgs). Exports set it from the output file's extension; it is
    // only read from older projects and presets, never chosen in the editor.
    bool plain = false,despill = false,recoverSkin = true;
    // Outward normals in the exported .vgs or .ply, their sides decided by cameras all around the
    // splats: NormalsNone or NormalsFine (gaussiannormals::kFineVisibility, about 3 s per 300 000-
    // splat frame), the editor's Write normals. NormalsNormal (kSurroundingVisibility, 1.3 s) is no
    // longer offered, as Fine costs so little more: it names normals earlier exports made that way,
    // and a project that asked for it now gets Fine. MINT cannot hold them.
    enum { NormalsNone = 0, NormalsNormal = 1, NormalsFine = 2 };
    int normals = NormalsFine;
    int shDegree = -1,playbackMode = -1; // -1 preserves the source setting.
    // Lossy SH reduction per splat at VGS/PGS export (EncodeOptions::shPruneTolerance):
    // 0 off, otherwise the RMS a splat's dropped bands may differ by.
    double shPrune = 0;
    double plyFps = 30;
    int plyRowOffset = 0;
    double despillStrength = 1,greenGain = 0.97,viewChromaScale = 0.5;
    QJsonObject json() const;
    bool validate(QString *error) const;
    static bool fromJson(const QJsonObject &json,CaptureSettings *settings,QString *error);
};
