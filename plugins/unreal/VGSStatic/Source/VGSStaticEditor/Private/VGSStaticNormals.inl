// Definitions of VGSStaticNormals.h. Included once, after gaussiannormals: by
// GaussianNormalsBuild.cpp in the plugin, by tests/unreal_static.cpp outside it.
#include "VGSStaticNormals.h"
#include "gaussiannormals.h"
#include "dependencies/nanoflann/nanoflann.hpp"

#include <Eigen/Dense>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace vgsstatic {

namespace normalsdetail {

struct Cloud {
  const float *positions = nullptr;
  size_t count = 0;
  size_t kdtree_get_point_count() const { return count; }
  float kdtree_get_pt(size_t i, size_t d) const { return positions[i * 3 + d]; }
  template <class Box> bool kdtree_get_bbox(Box &) const { return false; }
};
using Tree = nanoflann::KDTreeSingleIndexAdaptor<nanoflann::L2_Simple_Adaptor<float, Cloud>, Cloud, 3>;

inline Eigen::Matrix3f rotation(const float *wxyz) {
  return Eigen::Quaternionf(wxyz[0], wxyz[1], wxyz[2], wxyz[3]).normalized().toRotationMatrix();
}

} // namespace normalsdetail

bool rigFromComment(const Splats &s, NormalRig &rig) {
  std::string name = s.comment("capture_rig");
  for (char &c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
  if (name == "SMNV1" || name == "SMN01" || name == "SCANMENOW_V1") {
    rig = NormalRig::SmnV1;
    return true;
  }
  if (name == "THE4DSCANNER") {
    rig = NormalRig::The4DScanner;
    return true;
  }
  return false;
}

namespace {
// The splats as gaussiannormals takes them, and its estimate with `views`.
RigNormalsResult estimate(const Splats &s, const std::vector<gaussiannormals::View> *views, std::vector<float> &normals) {
  std::vector<float> rotations(s.count * 4); // xyzw, as gaussiannormals takes them
  std::vector<float> opacity(s.count);
  for (size_t i = 0; i < s.count; ++i) {
    const float *q = &s.rotations[i * 4]; // wxyz
    rotations[i * 4 + 0] = q[1];
    rotations[i * 4 + 1] = q[2];
    rotations[i * 4 + 2] = q[3];
    rotations[i * 4 + 3] = q[0];
    opacity[i] = s.radiance[i * 4 + 3];
  }
  gaussiannormals::Splats in;
  in.count = s.count;
  in.position = s.positions.data();
  in.rotation = rotations.data();
  in.scale = s.scales.data();
  in.opacity = opacity.data();
  normals.assign(s.count * 3, 0.0f);
  const gaussiannormals::Result r = gaussiannormals::computeNormals(in, views ? *views : gaussiannormals::surroundingViews(in), normals.data());
  RigNormalsResult result;
  result.opaque = r.opaque;
  result.seen = r.opaqueSeen;
  return result;
}
} // namespace

RigNormalsResult rigNormals(const Splats &s, NormalRig rig, std::vector<float> &normals) {
  const std::vector<gaussiannormals::View> views = gaussiannormals::rigViews(rig == NormalRig::SmnV1 ? gaussiannormals::Rig::SmnV1 : gaussiannormals::Rig::The4DScanner);
  return estimate(s, &views, normals);
}

RigNormalsResult surroundingNormals(const Splats &s, std::vector<float> &normals) {
  return estimate(s, nullptr, normals);
}

AxisNormalsResult axisNormals(const Splats &s, std::vector<float> &normals, const AxisNormalsSettings &settings) {
  using namespace normalsdetail;
  AxisNormalsResult result;
  result.splats = s.count;
  normals.assign(s.count * 3, 0.0f);
  if (s.count == 0) return result;

  // Each splat's shortest axis (PGSR's normal) and how flat it is: 1 for a disc, 0 for a ball.
  std::vector<Eigen::Vector3f> axis(s.count);
  std::vector<float> flatness(s.count), weight(s.count);
  Eigen::Vector3f lo = Eigen::Vector3f::Constant(std::numeric_limits<float>::max()), hi = -lo;
  for (size_t i = 0; i < s.count; ++i) {
    const float *sc = &s.scales[i * 3];
    int k = 0;
    for (int c = 1; c < 3; ++c)
      if (sc[c] < sc[k]) k = c;
    const float a = sc[(k + 1) % 3], b = sc[(k + 2) % 3];
    const float middle = std::max(std::min(a, b), 1e-12f);
    flatness[i] = std::max(0.0f, 1.0f - sc[k] / middle);
    axis[i] = rotation(&s.rotations[i * 4]).col(k);
    weight[i] = s.radiance[i * 4 + 3] * flatness[i] * flatness[i];
    const Eigen::Vector3f p(s.positions[i * 3], s.positions[i * 3 + 1], s.positions[i * 3 + 2]);
    lo = lo.cwiseMin(p);
    hi = hi.cwiseMax(p);
    result.flat += flatness[i] >= 1.0f - settings.flatRatio;
  }
  const float radius = std::max(settings.radiusShare * (hi - lo).norm(), 1e-6f);
  const float radius2 = radius * radius;
  const Eigen::Vector3f centre = 0.5f * (lo + hi);

  Cloud cloud;
  cloud.positions = s.positions.data();
  cloud.count = s.count;
  Tree tree(3, cloud, nanoflann::KDTreeSingleIndexAdaptorParams(16));
  tree.buildIndex();
  const size_t k = size_t(std::max(settings.neighbours, 1)) + 1; // itself included
  std::vector<std::vector<uint32_t>> neighbours(s.count);
  {
    std::vector<size_t> index(k);
    std::vector<float> distance(k);
    for (size_t i = 0; i < s.count; ++i) {
      nanoflann::KNNResultSet<float> found{k};
      found.init(index.data(), distance.data());
      tree.findNeighbors(found, &s.positions[i * 3], nanoflann::SearchParameters());
      for (size_t n = 0; n < found.size(); ++n)
        if (index[n] != i && distance[n] <= radius2) neighbours[i].push_back(uint32_t(index[n]));
    }
  }

  // Passes of the neighbours' consensus: the main direction of their axes, sign aside (the
  // largest eigenvector of the weighted sum of a a^T), and how much of the weight it holds.
  std::vector<Eigen::Vector3f> current = axis, next(s.count);
  std::vector<uint8_t> replaced(s.count, 0);
  for (int pass = 0; pass < settings.passes; ++pass) {
    for (size_t i = 0; i < s.count; ++i) {
      Eigen::Matrix3f t = Eigen::Matrix3f::Zero();
      float total = 0.0f;
      for (uint32_t j : neighbours[i]) {
        t += weight[j] * current[j] * current[j].transpose();
        total += weight[j];
      }
      next[i] = current[i];
      if (total <= 0.0f) continue;
      Eigen::SelfAdjointEigenSolver<Eigen::Matrix3f> eigen(t);
      const float share = eigen.eigenvalues()(2) / std::max(eigen.eigenvalues().sum(), 1e-20f);
      if (share < settings.consensus) continue; // the neighbours disagree: nothing better than its own
      const Eigen::Vector3f agreed = eigen.eigenvectors().col(2);
      const bool flat = flatness[i] >= 1.0f - settings.flatRatio;
      if (!flat || std::fabs(agreed.dot(axis[i])) < settings.agreeCos) {
        next[i] = agreed;
        replaced[i] = 1;
      }
    }
    current.swap(next);
  }

  // Then smoothed: each the main direction of itself and those of its neighbours within
  // smoothCos of it, so creases stay sharp (the VGS editor's smoothing before it weighed
  // neighbours by cos^8 instead).
  for (int pass = 0; pass < settings.smoothingPasses; ++pass) {
    for (size_t i = 0; i < s.count; ++i) {
      const float own = s.radiance[i * 4 + 3];
      Eigen::Matrix3f t = own * current[i] * current[i].transpose();
      for (uint32_t j : neighbours[i])
        if (std::fabs(current[j].dot(current[i])) >= settings.smoothCos) t += s.radiance[size_t(j) * 4 + 3] * current[j] * current[j].transpose();
      Eigen::SelfAdjointEigenSolver<Eigen::Matrix3f> eigen(t);
      next[i] = eigen.eigenvectors().col(2);
    }
    current.swap(next);
  }

  for (size_t i = 0; i < s.count; ++i) {
    Eigen::Vector3f n = current[i].normalized();
    const Eigen::Vector3f p(s.positions[i * 3], s.positions[i * 3 + 1], s.positions[i * 3 + 2]);
    if (n.dot(p - centre) < 0.0f) n = -n; // tidy only: the renderer decides the side on screen
    for (int c = 0; c < 3; ++c) normals[i * 3 + c] = n[c];
    result.replaced += replaced[i];
    result.roundKept += !replaced[i] && flatness[i] < 1.0f - settings.flatRatio;
  }
  return result;
}

} // namespace vgsstatic
