// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#include "gaussiannormals.h"
#include "dependencies/nanoflann/nanoflann.hpp"
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

namespace gaussiannormals {
namespace {

// SMNForge's values (plygaussiansplatting.cpp), kept equal so both tools agree.
constexpr int kNormalNeighbors = 24;
constexpr float kRoundSplatRatio = 0.5f;
constexpr int kNormalDepthResolution = 320;
constexpr int kNormalDepthClosePixels = 1;
constexpr float kNormalOccluderOpacity = 0.05f;
constexpr float kNormalDepthTolerance = 0.01f;
constexpr int kNormalSignPropagationPasses = 4;
constexpr int kNormalSmoothingPasses = 4;
constexpr float kAxisTrustCos = 0.8660254f;        // cos 30
constexpr float kAxisDistrustCos = 0.5f;           // cos 60
constexpr float kSmoothingMaxAngleCos = 0.8191520f; // cos 35

struct Cloud {
    std::vector<Eigen::Vector3f> pts;
    size_t kdtree_get_point_count() const { return pts.size(); }
    float kdtree_get_pt(size_t i, size_t d) const { return pts[i][int(d)]; }
    template <class Box> bool kdtree_get_bbox(Box &) const { return false; }
};

// Runs body(i) for i in [0, count) on the machine's cores, in contiguous blocks.
template <class Body> void parallelFor(int count, Body body) {
    const int threads = std::max(1, std::min<int>(int(std::thread::hardware_concurrency()), count / 4096));
    if (threads <= 1) {
        for (int i = 0; i < count; ++i) body(i);
        return;
    }
    std::vector<std::thread> pool;
    const int block = (count + threads - 1) / threads;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&, t] {
            for (int i = t * block, end = std::min(count, i + block); i < end; ++i) body(i);
        });
    for (auto &thread : pool) thread.join();
}

} // namespace

std::vector<View> transformedViews(const std::vector<View> &views, const Eigen::Matrix3f &linear,
                                   const Eigen::Vector3f &offset) {
    // A camera point is W (x - b)/k... with x = k R y + b; scaling the camera point by k
    // keeps every projection and every depth ratio, so the view becomes W R^T, k t - W R^T b.
    const float scale = std::cbrt(std::abs(linear.determinant()));
    const Eigen::Matrix3f rotation = linear / std::max(scale, 1e-20f);
    std::vector<View> out = views;
    for (auto &v : out) {
        v.worldToCamera = v.worldToCamera * rotation.transpose();
        v.translation = scale * v.translation - v.worldToCamera * offset;
    }
    return out;
}

