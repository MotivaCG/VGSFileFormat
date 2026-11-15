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

// How finely computeNormals tells which splats a camera sees: the side of the depth buffer it
// draws the opaque splats into (a view larger than that is scaled down), the pinholes it closes
// between them, and how far behind the nearest a splat still counts as seen, as a fraction of
// its distance (on top of twice its own size, up to 1 %). limitToDensity: no finer than the
// splats are dense, neighbours at most 2 * closePixels apart, for the closing to join them: a
// finer buffer of sparse splats is mostly holes, every splat seen from everywhere (on a
// 6000-splat sphere at 1024 pixels: 8 % of the normals right).
struct Visibility {
    int resolution = 320;
    int closePixels = 1;
    float tolerance = 0.01f;
    bool limitToDensity = false;
};

// A rig's cameras: SMNForge's values, kept equal so both tools agree.
inline constexpr Visibility kRigVisibility{};
// Cameras all around. Against gs2mesh's dense mesh of two captures (andreavictor, 127 000
// splats near it; the couple, 196 000), the side was wrong on 1.27 % / 1.39 % with 100 cameras
// and 1 % (3 cm at their distance: the back of fingers, ears and cloth edges passed as seen
// from the front and voted the wrong way); 0.44 % / 0.44 % with 0.1 %, and 0.40 % / 0.41 %
// with 256 cameras, in twice the time (1 s for 300 000 splats). 256 cameras at 1 % alone: no
// better than 100.
inline constexpr Visibility kSurroundingVisibility{320, 1, 0.001f};
// Finer, for when the time is spent on purpose (the SGS editor's Estimate normals): 0.34 % /
// 0.36 % in nine times as long. 1536 pixels: 0.34 % / 0.33 % in 30 times as long.
inline constexpr Visibility kFineVisibility{1024, 3, 0.0005f, true};
inline constexpr int kSurroundingViews = 256;

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
    size_t rigOpaqueSeen = 0; // estimateNormals: the opaque splats the rig's own cameras saw
    bool surrounding = false; // estimateNormals: decided with cameras all around (surroundingViews)
};

// Cameras for splats no studio rig captured (a PLY from elsewhere, or a capture that is not in
// its rig's frame): `count` of them spread evenly over a sphere around the opaque splats
// (Fibonacci), each seeing all of them in `resolution` pixels. Which way is up does not matter. With them,
// computeNormals decides each splat's side from the outside, as a rig would, so what lies
// inside a fold or between the legs is decided by its neighbours instead.
std::vector<View> surroundingViews(const Splats &splats, int count = kSurroundingViews,
                                   int resolution = kSurroundingVisibility.resolution);

// Writes a unit normal for every used splat into normals (3 per splat); others are left
// as they are. Direction: the shortest axis on flat splats, the neighbourhood plane on
// round ones. Side: towards the cameras that see the splat through a coarse depth
// buffer, then propagated to the splats no camera decided, then smoothed.
Result computeNormals(const Splats &splats, const std::vector<View> &views, float *normals,
                      const Visibility &visibility = kRigVisibility);

// computeNormals with a rig's cameras, or with `surrounding` cameras all around the splats when
// `rig` is empty or its cameras see less than half of the opaque splats (not that rig's capture,
// or not in its frame): there they would only guess the sides. The rig's cameras see as
// kRigVisibility; those all around as `around`.
Result estimateNormals(const Splats &splats, const std::vector<View> &rig, float *normals,
                       int surrounding = kSurroundingViews, const Visibility &around = kSurroundingVisibility);

} // namespace gaussiannormals
