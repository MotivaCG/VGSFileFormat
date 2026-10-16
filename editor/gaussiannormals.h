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
#include <Eigen/Dense>
#include <cstddef>
#include <cstdint>
#include <vector>

// Outward surface normals for Gaussian splats, from the studio rig that captured them.
// Ported from SMNForge's PlyGaussianSplatting::fillNormals (same constants and steps,
// without its rotation baking: VGS stores the normal beside the splat instead).
namespace gaussiannormals {

enum class Rig { SmnV1, The4DScanner };

// One camera: world to camera, pinhole. Lens distortion is ignored on purpose.
struct View {
    Eigen::Matrix3f worldToCamera = Eigen::Matrix3f::Identity();
    Eigen::Vector3f translation = Eigen::Vector3f::Zero();
    float fx = 0, fy = 0, cx = 0, cy = 0;
    int width = 0, height = 0;
};

// Every camera of a studio rig, in the capture frame (Y up, metres). See normalrigs.cpp.
std::vector<View> rigViews(Rig rig);

// The same cameras for points mapped by a similarity x -> linear * x + offset, where
// linear is a rotation times a positive uniform scale: perspective ignores the scale.
std::vector<View> transformedViews(const std::vector<View> &views, const Eigen::Matrix3f &linear,
                                   const Eigen::Vector3f &offset);

struct Splats {
    size_t count = 0;
    const float *position = nullptr; // 3 per splat
    const float *rotation = nullptr; // 4 per splat, xyzw
    const float *scale = nullptr;    // 3 per splat, activated
    const float *opacity = nullptr;  // 1 per splat, activated
    const uint8_t *use = nullptr;    // 1 per splat: non-zero for the splats to consider, or null
};

struct Result {
    size_t splats = 0, opaque = 0, opaqueSeen = 0, resolved = 0;
};

// Writes a unit normal for every used splat into normals (3 per splat); others are left
// as they are. Direction: the shortest axis on flat splats, the neighbourhood plane on
// round ones. Side: towards the cameras that see the splat through a coarse depth
// buffer, then propagated to the splats no camera decided, then smoothed.
Result computeNormals(const Splats &splats, const std::vector<View> &views, float *normals);

} // namespace gaussiannormals
