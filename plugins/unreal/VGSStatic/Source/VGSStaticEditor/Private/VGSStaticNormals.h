#pragma once

/*
 * Normals for a PLY that has none, worked out once, when it is imported.
 *
 * A PLY from SMNForge or the VGS editor already has its normals (nx, ny, nz), worked out from
 * the capture's own cameras; those are used as they are. One without them is worked out here,
 * in one of two ways:
 *
 * - With a studio rig (the PLY says which, `comment capture_rig SMNV1`, or the import is told):
 *   the VGS editor's estimate (editor/gaussiannormals.cpp, copied into the plugin when it is
 *   assembled), each splat's side decided by the rig's cameras that see it. The PLY has to be
 *   in that rig's frame (Y up, metres), as SMNForge's are.
 * - Without one (a PLY from anywhere else): as PGSR (Chen et al., "PGSR: Planar-based Gaussian
 *   Splatting", 2024) takes a splat's normal, its shortest axis; its side is decided on screen,
 *   facing the camera (the renderer turns such normals towards it, as PGSR does). A round splat
 *   has no shortest axis to speak of, and a flat one can lie across its surface, so where the
 *   neighbours agree on a direction a splat that is round, or far from it, takes theirs: the
 *   opacity- and flatness-weighted consensus of its nearest splats, within a share of the
 *   splats' size.
 *
 * Plain C++, as VGSStaticPly.h; the definitions are in VGSStaticNormals.inl, built with the
 * third-party code it uses (nanoflann, Eigen) by GaussianNormalsBuild.cpp, and by
 * tests/unreal_static.cpp.
 */

#include "VGSStaticPly.h"

#include <cstddef>
#include <vector>

namespace vgsstatic {

enum class NormalRig {
  SmnV1,        // the ScanMeNow rig
  The4DScanner, // the THE4DSCANNER rig
};

struct RigNormalsResult {
  size_t opaque = 0, seen = 0; // opaque splats, and those a camera sees
  double seenShare() const { return opaque ? double(seen) / double(opaque) : 0.0; }
};

/* The rig a PLY's `comment capture_rig` names, or false. */
bool rigFromComment(const Splats &s, NormalRig &rig);

/* The VGS editor's estimate with a rig's cameras: 3 floats per splat into `normals`, outward,
 * 0 where none could be found. */
RigNormalsResult rigNormals(const Splats &s, NormalRig rig, std::vector<float> &normals);

struct AxisNormalsSettings {
  int neighbours = 16;           // the nearest splats a consensus is taken over
  float radiusShare = 0.01f;     // ... no farther than this share of the splats' diagonal
  float flatRatio = 0.5f;        // flat: shortest scale below this share of the middle one
  float agreeCos = 0.6427876f;   // a flat splat keeps its axis within 50 degrees of its neighbours'
  float consensus = 0.6f;        // neighbours agree: their main direction holds this share of the weight
  int passes = 2;                // of the consensus above
  int smoothingPasses = 2;       // then averaged with the neighbours within smoothCos, as the VGS editor does
  float smoothCos = 0.8191520f;  // 35 degrees
};

struct AxisNormalsResult {
  size_t splats = 0, flat = 0, replaced = 0, roundKept = 0;
};

/* PGSR's normals, corrected by neighbours (above): 3 floats per splat into `normals`, unit
 * length, turned away from the splats' centre; the side is the renderer's to decide. */
AxisNormalsResult axisNormals(const Splats &s, std::vector<float> &normals, const AxisNormalsSettings &settings = AxisNormalsSettings());

} // namespace vgsstatic
