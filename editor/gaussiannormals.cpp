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
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#if defined(_M_X64) || defined(__x86_64__)
#define GAUSSIANNORMALS_X86 1
#include <immintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define GAUSSIANNORMALS_AVX
#else
#include <cpuid.h>
#define GAUSSIANNORMALS_AVX __attribute__((target("avx")))
#endif
#endif

namespace gaussiannormals {
namespace {

// SMNForge's values (plygaussiansplatting.cpp), kept equal so both tools agree.
constexpr int kNormalNeighbors = 24;
constexpr float kRoundSplatRatio = 0.5f;
constexpr float kNormalOccluderOpacity = 0.05f;
constexpr int kNormalSignPropagationPasses = 4;
constexpr int kNormalSmoothingPasses = 4;
constexpr float kAxisTrustCos = 0.8660254f;        // cos 30
constexpr float kAxisDistrustCos = 0.5f;           // cos 60
// The smoothing weighs each neighbour by its opacity times cos^8 of its angle to the splat
// (0.76 at 15 degrees, 0.20 at 35, 0.004 at 60), so creases stay sharp. It
// replaced a hard cut that took every neighbour within 35 degrees and none beyond. On splat
// solids with known normals (scratch test, noise as on andreavictor) the error at a 40 degree
// crease went from 15.9 to 13.9 degrees, 60: 17.7 to 17.0, round 2 cm: 8.4 to 8.0; flat and
// round 12 cm unchanged (2.6, 3.0). Cos^4 smoothed flat parts more but rounded creases
// (60: 21.3); a weight falling with distance only helped very thin parts and cost elsewhere.
inline float smoothingWeight(float facing) {
    const float f2 = std::max(facing, 0.0f) * std::max(facing, 0.0f), f4 = f2 * f2;
    return f4 * f4;
}
// Not SMNForge's: outlier correction before the smoothing (see computeNormals), over the
// nearest neighbours only, and smoothing over a wider neighbourhood than the estimate's. On
// maria_simbo_carrito (THE4DSCANNER rig) splats more than 60 degrees from their neighbours'
// consensus went from 14 % to 8 %, beyond 30 degrees from 30 % to 18.5 %; export 15 % slower.
constexpr int kNormalOutlierPasses = 2;
constexpr int kOutlierNeighbors = 12;   // the nearest of the kNormalNeighbors, itself included
constexpr int kSmoothingNeighbors = 48;
constexpr float kOutlierMaxAngleCos = 0.6427876f;   // cos 50
constexpr float kOutlierConsensus = 0.7f;           // neighbours' mean resultant length
// Visibility::footprints: how far out a splat covers (its sigmas) and at most how many pixels.
constexpr float kFootprintSigmas = 2.0f;
constexpr int kFootprintMaxPixels = 24;

struct Cloud {
    std::vector<Eigen::Vector3f> pts;
    size_t kdtree_get_point_count() const { return pts.size(); }
    float kdtree_get_pt(size_t i, size_t d) const { return pts[i][int(d)]; }
    template <class Box> bool kdtree_get_bbox(Box &) const { return false; }
};

// The machine's cores, kept for one computeNormals: its parallel loops hand them blocks of
// work instead of starting threads of their own every time (several per camera).
class Pool {
public:
    explicit Pool(int threads) {
        for (int t = 1; t < threads; ++t) workers_.emplace_back([this] { serve(); });
    }
    ~Pool() {
        { std::lock_guard<std::mutex> lock(mutex_); quit_ = true; }
        wake_.notify_all();
        for (auto &w : workers_) w.join();
    }
    int size() const { return int(workers_.size()) + 1; }
    // job(k) for k in [0, jobs), on every thread including this one; returns when all are done.
    void run(int jobs, const std::function<void(int)> &job) {
        if (jobs <= 0) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = &job; jobs_ = jobs; next_ = 0; pending_ = jobs; ++generation_;
        }
        wake_.notify_all();
        work();
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return pending_ == 0; });
        job_ = nullptr;
    }
