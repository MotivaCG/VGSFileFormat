// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#pragma once
#include "vgsframe.h"
#include <QString>
#include <QStringList>

// One 3D Gaussian Splatting PLY (the INRIA layout), read whole into a frame as the VGS
// editor's PLY import makes it (plysequence.cpp): positions, activated scales and opacity,
// base colour, rotation xyzw, f_rest as RGB per coefficient. Its normals come along when the
// file has any that are not zero, and its comments too (`comment capture_rig SMNV1`).
struct SgsPly {
    vgs::Frame frame; // frame.normal: the file's, or empty
    int shDegree = 0;
    QStringList comments;
    // The rest of the first comment that starts with `key` and a space, or empty.
    QString comment(const QString &key) const;
};
// Throws std::runtime_error with what is wrong with the file.
SgsPly readSgsPly(const QString &path);