Result computeNormals(const Splats &s, const std::vector<View> &views, float *normalsOut) {
    Result result;
    std::vector<size_t> ids;
    ids.reserve(s.count);
    for (size_t i = 0; i < s.count; ++i) {
        if (s.use && !s.use[i]) continue;
        const float *p = s.position + i * 3, *q = s.rotation + i * 4, *sc = s.scale + i * 3;
        bool finite = true;
        for (int c = 0; c < 3; ++c) finite = finite && std::isfinite(p[c]) && std::isfinite(sc[c]);
        for (int c = 0; c < 4; ++c) finite = finite && std::isfinite(q[c]);
        if (finite && q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] > std::numeric_limits<float>::epsilon())
            ids.push_back(i);
    }
    const int count = int(ids.size());
    result.splats = ids.size();
    if (count < kNormalNeighbors) {
        // Too few to have a neighbourhood: the shortest axis, unoriented, is all there is.
        for (size_t i : ids) {
            const float *q = s.rotation + i * 4, *sc = s.scale + i * 3;
            const int shortest = sc[0] <= sc[1] && sc[0] <= sc[2] ? 0 : sc[1] <= sc[2] ? 1 : 2;
            const Eigen::Vector3f n = Eigen::Quaternionf(q[3], q[0], q[1], q[2]).normalized().toRotationMatrix().col(shortest);
            for (int c = 0; c < 3; ++c) normalsOut[i * 3 + c] = n[c];
        }
        return result;
    }

    Cloud cloud;
    cloud.pts.resize(ids.size());
    std::vector<Eigen::Vector3f> axis(ids.size());
    std::vector<float> roundness(ids.size()), radius(ids.size()), alpha(ids.size());
    parallelFor(count, [&](int i) {
        const size_t id = ids[size_t(i)];
        const float *p = s.position + id * 3, *q = s.rotation + id * 4, *sc = s.scale + id * 3;
        const Eigen::Vector3f sigma(sc[0], sc[1], sc[2]);
        int order[3] = {0, 1, 2};
        std::sort(order, order + 3, [&](int a, int b) { return sigma[a] < sigma[b]; });
        const Eigen::Matrix3f basis = Eigen::Quaternionf(q[3], q[0], q[1], q[2]).normalized().toRotationMatrix();
        cloud.pts[size_t(i)] = Eigen::Vector3f(p[0], p[1], p[2]);
        axis[size_t(i)] = basis.col(order[0]);
        roundness[size_t(i)] = sigma[order[1]] > 0.0f ? sigma[order[0]] / sigma[order[1]] : 1.0f;
        radius[size_t(i)] = sigma[order[2]];
        alpha[size_t(i)] = std::clamp(s.opacity[id], 0.0f, 1.0f);
    });

    using KDTree = nanoflann::KDTreeSingleIndexAdaptor<nanoflann::L2_Simple_Adaptor<float, Cloud>, Cloud, 3>;
    KDTree tree(3, cloud, nanoflann::KDTreeSingleIndexAdaptorParams(10));
    tree.buildIndex();

    // Neighbourhoods include the splat itself.
    std::vector<size_t> neighbors(ids.size() * kNormalNeighbors);
    const auto neighborhood = [&](int i) { return neighbors.data() + size_t(i) * kNormalNeighbors; };
    parallelFor(count, [&](int i) {
        float distances[kNormalNeighbors];
        nanoflann::KNNResultSet<float> found(kNormalNeighbors);
        found.init(neighborhood(i), distances);
        tree.findNeighbors(found, cloud.pts[size_t(i)].data(), nanoflann::SearchParameters());
    });

    // Direction: own shortest axis on flat splats, local surface plane on round ones - and
    // on flat splats whose axis disagrees with a well-defined plane.
    std::vector<Eigen::Vector3f> normal(ids.size());
    parallelFor(count, [&](int i) {
        const size_t *hood = neighborhood(i);
        // Weighted by opacity: near-transparent splats say little about where the surface is.
        float weightSum = 0.0f;
        Eigen::Vector3f mean = Eigen::Vector3f::Zero();
        for (int j = 0; j < kNormalNeighbors; ++j) {
            mean += alpha[hood[j]] * cloud.pts[hood[j]];
            weightSum += alpha[hood[j]];
        }
        const bool weighted = weightSum > 1e-3f;
        if (!weighted) {
            mean = Eigen::Vector3f::Zero();
            for (int j = 0; j < kNormalNeighbors; ++j) mean += cloud.pts[hood[j]];
            weightSum = float(kNormalNeighbors);
        }
        mean /= weightSum;
        Eigen::Matrix3f covariance = Eigen::Matrix3f::Zero();
        for (int j = 0; j < kNormalNeighbors; ++j) {
            const Eigen::Vector3f d = cloud.pts[hood[j]] - mean;
            covariance += (weighted ? alpha[hood[j]] : 1.0f) * d * d.transpose();
        }
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix3f> solver;
        solver.computeDirect(covariance);
        Eigen::Vector3f surface = solver.eigenvectors().col(0);
        const Eigen::Vector3f eigenvalues = solver.eigenvalues();
        if (!surface.allFinite()) surface = axis[size_t(i)];
        if (surface.dot(axis[size_t(i)]) < 0.0f) surface = -surface;

        float ownWeight = std::clamp((kRoundSplatRatio + 0.2f - roundness[size_t(i)]) / 0.4f, 0.0f, 1.0f);
        // A flat splat is not necessarily tangent (gap fillers turned to a camera,
        // silhouettes): when its axis leaves a clearly planar neighbourhood, the plane wins.
        const float planarity = (eigenvalues[1] > 0.0f && surface.allFinite())
            ? std::clamp(1.0f - 2.0f * eigenvalues[0] / eigenvalues[1], 0.0f, 1.0f) : 0.0f;
        const float agreement = std::abs(surface.dot(axis[size_t(i)]));
        const float axisTrust = std::clamp((agreement - kAxisDistrustCos) / (kAxisTrustCos - kAxisDistrustCos), 0.0f, 1.0f);
        ownWeight *= 1.0f - planarity * (1.0f - axisTrust);
        normal[size_t(i)] = (ownWeight * axis[size_t(i)] + (1.0f - ownWeight) * surface).normalized();
    });

    // Side: towards the cameras that see the splat through a coarse depth buffer.
    std::vector<Eigen::Vector3f> votes(ids.size(), Eigen::Vector3f::Zero());
    std::vector<int> viewCount(ids.size(), 0), pixel(ids.size());
    std::vector<float> depth(ids.size());
    for (const View &view : views) {
        if (view.width <= 0 || view.height <= 0 || view.fx <= 0.0f || view.fy <= 0.0f) continue;
        const float scale = std::min(1.0f, float(kNormalDepthResolution) / float(std::max(view.width, view.height)));
        const int width = std::max(1, int(view.width * scale)), height = std::max(1, int(view.height * scale));
        const Eigen::Vector3f centre = -view.worldToCamera.transpose() * view.translation;
        parallelFor(count, [&](int i) {
            const Eigen::Vector3f p = view.worldToCamera * cloud.pts[size_t(i)] + view.translation;
            pixel[size_t(i)] = -1;
            depth[size_t(i)] = p.z();
            if (p.z() <= 1e-6f) return;
            const int u = int(std::floor((view.fx * p.x() / p.z() + view.cx) * scale));
            const int v = int(std::floor((view.fy * p.y() / p.z() + view.cy) * scale));
            if (u >= 0 && u < width && v >= 0 && v < height) pixel[size_t(i)] = v * width + u;
        });
        std::vector<float> nearest(size_t(width) * height, std::numeric_limits<float>::infinity());
        for (int i = 0; i < count; ++i)
            if (pixel[size_t(i)] >= 0 && alpha[size_t(i)] > kNormalOccluderOpacity)
                nearest[size_t(pixel[size_t(i)])] = std::min(nearest[size_t(pixel[size_t(i)])], depth[size_t(i)]);
        // Close the pinholes left between point samples.
        std::vector<float> closed(nearest.size(), std::numeric_limits<float>::infinity());
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                float value = std::numeric_limits<float>::infinity();
                for (int dy = -kNormalDepthClosePixels; dy <= kNormalDepthClosePixels; ++dy)
                    for (int dx = -kNormalDepthClosePixels; dx <= kNormalDepthClosePixels; ++dx) {
                        const int sx = x + dx, sy = y + dy;
                        if (sx >= 0 && sx < width && sy >= 0 && sy < height)
                            value = std::min(value, nearest[size_t(sy) * width + sx]);
                    }
                closed[size_t(y) * width + x] = value;
            }
        parallelFor(count, [&](int i) {
            if (pixel[size_t(i)] < 0) return;
            const float z = depth[size_t(i)];
            const float tolerance = kNormalDepthTolerance * z + std::min(2.0f * radius[size_t(i)], 0.01f * z);
            if (z > closed[size_t(pixel[size_t(i)])] + tolerance) return;
            votes[size_t(i)] += (centre - cloud.pts[size_t(i)]).normalized();
            ++viewCount[size_t(i)];
        });
    }

    std::vector<uint8_t> resolved(ids.size(), 0);
    for (int i = 0; i < count; ++i) {
        const float facing = normal[size_t(i)].dot(votes[size_t(i)]);
        if (viewCount[size_t(i)] > 0 && std::abs(facing) > 1e-6f) {
            if (facing < 0.0f) normal[size_t(i)] = -normal[size_t(i)];
            resolved[size_t(i)] = 1;
        }
    }
    // Splats no camera decided follow their decided neighbours.
    for (int pass = 0; pass < kNormalSignPropagationPasses; ++pass) {
        const std::vector<uint8_t> known = resolved;
        int changed = 0;
        for (int i = 0; i < count; ++i) {
            if (known[size_t(i)]) continue;
            Eigen::Vector3f field = Eigen::Vector3f::Zero();
            const size_t *hood = neighborhood(i);
            for (int j = 0; j < kNormalNeighbors; ++j)
                if (known[hood[j]]) field += alpha[hood[j]] * normal[hood[j]];
            const float facing = normal[size_t(i)].dot(field);
            if (std::abs(facing) > 1e-9f) {
                if (facing < 0.0f) normal[size_t(i)] = -normal[size_t(i)];
                resolved[size_t(i)] = 1;
                ++changed;
            }
        }
        if (changed == 0) break;
    }
    // Smoothing among neighbours already facing within 35 degrees, so creases stay sharp.
    for (int pass = 0; pass < kNormalSmoothingPasses; ++pass) {
        const std::vector<Eigen::Vector3f> previous = normal;
        parallelFor(count, [&](int i) {
            Eigen::Vector3f average = Eigen::Vector3f::Zero();
            const size_t *hood = neighborhood(i);
            for (int j = 0; j < kNormalNeighbors; ++j)
                if (previous[hood[j]].dot(previous[size_t(i)]) >= kSmoothingMaxAngleCos)
                    average += alpha[hood[j]] * previous[hood[j]];
            if (average.squaredNorm() <= 1e-12f) return;
            average.normalize();
            if (average.dot(previous[size_t(i)]) < 0.0f) average = -average;
            normal[size_t(i)] = (previous[size_t(i)] + average).normalized();
        });
    }

    for (int i = 0; i < count; ++i) {
        for (int c = 0; c < 3; ++c) normalsOut[ids[size_t(i)] * 3 + c] = normal[size_t(i)][c];
        const bool opaque = alpha[size_t(i)] > kNormalOccluderOpacity;
        result.opaque += opaque ? 1 : 0;
        result.opaqueSeen += opaque && viewCount[size_t(i)] > 0 ? 1 : 0;
        result.resolved += resolved[size_t(i)] ? 1 : 0;
    }
    return result;
}

} // namespace gaussiannormals