private:
    void work() {
        for (;;) {
            const int k = next_.fetch_add(1);
            if (k >= jobs_) return;
            (*job_)(k);
            if (pending_.fetch_sub(1) == 1) { std::lock_guard<std::mutex> lock(mutex_); done_.notify_all(); }
        }
    }
    void serve() {
        unsigned seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return quit_ || generation_ != seen; });
                if (quit_) return;
                seen = generation_;
            }
            work();
        }
    }
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    const std::function<void(int)> *job_ = nullptr;
    int jobs_ = 0;
    std::atomic<int> next_{0}, pending_{0};
    unsigned generation_ = 0;
    bool quit_ = false;
};
thread_local Pool *currentPool = nullptr;
// A splat's 2-sigma ellipse in one camera (Visibility::footprints); reachPx < 0: not drawn.
struct Footprint { float u, v, ia, ib, ic, np, z, reach; Eigen::Vector3f n; int reachPx; };
// The camera's intrinsics at the depth buffer's scale.
struct Intrinsics { float fx, fy, cx, cy; };

// Depths as integers that order as the floats do, so the nearest wins by an atomic minimum.
inline uint32_t orderedBits(float z) {
    uint32_t b;
    std::memcpy(&b, &z, 4);
    return (b & 0x80000000u) ? ~b : (b | 0x80000000u);
}
inline void atomicMin(std::atomic<uint32_t> &cell, uint32_t bits) {
    uint32_t seen = cell.load(std::memory_order_relaxed);
    while (bits < seen && !cell.compare_exchange_weak(seen, bits, std::memory_order_relaxed)) {}
}

// Row y of a footprint, pixels x0 to x1: those inside its ellipse take the depth of its plane
// along their ray, held within its reach of its centre.
inline void drawRow(const Footprint &f, const Intrinsics &k, int y, int x0, int x1, std::atomic<uint32_t> *row) {
    const float dy = y + 0.5f - f.v;
    for (int x = x0; x <= x1; ++x) {
        const float dx = x + 0.5f - f.u;
        if (f.ia * dx * dx + 2.0f * f.ib * dx * dy + f.ic * dy * dy > kFootprintSigmas * kFootprintSigmas) continue;
        const float nr = f.n.dot(Eigen::Vector3f((x + 0.5f - k.cx) / k.fx, (y + 0.5f - k.cy) / k.fy, 1.0f));
        const float z = std::clamp(std::abs(nr) > 0.2f ? f.np / nr : f.z, f.z - f.reach, f.z + f.reach);
        atomicMin(row[x], orderedBits(z));
    }
}

