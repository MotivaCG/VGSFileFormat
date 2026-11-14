// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#pragma once
#include <QString>
#include <QStringList>
#include <QVector3D>
#include <vector>

// The calibrated photos a splat capture was trained from, as gs2mesh takes them: an
// undistorted COLMAP model (cameras.bin, images.bin) and its images. gs2mesh uses them for
// stereo geometry, to tell the slits no photo sees, for the facial relief and for the
// albedo; without them it works from the splats alone.
struct ColmapModel {
    QString folder;        // what was chosen
    QString sparse, images; // where the model and its images were found in it
    int cameras = 0, photos = 0, missing = 0;
    bool undistorted = true; // every camera PINHOLE or SIMPLE_PINHOLE
    std::vector<QVector3D> centres; // each photo's camera centre
    QString problem;        // why gs2mesh cannot use it; empty when it can
    bool usable() const { return problem.isEmpty(); }
};

// Looks for a model in `folder`: the folder itself, undistorted/sparse (what COLMAP's
// undistorter writes beside the original, distorted sparse/), sparse/, sparse/0, then any folder
// below holding cameras.bin and images.bin, with images/ beside the model or a level or two up.
// The first one gs2mesh can use; else the first found, with its problem.
ColmapModel findColmap(const QString &folder);

// Whether the model's cameras look at the splats: an empty string when they stand around the
// splats' box, else what is wrong (the photos of another capture, or another scale).
QString colmapMatches(const ColmapModel &, const QVector3D &minimum, const QVector3D &maximum);

// A folder beside the PLY, or the PLY's own, that holds a usable model; empty when none.
QString suggestColmap(const QString &plyPath);
