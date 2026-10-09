#include "pruning.h"
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
constexpr int Size = 256;    // pixels per side of each scoring view
constexpr int Instants = 4;  // instants per chunk
constexpr double Pi = 3.14159265358979323846;
const double HalfFov = 22.5 * Pi / 180; // the editor's 45 degree vertical field of view

struct View { Eigen::Vector3d eye, right, up, forward; };
View look(const Eigen::Vector3d &eye, const Eigen::Vector3d &target) {
    View v; v.eye = eye; v.forward = (target - eye).normalized();
    const Eigen::Vector3d hint = std::abs(v.forward.y()) > 0.99 ? Eigen::Vector3d(0, 0, 1) : Eigen::Vector3d(0, 1, 0);
    v.right = v.forward.cross(hint).normalized(); v.up = v.right.cross(v.forward);
    return v;
}
struct Item { float depth, x, y, a, b, c, opacity; uint32_t index; int x0, x1, y0, y1; };

// One view of one instant, drawn as the viewport draws splats (EWA footprint with the 0.3 px
// low-pass, and its opacity compensation for captures trained with anti-aliasing), front to
// back. Each record is credited with its blending weight summed over the pixels it touches.
void render(const vgs::Frame &f, const View &v, double focal, bool antialiased,
            std::vector<Item> &items, std::vector<float> &transmittance, std::vector<double> &credit) {
    items.clear();
    Eigen::Matrix3d world;
    world.row(0) = v.right.transpose(); world.row(1) = v.up.transpose(); world.row(2) = v.forward.transpose();
    for (size_t i = 0; i < f.count; ++i) {
        if (!f.active[i]) continue;
        const Eigen::Vector3d d = Eigen::Vector3d(f.position[i*3], f.position[i*3+1], f.position[i*3+2]) - v.eye;
        const double z = d.dot(v.forward); if (z < 0.05) continue;
        const double x = d.dot(v.right), y = d.dot(v.up);
        const float *q = f.rotation.data() + i*4;
        Eigen::Quaterniond rotation(q[3], q[0], q[1], q[2]);
        const double norm = rotation.norm(); if (!(norm > 1e-12)) continue;
        rotation.coeffs() /= norm;
        Eigen::Matrix<double, 2, 3> jacobian;
        jacobian << focal/z, 0, -focal*x/(z*z), 0, focal/z, -focal*y/(z*z);
        const Eigen::Matrix<double, 2, 3> axes = jacobian * world * rotation.toRotationMatrix();
        Eigen::Matrix2d raw = Eigen::Matrix2d::Zero();
        for (int k = 0; k < 3; ++k) { const double s = f.scale[i*3+k]; raw += s*s * axes.col(k) * axes.col(k).transpose(); }
        Eigen::Matrix2d cov = raw; cov(0, 0) += 0.3; cov(1, 1) += 0.3;
        const double det = cov(0, 0)*cov(1, 1) - cov(0, 1)*cov(1, 0); if (!(det > 0)) continue;
        const double aa = antialiased ? std::sqrt(std::max(0.0, (raw(0, 0)*raw(1, 1) - raw(0, 1)*raw(1, 0)) / det)) : 1.0;
        const double opacity = f.opacity[i] * aa; if (!(opacity >= 1.0/255)) continue;
        const double sx = Size/2.0 + focal*x/z, sy = Size/2.0 - focal*y/z, extent = 3*std::sqrt(std::max(cov(0, 0), cov(1, 1)));
        const int x0 = std::max(0, int(sx - extent)), x1 = std::min(Size - 1, int(sx + extent));
        const int y0 = std::max(0, int(sy - extent)), y1 = std::min(Size - 1, int(sy + extent));
        if (x0 > x1 || y0 > y1) continue;
        items.push_back({float(z), float(sx), float(sy), float(cov(1, 1)/det), float(-cov(0, 1)/det), float(cov(0, 0)/det),
                         float(opacity), uint32_t(i), x0, x1, y0, y1});
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.depth < b.depth; });
    std::fill(transmittance.begin(), transmittance.end(), 1.f);
    for (const auto &it : items) {
        double given = 0;
        for (int py = it.y0; py <= it.y1; ++py) {
            float *row = transmittance.data() + size_t(py)*Size; const float dy = py + 0.5f - it.y;
            for (int px = it.x0; px <= it.x1; ++px) {
                float &t = row[px]; if (t < 1e-4f) continue;
                const float dx = px + 0.5f - it.x, power = -0.5f*(it.a*dx*dx + 2*it.b*dx*dy + it.c*dy*dy);
                if (power > 0) continue;
                const float alpha = std::min(0.99f, it.opacity*std::exp(power)); if (alpha < 1.f/255) continue;
                given += alpha*t; t *= 1 - alpha;
            }
        }
        credit[it.index] += given;
    }
}
}