#ifdef GAUSSIANNORMALS_X86
// drawRow eight pixels at a time with AVX (no AVX2 needed), the same operations in the same order
// (no fused multiply-adds), so the depths are bit for bit the same.
GAUSSIANNORMALS_AVX void drawRowAvx(const Footprint &f, const Intrinsics &k, int y, int x0, int x1, std::atomic<uint32_t> *row) {
    const float dy = y + 0.5f - f.v;
    // Eigen's dot product adds as x + (y + z): y + z is the same along the row.
    const float dyTerm = f.ic * dy * dy, rayYZ = f.n.y() * ((y + 0.5f - k.cy) / k.fy) + f.n.z() * 1.0f;
    const __m256 half = _mm256_set1_ps(0.5f), u = _mm256_set1_ps(f.u), ia = _mm256_set1_ps(f.ia), ib2 = _mm256_set1_ps(2.0f * f.ib);
    const __m256 dyv = _mm256_set1_ps(dy), dyt = _mm256_set1_ps(dyTerm), limit = _mm256_set1_ps(kFootprintSigmas * kFootprintSigmas);
    const __m256 cx = _mm256_set1_ps(k.cx), fx = _mm256_set1_ps(k.fx), nx = _mm256_set1_ps(f.n.x()), ryz = _mm256_set1_ps(rayYZ);
    const __m256 np = _mm256_set1_ps(f.np), z0 = _mm256_set1_ps(f.z), lo = _mm256_set1_ps(f.z - f.reach), hi = _mm256_set1_ps(f.z + f.reach);
    const __m256 tilt = _mm256_set1_ps(0.2f), signless = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
    // Pixel columns as floats (exact: far below 2^24), which AVX compares without AVX2's integers.
    const __m256 lanes = _mm256_setr_ps(0, 1, 2, 3, 4, 5, 6, 7), last = _mm256_set1_ps(float(x1));
    alignas(32) float depths[8];
    for (int x = x0; x <= x1; x += 8) {
        const __m256 column = _mm256_add_ps(_mm256_set1_ps(float(x)), lanes);
        const __m256 xf = _mm256_add_ps(column, half);
        const __m256 dx = _mm256_sub_ps(xf, u);
        const __m256 q = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(_mm256_mul_ps(ia, dx), dx), _mm256_mul_ps(_mm256_mul_ps(ib2, dx), dyv)), dyt);
        const __m256 inside = _mm256_and_ps(_mm256_cmp_ps(q, limit, _CMP_NGT_UQ),
                                            _mm256_cmp_ps(column, last, _CMP_LE_OQ));
        const int mask = _mm256_movemask_ps(inside);
        if (!mask) continue;
        const __m256 nr = _mm256_add_ps(_mm256_mul_ps(nx, _mm256_div_ps(_mm256_sub_ps(xf, cx), fx)), ryz);
        const __m256 tilted = _mm256_cmp_ps(_mm256_and_ps(nr, signless), tilt, _CMP_GT_OQ);
        const __m256 v = _mm256_blendv_ps(z0, _mm256_div_ps(np, nr), tilted);
        const __m256 held = _mm256_blendv_ps(_mm256_blendv_ps(v, hi, _mm256_cmp_ps(hi, v, _CMP_LT_OQ)), lo, _mm256_cmp_ps(v, lo, _CMP_LT_OQ));
        _mm256_store_ps(depths, held);
        for (int lane = 0; lane < 8; ++lane)
            if (mask & (1 << lane)) atomicMin(row[x + lane], orderedBits(depths[lane]));
    }
    _mm256_zeroupper(); // back to the SSE code around it without the transition's penalty
}

// Whether this processor and system run AVX (checked once).
bool avx() {
    static const bool available = [] {
        if (const char *off = std::getenv("GAUSSIANNORMALS_NO_AVX")) if (*off && *off != '0') return false;
#if defined(_MSC_VER) && !defined(__clang__)
        int r[4];
        __cpuid(r, 1);
        if (!(r[2] & (1 << 27)) || !(r[2] & (1 << 28))) return false; // OSXSAVE, AVX
        return (_xgetbv(0) & 6) == 6;                                   // the system saves the registers
#else
        return __builtin_cpu_supports("avx") != 0;
#endif
    }();
    return available;
}
#else
bool avx() { return false; }
#endif

