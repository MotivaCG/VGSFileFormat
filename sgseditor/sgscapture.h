// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#pragma once
#include "captureworker.h"
#include "gaussiannormals.h"
#include "sgsply.h"
#include <QString>
#include <QVector3D>
#include <optional>

// Where the normals the SGS editor shows and exports came from.
enum class NormalsSource {
    None,        // none yet: the splats cannot be lit
    File,        // the PLY's own nx, ny, nz
    Rig,         // our estimate with a rig's cameras: no longer made (Surrounding instead)
    Surrounding  // our estimate, sides decided by cameras all around the splats
};
QString normalsSourceText(NormalsSource);

// The PLY the SGS editor has open: the splats as read, and their normals.
struct SgsCapture {
    QString path;
    vgs::Frame frame; // frame.normal: one per splat, or empty
    int shDegree = 0;
    QStringList comments;
    NormalsSource normals = NormalsSource::None;
    // The box of the visible splats, without the 1 % farthest along each axis (floaters).
    QVector3D minimum, maximum;
    size_t visible = 0;

    static SgsCapture load(const QString &path); // throws std::runtime_error
    // The rig the PLY names (`comment capture_rig SMNV1` or THE4DSCANNER), if any.
    std::optional<gaussiannormals::Rig> rig() const;
    // Our estimate (gaussiannormals, as the VGS editor's export makes them): the sides decided
    // by cameras all around the splats, whichever rig the PLY names. Replaces the normals;
    // `note`, when given, says what decided the sides. `fine`: the cameras see finer
    // (gaussiannormals::kFineVisibility, the editor's Fine), some 2.3 times slower.
    void estimateNormals(QString *note = nullptr, int surrounding = SurroundingViews, bool fine = true);
    // The same in two steps: the estimate, which only reads the capture (another thread may make
    // it while nothing changes the capture), and taking it.
    struct NormalsEstimate {
        std::vector<float> normals;
        gaussiannormals::Result result;
        int surrounding = 0;
        bool fine = false;
    };
    NormalsEstimate computeNormals(int surrounding = SurroundingViews, bool fine = true) const;
    void applyNormals(NormalsEstimate estimate, QString *note = nullptr);
    // The cameras all around, as the VGS editor's export.
    static constexpr int SurroundingViews = gaussiannormals::kSurroundingViews;
    // What the viewport draws: the splats and their normals.
    FramePtr renderFrame() const;
};