std::vector<double> pruneSampleTimes(double start, double end) {
    std::vector<double> times;
    for (int k = 0; k < Instants; ++k) times.push_back(start + (k + 0.5) / Instants * (end - start));
    return times;
}

std::vector<float> contributionScores(const std::vector<vgs::Frame> &samples, bool antialiased, const std::function<bool()> &cancelled) {
    if (samples.empty()) return {};
    const size_t n = samples.front().count;
    for (const auto &s : samples) if (s.count != n) throw std::runtime_error("The frames of a chunk disagree on its records.");
    // Where the capture is: its live positions over the instants.
    Eigen::Vector3d lo = Eigen::Vector3d::Constant(std::numeric_limits<double>::max()), hi = -lo;
    std::vector<int> alive(n, 0); bool any = false;
    for (const auto &s : samples) for (size_t i = 0; i < n; ++i) if (s.active[i]) {
        ++alive[i]; any = true;
        for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], double(s.position[i*3+a])); hi[a] = std::max(hi[a], double(s.position[i*3+a])); }
    }
    std::vector<float> scores(n, -1.f);
    if (!any) return scores;
    const Eigen::Vector3d centre = (lo + hi) / 2;
    const double radius = std::max((hi - lo).norm() / 2, 1e-3), far = radius / std::tan(HalfFov) * 1.05, near = far * 0.45;
    std::vector<View> views;
    for (int k = 0; k < 8; ++k) {
        const double yaw = k * Pi / 4;
        views.push_back(look(centre + Eigen::Vector3d(std::sin(yaw)*far, 0.3*radius, std::cos(yaw)*far), centre));
        views.push_back(look(centre + Eigen::Vector3d(std::sin(yaw + Pi/8)*near, 0.1*radius, std::cos(yaw + Pi/8)*near), centre));
    }
    for (int k = 0; k < 4; ++k) {
        const double yaw = (k + 0.5) * Pi / 2;
        for (double height : {0.95, -0.95})
            views.push_back(look(centre + Eigen::Vector3d(std::sin(yaw)*0.35*far, height*far, std::cos(yaw)*0.35*far), centre));
    }
    if (cancelled && cancelled()) throw std::runtime_error("Export cancelled.");
    const double focal = Size / 2.0 / std::tan(HalfFov);
    const size_t workers = std::clamp<size_t>(std::thread::hardware_concurrency(), 1, views.size());
    std::vector<std::vector<double>> credits(workers, std::vector<double>(n, 0));
    std::vector<std::thread> threads;
    for (size_t w = 0; w < workers; ++w) threads.emplace_back([&, w] {
        std::vector<Item> items; std::vector<float> transmittance(size_t(Size)*Size);
        for (size_t v = w; v < views.size(); v += workers)
            for (const auto &s : samples) render(s, views[v], focal, antialiased, items, transmittance, credits[w]);
    });
    for (auto &t : threads) t.join();
    if (cancelled && cancelled()) throw std::runtime_error("Export cancelled.");
    // Pixels at this size, as pixels of a 1080p view.
    const double toHd = (1080.0 / Size) * (1080.0 / Size);
    for (size_t i = 0; i < n; ++i) {
        if (!alive[i]) continue;
        double credit = 0; for (const auto &c : credits) credit += c[i];
        scores[i] = float(credit / (double(alive[i]) * double(views.size())) * toHd);
    }
    return scores;
}

std::vector<uint8_t> pruneKeep(const std::vector<float> &scores, const QVector<PruneFilter> &filters, std::vector<PruneStats> *stats) {
    std::vector<uint8_t> keep(scores.size(), 1);
    if (stats) stats->clear();
    for (const auto &filter : filters) {
        std::vector<uint32_t> candidates;
        for (size_t i = 0; i < scores.size(); ++i) if (scores[i] >= 0 && double(scores[i]) < filter.protectAbove) candidates.push_back(uint32_t(i));
        std::sort(candidates.begin(), candidates.end(), [&](uint32_t a, uint32_t b) { return scores[a] < scores[b] || (scores[a] == scores[b] && a < b); });
        const size_t asked = size_t(std::floor(std::clamp(filter.percent, 0.0, 100.0) / 100 * double(scores.size())));
        const size_t limit = std::min(candidates.size(), asked);
        for (size_t k = 0; k < limit; ++k) keep[candidates[k]] = 0;
        if (stats) stats->push_back({scores.size(), asked, limit});
    }
    return keep;
}

std::vector<uint8_t> pruneChunk(double start, double end, const std::function<vgs::Frame(double)> &frameAt, bool antialiased,
                                const QVector<PruneFilter> &filters, const std::function<bool()> &cancelled, std::vector<PruneStats> *stats) {
    std::vector<vgs::Frame> samples;
    for (double t : pruneSampleTimes(start, end)) samples.push_back(frameAt(t));
    return pruneKeep(contributionScores(samples, antialiased, cancelled), filters, stats);
}