// Runs body(i) for i in [0, count) on the machine's cores, in contiguous blocks.
template <class Body> void parallelFor(int count, Body body) {
    const int threads = std::max(1, std::min<int>(currentPool ? currentPool->size() : int(std::thread::hardware_concurrency()), count / 4096));
    if (threads <= 1) {
        for (int i = 0; i < count; ++i) body(i);
        return;
    }
    const int block = (count + threads - 1) / threads;
    const std::function<void(int)> job = [&](int t) {
        for (int i = t * block, end = std::min(count, i + block); i < end; ++i) body(i);
    };
    if (currentPool) { currentPool->run(threads, job); return; }
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back(job, t);
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

std::vector<View> surroundingViews(const Splats &s, int count, int resolution) {
    // The opaque splats' extent, per axis between the 1st and 99th percentiles, so that a
    // few floaters far out do not shrink the subject in every camera.
    std::vector<float> axes[3];
    for (size_t i = 0; i < s.count; ++i) {
        if ((s.use && !s.use[i]) || !(s.opacity[i] > kNormalOccluderOpacity)) continue;
        const float *p = s.position + i * 3;
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
        for (int c = 0; c < 3; ++c) axes[c].push_back(p[c]);
    }
    if (axes[0].empty() || count <= 0) return {};
    Eigen::Vector3f lo, hi;
    for (int c = 0; c < 3; ++c) {
        auto &v = axes[c];
        const size_t a = v.size() / 100, b = v.size() - 1 - v.size() / 100;
        std::nth_element(v.begin(), v.begin() + ptrdiff_t(a), v.end());
        lo[c] = v[a];
        std::nth_element(v.begin(), v.begin() + ptrdiff_t(b), v.end());
        hi[c] = v[b];
    }
    const Eigen::Vector3f centre = 0.5f * (lo + hi);
    const float radius = std::max(0.5f * (hi - lo).norm(), 1e-4f);
    constexpr float kDistance = 3.0f; // in radii: a sphere of the subject's size fills ~40 degrees
    const int side = std::max(resolution, 16);
    const float focal = 0.5f * float(side) / std::tan(std::asin(1.0f / kDistance)) * 0.95f;

    std::vector<View> views;
    views.reserve(size_t(count));
    const float golden = 3.14159265f * (3.0f - std::sqrt(5.0f));
    for (int k = 0; k < count; ++k) {
        const float y = 1.0f - 2.0f * (float(k) + 0.5f) / float(count);
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const Eigen::Vector3f direction(r * std::cos(golden * float(k)), y, r * std::sin(golden * float(k)));
        const Eigen::Vector3f eye = centre + direction * (kDistance * radius);
        // Camera axes: z forward (to the centre), x right, y down.
        const Eigen::Vector3f forward = -direction;
        const Eigen::Vector3f helper = std::abs(forward.y()) < 0.9f ? Eigen::Vector3f::UnitY() : Eigen::Vector3f::UnitX();
        const Eigen::Vector3f right = helper.cross(forward).normalized();
        const Eigen::Vector3f down = forward.cross(right).normalized();
        View v;
        v.worldToCamera.row(0) = right;
        v.worldToCamera.row(1) = down;
        v.worldToCamera.row(2) = forward;
        v.translation = -v.worldToCamera * eye;
        v.fx = v.fy = focal;
        v.cx = v.cy = 0.5f * float(side);
        v.width = v.height = side;
        views.push_back(v);
    }
    return views;
}

Result computeNormals(const Splats &s, const std::vector<View> &views, float *normalsOut, const Visibility &visibility,
                      const std::function<void()> &poll) {
    Pool pool(std::max(1, int(std::thread::hardware_concurrency())));
    struct Current { Current(Pool *p) { currentPool = p; } ~Current() { currentPool = nullptr; } } current(&pool);
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
    // Each splat's covariance, for drawing its footprint (Visibility::footprints).
    std::vector<Eigen::Matrix3f> shape(visibility.footprints ? ids.size() : 0);
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
        if (!shape.empty()) shape[size_t(i)] = basis * sigma.cwiseProduct(sigma).asDiagonal() * basis.transpose();
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
    // The smoothing's wider neighbourhood (all of them, in a frame with fewer splats).
    const int smoothingNeighbors = std::min(kSmoothingNeighbors, count);
    std::vector<size_t> wideNeighbors(ids.size() * size_t(smoothingNeighbors));
    parallelFor(count, [&](int i) {
        float distances[kSmoothingNeighbors];
        nanoflann::KNNResultSet<float> found{size_t(smoothingNeighbors)};
        found.init(wideNeighbors.data() + size_t(i) * smoothingNeighbors, distances);
        tree.findNeighbors(found, cloud.pts[size_t(i)].data(), nanoflann::SearchParameters());
    });
    const auto smoothHood = [&](int i) { return wideNeighbors.data() + size_t(i) * smoothingNeighbors; };

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
    const bool probing = visibility.probes[0] > 0 || visibility.probes[1] > 0 || visibility.probes[2] > 0;
    std::vector<int> probeScore(probing ? ids.size() : 0, 0); // cameras that saw the outer point free, less the inner
    std::vector<float> depth(ids.size());
    // limitToDensity: how far apart the opaque splats are (the median distance to the nearest
    // other), and where they are, so that no view draws them finer than the closing can join.
    float spacing = 0.0f;
    Eigen::Vector3f middle = Eigen::Vector3f::Zero();
    if (visibility.limitToDensity) {
        std::vector<float> gaps;
        gaps.reserve(ids.size());
        for (int i = 0; i < count; ++i) {
            if (!(alpha[size_t(i)] > kNormalOccluderOpacity)) continue;
            const size_t *hood = neighborhood(i);
            for (int j = 0; j < kNormalNeighbors; ++j) {
                const float d = (cloud.pts[hood[j]] - cloud.pts[size_t(i)]).norm();
                if (d > 0.0f) { gaps.push_back(d); break; }
            }
            middle += cloud.pts[size_t(i)];
        }
        if (!gaps.empty()) {
            middle /= float(gaps.size());
            std::nth_element(gaps.begin(), gaps.begin() + ptrdiff_t(gaps.size() / 2), gaps.end());
            spacing = gaps[gaps.size() / 2];
        }
    }
    std::vector<Footprint> footprints; // each opaque splat's ellipse in the camera at hand
    std::vector<std::atomic<uint32_t>> depthBits; // the footprints' depth buffer, drawn from every thread
    for (const View &view : views) {
        if (poll) poll();
        if (view.width <= 0 || view.height <= 0 || view.fx <= 0.0f || view.fy <= 0.0f) continue;
        float scale = std::min(1.0f, float(visibility.resolution) / float(std::max(view.width, view.height)));
        if (spacing > 0.0f) {
            // Neighbours at most 2 * closePixels apart where the splats are: the closing joins them.
            const float z = (view.worldToCamera * middle + view.translation).z();
            if (z > 0.0f) scale = std::min(scale, 2.0f * float(std::max(1, visibility.closePixels)) * z / (view.fx * spacing));
        }
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
        std::vector<float> closed(size_t(width) * height, std::numeric_limits<float>::infinity());
        if (visibility.footprints) {
            // Each opaque splat over its 2-sigma ellipse, at the depth of its own plane along each ray (held within
            // twice its size of its centre): first each one's ellipse in this camera, then drawn.
            const float fx = view.fx * scale, fy = view.fy * scale, cx = view.cx * scale, cy = view.cy * scale;
            if (footprints.size() != size_t(count)) footprints.resize(size_t(count));
            parallelFor(count, [&](int i) {
                Footprint &f = footprints[size_t(i)];
                f.reachPx = -1;
                if (!(alpha[size_t(i)] > kNormalOccluderOpacity)) return;
                const Eigen::Vector3f p = view.worldToCamera * cloud.pts[size_t(i)] + view.translation;
                if (p.z() <= 1e-3f) return;
                Eigen::Matrix<float, 2, 3> J;
                J << fx / p.z(), 0, -fx * p.x() / (p.z() * p.z()), 0, fy / p.z(), -fy * p.y() / (p.z() * p.z());
                const Eigen::Matrix2f S = J * (view.worldToCamera * shape[size_t(i)] * view.worldToCamera.transpose()) * J.transpose()
                                        + 0.3f * Eigen::Matrix2f::Identity();
                const float det = S.determinant();
                if (!(det > 0.0f)) return;
                const float mid = 0.5f * (S(0, 0) + S(1, 1)), largest = mid + std::sqrt(std::max(0.0f, mid * mid - det));
                f.reachPx = std::min(kFootprintMaxPixels, int(std::ceil(kFootprintSigmas * std::sqrt(largest))));
                f.u = fx * p.x() / p.z() + cx; f.v = fy * p.y() / p.z() + cy;
                f.ia = S(1, 1) / det; f.ib = -S(0, 1) / det; f.ic = S(0, 0) / det;
                f.n = view.worldToCamera * axis[size_t(i)];
                f.np = f.n.dot(p); f.z = p.z(); f.reach = 2.0f * radius[size_t(i)];
            });
            // Drawn straight into one buffer from every thread, the nearest winning by an atomic
            // minimum whatever the order; with AVX, eight pixels of a row at a time.
            if (depthBits.size() < closed.size()) depthBits = std::vector<std::atomic<uint32_t>>(closed.size());
            const uint32_t far = orderedBits(std::numeric_limits<float>::infinity());
            parallelFor(int(closed.size()), [&](int k) { depthBits[size_t(k)].store(far, std::memory_order_relaxed); });
            const Intrinsics intrinsics{fx, fy, cx, cy};
            const bool wide = avx();
            parallelFor(count, [&](int i) {
                const Footprint &f = footprints[size_t(i)];
                if (f.reachPx < 0) return;
                const int x0 = std::max(0, int(f.u) - f.reachPx), x1 = std::min(width - 1, int(f.u) + f.reachPx);
                for (int y = std::max(0, int(f.v) - f.reachPx); y <= std::min(height - 1, int(f.v) + f.reachPx); ++y) {
                    std::atomic<uint32_t> *row = depthBits.data() + size_t(y) * width;
#ifdef GAUSSIANNORMALS_X86
                    if (wide) {drawRowAvx(f, intrinsics, y, x0, x1, row); continue;}
#endif
                    drawRow(f, intrinsics, y, x0, x1, row);
                }
            });
            parallelFor(int(closed.size()), [&](int k) {
                uint32_t b = depthBits[size_t(k)].load(std::memory_order_relaxed);
                b = (b & 0x80000000u) ? (b & 0x7fffffffu) : ~b;
                std::memcpy(&closed[size_t(k)], &b, 4);
            });
        } else {
            std::vector<float> nearest(size_t(width) * height, std::numeric_limits<float>::infinity());
            for (int i = 0; i < count; ++i)
                if (pixel[size_t(i)] >= 0 && alpha[size_t(i)] > kNormalOccluderOpacity)
                    nearest[size_t(pixel[size_t(i)])] = std::min(nearest[size_t(pixel[size_t(i)])], depth[size_t(i)]);
            // Close the pinholes left between point samples.
            parallelFor(height, [&](int y) {
                const int close = visibility.closePixels;
                for (int x = 0; x < width; ++x) {
                    float value = std::numeric_limits<float>::infinity();
                    for (int dy = -close; dy <= close; ++dy)
                        for (int dx = -close; dx <= close; ++dx) {
                            const int sx = x + dx, sy = y + dy;
                            if (sx >= 0 && sx < width && sy >= 0 && sy < height)
                                value = std::min(value, nearest[size_t(sy) * width + sx]);
                        }
                    closed[size_t(y) * width + x] = value;
                }
            });
        }
        parallelFor(count, [&](int i) {
            if (pixel[size_t(i)] < 0) return;
            const float z = depth[size_t(i)];
            const float tolerance = visibility.tolerance * z + std::min(2.0f * radius[size_t(i)], 0.01f * z);
            if (z > closed[size_t(pixel[size_t(i)])] + tolerance) return;
            votes[size_t(i)] += (centre - cloud.pts[size_t(i)]).normalized();
            ++viewCount[size_t(i)];
        });
        if (probing) parallelFor(count, [&](int i) {
            auto freeAt = [&](const Eigen::Vector3f &q) {
                const Eigen::Vector3f p = view.worldToCamera * q + view.translation;
                if (p.z() <= 1e-6f) return 0;
                const int u = int(std::floor((view.fx * p.x() / p.z() + view.cx) * scale));
                const int v = int(std::floor((view.fy * p.y() / p.z() + view.cy) * scale));
                if (u < 0 || u >= width || v < 0 || v >= height) return 0;
                return p.z() < closed[size_t(v) * width + u] ? 1 : 0;
            };
            const Eigen::Vector3f &n = normal[size_t(i)], &c = cloud.pts[size_t(i)];
            for (const float d : visibility.probes)
                if (d > 0) probeScore[size_t(i)] += freeAt(c + d * n) - freeAt(c - d * n);
        });
    }

    std::vector<uint8_t> resolved(ids.size(), 0);
    for (int i = 0; i < count; ++i) {
        if (probing && probeScore[size_t(i)] != 0) {
            if (probeScore[size_t(i)] < 0) normal[size_t(i)] = -normal[size_t(i)];
            resolved[size_t(i)] = 1;
            continue;
        }
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
    // Outliers first: the smoothing below hardly listens to neighbours beyond 35 degrees, so a
    // splat whose normal is far off - its axis across the surface, or its side turned - is
    // never corrected by it and shows lit as a dark speck. Where the neighbours agree among
    // themselves and the splat disagrees with them, it takes theirs.
    for (int pass = 0; pass < kNormalOutlierPasses; ++pass) {
        const std::vector<Eigen::Vector3f> previous = normal;
        parallelFor(count, [&](int i) {
            Eigen::Vector3f sum = Eigen::Vector3f::Zero();
            float weight = 0.0f;
            const size_t *hood = neighborhood(i);
            for (int j = 0; j < kOutlierNeighbors; ++j) {
                if (hood[j] == size_t(i)) continue;
                sum += alpha[hood[j]] * previous[hood[j]];
                weight += alpha[hood[j]];
            }
            if (weight <= 1e-6f) return;
            const float agreement = sum.norm() / weight; // 1 when the neighbours all agree
            if (agreement < kOutlierConsensus) return;
            const Eigen::Vector3f consensus = sum.normalized();
            if (consensus.dot(previous[size_t(i)]) < kOutlierMaxAngleCos) normal[size_t(i)] = consensus;
        });
    }
    // Smoothing, mostly among neighbours already facing the same way (smoothingWeight).
    for (int pass = 0; pass < kNormalSmoothingPasses; ++pass) {
        const std::vector<Eigen::Vector3f> previous = normal;
        parallelFor(count, [&](int i) {
            Eigen::Vector3f average = Eigen::Vector3f::Zero();
            const size_t *hood = smoothHood(i);
            for (int j = 0; j < smoothingNeighbors; ++j)
                average += alpha[hood[j]] * smoothingWeight(previous[hood[j]].dot(previous[size_t(i)])) * previous[hood[j]];
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

Result estimateNormals(const Splats &s, const std::vector<View> &rig, float *normalsOut, int surrounding,
                       const Visibility &around) {
    if (!rig.empty()) {
        Result r = computeNormals(s, rig, normalsOut);
        r.rigOpaqueSeen = r.opaqueSeen;
        if (!r.opaque || 2 * r.opaqueSeen >= r.opaque) return r;
        const size_t seen = r.opaqueSeen;
        r = computeNormals(s, surroundingViews(s, surrounding, around.resolution), normalsOut, around);
        r.rigOpaqueSeen = seen;
        r.surrounding = true;
        return r;
    }
    Result r = computeNormals(s, surroundingViews(s, surrounding, around.resolution), normalsOut, around);
    r.surrounding = true;
    return r;
}

} // namespace gaussiannormals
