// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of PGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#include "meshsculpt.h"
#include "dependencies/nanoflann/nanoflann.hpp"
#include <QFile>
#include <QSaveFile>
#include <cstring>
#include <functional>
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <Eigen/Eigenvalues>

namespace {
// Taubin's steps: a shrink, then a slightly larger inflate.
constexpr float kLambda = 0.55f, kMu = -0.58f;
constexpr int kLeafTriangles = 4;

// T4DEditor's brushFalloff: full inside the hardness share, then smooth (C1 at both ends) to 0.
float brushFalloff(float t, float hardness = 0.f) {
    if (t >= 1.f) return 0.f;
    if (t <= hardness) return 1.f;
    const float s = (t - hardness) / std::max(1e-6f, 1.f - hardness);
    const float u = 1.f - s * s;
    return u * u;
}
// Compressed rows: for each key, the values given to it, in order.
void compress(size_t keys, const std::vector<std::pair<uint32_t, uint32_t>> &pairs, std::vector<uint32_t> &start, std::vector<uint32_t> &values) {
    start.assign(keys + 1, 0);
    for (const auto &p : pairs) ++start[p.first + 1];
    for (size_t k = 0; k < keys; ++k) start[k + 1] += start[k];
    values.resize(pairs.size());
    std::vector<uint32_t> at(start.begin(), start.end() - 1);
    for (const auto &p : pairs) values[at[p.first]++] = p.second;
}
} // namespace

SculptMesh::SculptMesh(const MeshData &mesh, const QImage &albedo) : source_(mesh) {
    size_t count = mesh.positionCount;
    for (uint32_t id : mesh.positionIds) count = std::max(count, size_t(id) + 1);
    if (mesh.positionIds.size() != mesh.vertices()) {
        // Without the OBJ's positions: each vertex its own point (seams would open; not gs2mesh's).
        source_.positionIds.resize(mesh.vertices());
        std::iota(source_.positionIds.begin(), source_.positionIds.end(), 0u);
        count = mesh.vertices();
    }
    source_.positionCount = count;
    vertexPoint_ = source_.positionIds;
    points_.assign(count * 3, 0.f);
    for (size_t v = 0; v < mesh.vertices(); ++v)
        for (int a = 0; a < 3; ++a) points_[size_t(vertexPoint_[v]) * 3 + a] = mesh.positions[v * 3 + a];
    original_ = points_;
    std::vector<std::pair<uint32_t, uint32_t>> pv;
    pv.reserve(mesh.vertices());
    for (size_t v = 0; v < mesh.vertices(); ++v) pv.emplace_back(vertexPoint_[v], uint32_t(v));
    compress(count, pv, pointVertexStart_, pointVertices_);
    indices_ = mesh.indices;
    view_ = std::make_shared<Viewport::ViewMesh>();
    view_->albedo = albedo.isNull() ? QImage() : albedo.convertToFormat(QImage::Format_RGBA8888);
    albedoOriginal_ = view_->albedo;
    view_->vertices.assign(mesh.vertices() * 8, 0.f);
    for (size_t v = 0; v < mesh.vertices(); ++v) {
        view_->vertices[v * 8 + 6] = mesh.uvs[v * 2];
        view_->vertices[v * 8 + 7] = mesh.uvs[v * 2 + 1];
    }
    stamp_.assign(count, 0);
    distance_.assign(count, 0.f);
    strokeIndex_.assign(count, -1);
    build();
}

void SculptMesh::build() {
    const size_t count = points_.size() / 3, triangles = indices_.size() / 3;
    tris_.resize(indices_.size());
    for (size_t i = 0; i < indices_.size(); ++i) tris_[i] = vertexPoint_[indices_[i]];
    std::vector<std::pair<uint32_t, uint32_t>> pt, edges;
    pt.reserve(tris_.size());
    edges.reserve(tris_.size() * 2);
    for (size_t t = 0; t < triangles; ++t)
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = tris_[t * 3 + k], b = tris_[t * 3 + (k + 1) % 3];
            pt.emplace_back(a, uint32_t(t));
            if (a != b) edges.emplace_back(a, b), edges.emplace_back(b, a);
        }
    compress(count, pt, pointTriStart_, pointTris_);
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    compress(count, edges, neighbourStart_, neighbours_);
    view_->indices = indices_;
    std::vector<uint32_t> all(count);
    std::iota(all.begin(), all.end(), 0u);
    pointNormals_.assign(count * 3, 0.f);
    buildBvh();
    touched(all);
}

void SculptMesh::touched(const std::vector<uint32_t> &moved) {
    // The normals of the moved points and of their neighbours (whose faces moved too).
    const uint32_t id = ++stampId_;
    std::vector<uint32_t> around;
    around.reserve(moved.size() * 2);
    for (uint32_t p : moved) {
        if (stamp_[p] != id) stamp_[p] = id, around.push_back(p);
        for (uint32_t k = neighbourStart_[p]; k < neighbourStart_[p + 1]; ++k)
            if (stamp_[neighbours_[k]] != id) stamp_[neighbours_[k]] = id, around.push_back(neighbours_[k]);
    }
    for (uint32_t p : around) {
        QVector3D n;
        for (uint32_t k = pointTriStart_[p]; k < pointTriStart_[p + 1]; ++k) {
            const uint32_t *t = &tris_[size_t(pointTris_[k]) * 3];
            n += QVector3D::crossProduct(point(t[1]) - point(t[0]), point(t[2]) - point(t[0]));
        }
        n = n.lengthSquared() > 1e-30f ? n.normalized() : QVector3D(0, 1, 0);
        for (int a = 0; a < 3; ++a) pointNormals_[size_t(p) * 3 + a] = n[a];
        for (uint32_t k = pointVertexStart_[p]; k < pointVertexStart_[p + 1]; ++k) {
            float *v = &view_->vertices[size_t(pointVertices_[k]) * 8];
            for (int a = 0; a < 3; ++a) v[a] = points_[size_t(p) * 3 + a], v[3 + a] = n[a];
        }
    }
    refit();
}

// ---- Bounding volumes over the triangles: built once per topology, refitted as points move.
void SculptMesh::buildBvh() {
    const size_t triangles = tris_.size() / 3;
    nodeTris_.resize(triangles);
    std::iota(nodeTris_.begin(), nodeTris_.end(), 0u);
    std::vector<QVector3D> centres(triangles);
    for (size_t t = 0; t < triangles; ++t) centres[t] = (point(tris_[t * 3]) + point(tris_[t * 3 + 1]) + point(tris_[t * 3 + 2])) / 3.f;
    nodes_.clear();
    nodes_.reserve(triangles / kLeafTriangles * 2 + 1);
    nodes_.push_back(Node{});
    nodes_[0].first = 0, nodes_[0].count = int(triangles);
    std::vector<int> work{0};
    while (!work.empty()) {
        const int at = work.back();
        work.pop_back();
        Node node = nodes_[size_t(at)];
        if (node.count <= kLeafTriangles) continue;
        QVector3D lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        for (int i = node.first; i < node.first + node.count; ++i) {
            const QVector3D &c = centres[nodeTris_[size_t(i)]];
            for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], c[a]), hi[a] = std::max(hi[a], c[a]);
        }
        const QVector3D size = hi - lo;
        const int axis = size.x() >= size.y() && size.x() >= size.z() ? 0 : size.y() >= size.z() ? 1 : 2;
        const int half = node.count / 2;
        std::nth_element(nodeTris_.begin() + node.first, nodeTris_.begin() + node.first + half, nodeTris_.begin() + node.first + node.count,
                         [&](uint32_t a, uint32_t b) { return centres[a][axis] < centres[b][axis]; });
        Node left, right;
        left.first = node.first, left.count = half;
        right.first = node.first + half, right.count = node.count - half;
        nodes_[size_t(at)].left = int(nodes_.size());
        nodes_.push_back(left);
        nodes_[size_t(at)].right = int(nodes_.size());
        nodes_.push_back(right);
        work.push_back(nodes_[size_t(at)].left);
        work.push_back(nodes_[size_t(at)].right);
    }
    refit();
}

void SculptMesh::refit() {
    // Children come after their parent: from the last node back, each sees its children done.
    for (size_t i = nodes_.size(); i-- > 0;) {
        Node &n = nodes_[i];
        for (int a = 0; a < 3; ++a) n.lo[a] = 1e30f, n.hi[a] = -1e30f;
        if (n.left < 0) {
            for (int k = n.first; k < n.first + n.count; ++k) {
                const uint32_t *t = &tris_[size_t(nodeTris_[size_t(k)]) * 3];
                for (int c = 0; c < 3; ++c)
                    for (int a = 0; a < 3; ++a) {
                        const float v = points_[size_t(t[c]) * 3 + a];
                        n.lo[a] = std::min(n.lo[a], v), n.hi[a] = std::max(n.hi[a], v);
                    }
            }
        } else {
            const Node &l = nodes_[size_t(n.left)], &r = nodes_[size_t(n.right)];
            for (int a = 0; a < 3; ++a) n.lo[a] = std::min(l.lo[a], r.lo[a]), n.hi[a] = std::max(l.hi[a], r.hi[a]);
        }
    }
}

bool SculptMesh::raycast(const QVector3D &origin, const QVector3D &direction, QVector3D *hit, QVector3D *normal, int *triangle) const {
    if (nodes_.empty() || tris_.empty()) return false;
    const QVector3D d = direction.normalized();
    QVector3D inverse;
    for (int a = 0; a < 3; ++a) inverse[a] = std::abs(d[a]) > 1e-12f ? 1.f / d[a] : (d[a] < 0 ? -1e30f : 1e30f);
    float best = std::numeric_limits<float>::max();
    int found = -1;
    std::vector<int> work{0};
    while (!work.empty()) {
        const Node &n = nodes_[size_t(work.back())];
        work.pop_back();
        float t0 = 0, t1 = best;
        bool missed = false;
        for (int a = 0; a < 3 && !missed; ++a) {
            float near = (n.lo[a] - origin[a]) * inverse[a], far = (n.hi[a] - origin[a]) * inverse[a];
            if (near > far) std::swap(near, far);
            t0 = std::max(t0, near), t1 = std::min(t1, far);
            missed = t0 > t1;
        }
        if (missed) continue;
        if (n.left >= 0) { work.push_back(n.left), work.push_back(n.right); continue; }
        for (int k = n.first; k < n.first + n.count; ++k) {
            const uint32_t *t = &tris_[size_t(nodeTris_[size_t(k)]) * 3];
            // Möller and Trumbore.
            const QVector3D a = point(t[0]), e1 = point(t[1]) - a, e2 = point(t[2]) - a;
            const QVector3D p = QVector3D::crossProduct(d, e2);
            const float det = QVector3D::dotProduct(e1, p);
            if (std::abs(det) < 1e-20f) continue;
            const float inv = 1.f / det;
            const QVector3D s = origin - a;
            const float u = QVector3D::dotProduct(s, p) * inv;
            if (u < 0 || u > 1) continue;
            const QVector3D q = QVector3D::crossProduct(s, e1);
            const float v = QVector3D::dotProduct(d, q) * inv;
            if (v < 0 || u + v > 1) continue;
            const float distance = QVector3D::dotProduct(e2, q) * inv;
            if (distance > 0 && distance < best) best = distance, found = int(nodeTris_[size_t(k)]);
        }
    }
    if (found < 0) return false;
    if (hit) *hit = origin + d * best;
    if (triangle) *triangle = found;
    if (normal) {
        const uint32_t *t = &tris_[size_t(found) * 3];
        *normal = QVector3D::crossProduct(point(t[1]) - point(t[0]), point(t[2]) - point(t[0])).normalized();
    }
    return true;
}

void SculptMesh::gather(const QVector3D &centre, float radius, std::vector<uint32_t> &out) const {
    out.clear();
    if (nodes_.empty()) return;
    const uint32_t id = ++stampId_;
    const float r2 = radius * radius;
    std::vector<int> work{0};
    while (!work.empty()) {
        const Node &n = nodes_[size_t(work.back())];
        work.pop_back();
        float d2 = 0;
        for (int a = 0; a < 3; ++a) {
            const float c = centre[a], below = n.lo[a] - c, above = c - n.hi[a];
            if (below > 0) d2 += below * below;
            else if (above > 0) d2 += above * above;
        }
        if (d2 > r2) continue;
        if (n.left >= 0) { work.push_back(n.left), work.push_back(n.right); continue; }
        for (int k = n.first; k < n.first + n.count; ++k)
            for (int c = 0; c < 3; ++c) {
                const uint32_t p = tris_[size_t(nodeTris_[size_t(k)]) * 3 + c];
                if (stamp_[p] == id) continue;
                stamp_[p] = id;
                if ((point(p) - centre).lengthSquared() <= r2) out.push_back(p);
            }
    }
}

// ---- Strokes
void SculptMesh::remember(uint32_t p) {
    if (strokeIndex_[p] >= 0) return;
    strokeIndex_[p] = int(strokePoints_.size());
    strokePoints_.push_back(p);
    for (int a = 0; a < 3; ++a) strokeBefore_.push_back(points_[size_t(p) * 3 + a]);
    strokeReach_.push_back(0.f);
}

void SculptMesh::footprint(const Dab &at, std::vector<uint32_t> &pts, std::vector<float> &weights) const {
    pts.clear(), weights.clear();
    if (at.radius <= 0) return;
    // Seeds: the corners of the triangle it landed on, else the points near it.
    std::vector<std::pair<uint32_t, float>> seeds;
    if (at.triangle >= 0 && size_t(at.triangle) * 3 < tris_.size())
        for (int k = 0; k < 3; ++k) {const uint32_t p = tris_[size_t(at.triangle) * 3 + k]; seeds.emplace_back(p, (point(p) - at.centre).length());}
    else {
        std::vector<uint32_t> near;
        gather(at.centre, at.radius, near);
        for (uint32_t p : near) seeds.emplace_back(p, (point(p) - at.centre).length());
    }
    // Along the surface (Dijkstra over the edges); in sphere mode a little further, then clipped
    // by the straight distance: still connected, but round in space.
    const float reach = at.alongSurface ? at.radius : at.radius * 1.6f;
    const uint32_t id = ++stampId_;
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
    std::vector<uint32_t> reached;
    for (const auto &[p, d] : seeds) {
        if (d > reach) continue;
        if (stamp_[p] != id) stamp_[p] = id, distance_[p] = d, reached.push_back(p), queue.push({d, p});
        else if (d < distance_[p]) distance_[p] = d, queue.push({d, p});
    }
    while (!queue.empty()) {
        const auto [d, p] = queue.top();
        queue.pop();
        if (d > distance_[p]) continue;
        for (uint32_t k = neighbourStart_[p]; k < neighbourStart_[p + 1]; ++k) {
            const uint32_t q = neighbours_[k];
            const float nd = d + (point(q) - point(p)).length();
            if (nd > reach) continue;
            if (stamp_[q] != id) stamp_[q] = id, distance_[q] = nd, reached.push_back(q), queue.push({nd, q});
            else if (nd < distance_[q]) distance_[q] = nd, queue.push({nd, q});
        }
    }
    for (uint32_t p : reached) {
        const float d = at.alongSurface ? distance_[p] : (point(p) - at.centre).length();
        const float f = brushFalloff(d / at.radius, std::clamp(at.hardness, 0.f, 0.95f));
        if (f > 0) pts.push_back(p), weights.push_back(f);
    }
}

void SculptMesh::beginStroke(Brush brush, const Dab &at) {
    if (stroking_) endStroke();
    stroking_ = true;
    brush_ = brush;
    strokePoints_.clear(), strokeBefore_.clear(), strokeReach_.clear();
    grabPoints_.clear(), grabWeights_.clear(), grabFrom_.clear();
    if (brush == Brush::Move) {
        footprint(at, grabPoints_, grabWeights_);
        for (uint32_t p : grabPoints_) {
            for (int a = 0; a < 3; ++a) grabFrom_.push_back(points_[size_t(p) * 3 + a]);
            remember(p);
        }
    }
}

void SculptMesh::smoothPoints(const std::vector<uint32_t> &pts, const std::vector<float> &weights, int passes) {
    std::vector<QVector3D> next(pts.size());
    for (int pass = 0; pass < passes; ++pass)
        for (const float step : {kLambda, kMu}) {
            for (size_t i = 0; i < pts.size(); ++i) {
                const uint32_t p = pts[i];
                const uint32_t from = neighbourStart_[p], to = neighbourStart_[p + 1];
                if (from == to) { next[i] = point(p); continue; }
                QVector3D mean;
                for (uint32_t k = from; k < to; ++k) mean += point(neighbours_[k]);
                mean /= float(to - from);
                next[i] = point(p) + (mean - point(p)) * (step * weights[i]);
            }
            for (size_t i = 0; i < pts.size(); ++i) setPoint(pts[i], next[i]);
        }
}

QVector3D SculptMesh::faceNormal(uint32_t t) const {
    const uint32_t *f = &tris_[size_t(t) * 3];
    return QVector3D::crossProduct(point(f[1]) - point(f[0]), point(f[2]) - point(f[0]));
}

bool SculptMesh::folded(uint32_t p, const QVector3D &reference) const {
    for (uint32_t k = pointTriStart_[p]; k < pointTriStart_[p + 1]; ++k) {
        const QVector3D n = faceNormal(pointTris_[k]);
        if (n.lengthSquared() > 1e-30f && QVector3D::dotProduct(n.normalized(), reference) < 0.f) return true;
    }
    return false;
}

void SculptMesh::relaxPoints(const std::vector<uint32_t> &pts, const std::vector<float> &weights, int passes) {
    std::vector<QVector3D> next(pts.size()), reference(pts.size()), spread(pts.size());
    if (local_.size() != points_.size() / 3) local_.assign(points_.size() / 3, -1);
    for (size_t i = 0; i < pts.size(); ++i) local_[pts[i]] = int(i);
    auto normal = [&](uint32_t p) { return QVector3D(pointNormals_[p * 3], pointNormals_[p * 3 + 1], pointNormals_[p * 3 + 2]); };
    for (int pass = 0; pass < passes; ++pass) {
        // The surface the points should lie in: their normals spread over four rings, so that in a
        // tangle, where every normal is wrong, the surface around still says which way it faces.
        for (size_t i = 0; i < pts.size(); ++i) reference[i] = normal(pts[i]);
        for (int ring = 0; ring < 4; ++ring) {
            for (size_t i = 0; i < pts.size(); ++i) {
                QVector3D sum = reference[i];
                for (uint32_t k = neighbourStart_[pts[i]]; k < neighbourStart_[pts[i] + 1]; ++k) {
                    const int j = local_[neighbours_[k]];
                    sum += j >= 0 ? reference[size_t(j)] : normal(neighbours_[k]);
                }
                spread[i] = sum.lengthSquared() > 1e-20f ? sum.normalized() : reference[i];
            }
            reference.swap(spread);
        }
        for (size_t i = 0; i < pts.size(); ++i) {
            const uint32_t p = pts[i];
            const uint32_t from = neighbourStart_[p], to = neighbourStart_[p + 1];
            if (from == to) { next[i] = point(p); continue; }
            QVector3D mean;
            for (uint32_t k = from; k < to; ++k) mean += point(neighbours_[k]);
            mean /= float(to - from);
            const QVector3D towards = mean - point(p), r = reference[i];
            // Folded, or facing far off the surface around: all the way to the middle; else along
            // the surface only.
            if (folded(p, r) || QVector3D::dotProduct(normal(p), r) < 0.5f) next[i] = point(p) + towards * std::min(1.f, 2.f * weights[i]);
            else next[i] = point(p) + (towards - r * QVector3D::dotProduct(towards, r)) * (0.6f * weights[i]);
        }
        for (size_t i = 0; i < pts.size(); ++i) setPoint(pts[i], next[i]);
        touched(pts);
    }
    for (uint32_t p : pts) local_[p] = -1;
}

void SculptMesh::dab(const Dab &at) {
    if (!stroking_ || brush_ == Brush::Move || at.radius <= 0) return;
    std::vector<uint32_t> pts;
    std::vector<float> f;
    const Brush kind = at.smooth ? Brush::Smooth : brush_; // Alt smooths with any brush
    // Relax works where the surface is torn and folded: distances along it mean little there, so
    // it reaches as far in space.
    Dab reach = at;
    if (kind == Brush::Relax) reach.alongSurface = false;
    footprint(reach, pts, f);
    if (pts.empty()) return;
    for (uint32_t p : pts) remember(p);
    const float strength = std::clamp(at.strength, 0.f, 1.f), sign = at.invert ? -1.f : 1.f;
    // A fraction of the radius per dab, so brushes behave the same at any size.
    const float step = strength * at.radius * 0.05f;
    auto normal = [&](uint32_t p) { return QVector3D(pointNormals_[p * 3], pointNormals_[p * 3 + 1], pointNormals_[p * 3 + 2]); };
    QVector3D areaNormal;
    for (size_t i = 0; i < pts.size(); ++i) areaNormal += normal(pts[i]) * f[i];
    areaNormal = areaNormal.lengthSquared() > 1e-20f ? areaNormal.normalized() : QVector3D(0, 1, 0);
    std::vector<QVector3D> delta(pts.size());
    switch (kind) {
    case Brush::Move: break;
    case Brush::Draw:
        for (size_t i = 0; i < pts.size(); ++i) delta[i] = areaNormal * (sign * step * f[i]);
        break;
    case Brush::Inflate:
        // Along normals averaged over two rings: pushing each point its own way folds the
        // surface. What is around is relaxed along the surface so it stays even.
        for (size_t i = 0; i < pts.size(); ++i) {
            const uint32_t v = pts[i];
            QVector3D n = normal(v), mean;
            const uint32_t from = neighbourStart_[v], to = neighbourStart_[v + 1];
            for (uint32_t k = from; k < to; ++k) {
                const uint32_t u = neighbours_[k];
                n += normal(u);
                for (uint32_t j = neighbourStart_[u]; j < neighbourStart_[u + 1]; ++j) n += normal(neighbours_[j]) * 0.5f;
                mean += point(u);
            }
            n = n.lengthSquared() > 1e-20f ? n.normalized() : normal(v);
            QVector3D relax;
            if (to > from) {relax = mean / float(to - from) - point(v); relax -= n * QVector3D::dotProduct(relax, n);}
            delta[i] = (n * (sign * step) + relax * 0.3f) * f[i];
        }
        break;
    case Brush::Pinch:
        for (size_t i = 0; i < pts.size(); ++i) {
            QVector3D toCentre = at.centre - point(pts[i]);
            toCentre -= areaNormal * QVector3D::dotProduct(toCentre, areaNormal);
            delta[i] = toCentre * (sign * std::min(1.f, strength * 0.1f) * f[i]);
        }
        break;
    case Brush::Smooth:
        for (size_t i = 0; i < pts.size(); ++i) {
            const uint32_t v = pts[i], from = neighbourStart_[v], to = neighbourStart_[v + 1];
            if (from == to) continue;
            QVector3D mean;
            for (uint32_t k = from; k < to; ++k) mean += point(neighbours_[k]);
            delta[i] = (mean / float(to - from) - point(v)) * (std::min(1.f, strength) * 0.5f * f[i]);
        }
        break;
    case Brush::Relax: {
        std::vector<float> w(pts.size());
        for (size_t i = 0; i < pts.size(); ++i) w[i] = strength * f[i];
        relaxPoints(pts, w, 2);
        break;
    }
    case Brush::Flatten: {
        // The plane through them, least squares (weighted principal axes).
        QVector3D mid;
        float total = 0;
        for (size_t i = 0; i < pts.size(); ++i) mid += point(pts[i]) * f[i], total += f[i];
        if (total <= 0) break;
        mid /= total;
        Eigen::Matrix3f cov = Eigen::Matrix3f::Zero();
        for (size_t i = 0; i < pts.size(); ++i) {
            const QVector3D d = point(pts[i]) - mid;
            const Eigen::Vector3f e(d.x(), d.y(), d.z());
            cov += f[i] * e * e.transpose();
        }
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix3f> solver(cov);
        const Eigen::Vector3f e = solver.eigenvectors().col(0);
        QVector3D n(e.x(), e.y(), e.z());
        if (!(n.lengthSquared() > 1e-20f)) break;
        n.normalize();
        for (size_t i = 0; i < pts.size(); ++i)
            delta[i] = n * (-QVector3D::dotProduct(point(pts[i]) - mid, n) * std::min(1.f, strength * 0.5f) * f[i]);
        break;
    }
    case Brush::Erase:
        // Back towards where gs2mesh made it.
        for (size_t i = 0; i < pts.size(); ++i) {
            const uint32_t v = pts[i];
            const QVector3D made(original_[v * 3], original_[v * 3 + 1], original_[v * 3 + 2]);
            delta[i] = (made - point(v)) * (std::min(1.f, strength * 0.5f) * f[i]);
        }
        break;
    }
    if (kind != Brush::Relax)
        for (size_t i = 0; i < pts.size(); ++i) setPoint(pts[i], point(pts[i]) + delta[i]);
    // One stroke builds up to a height (Pinch: a pull) set by the strength, however many times it
    // passes over the same place; a new stroke adds more.
    if (kind == Brush::Draw || kind == Brush::Inflate || kind == Brush::Pinch) {
        const float cap = at.radius * std::max(0.05f, strength) * (kind == Brush::Pinch ? 0.15f : 0.35f);
        for (size_t i = 0; i < pts.size(); ++i) {
            const int s = strokeIndex_[pts[i]];
            float &reached = strokeReach_[size_t(s)];
            reached = std::max(reached, f[i]);
            const QVector3D before(strokeBefore_[size_t(s) * 3], strokeBefore_[size_t(s) * 3 + 1], strokeBefore_[size_t(s) * 3 + 2]);
            const QVector3D moved = point(pts[i]) - before;
            const float amount = moved.length(), limit = cap * reached;
            if (amount > limit && amount > 0) setPoint(pts[i], before + moved * (limit / amount));
        }
    }
    if (kind != Brush::Relax) touched(pts);
}

void SculptMesh::moveBy(const QVector3D &offset) {
    if (!stroking_ || brush_ != Brush::Move || grabPoints_.empty()) return;
    for (size_t i = 0; i < grabPoints_.size(); ++i)
        setPoint(grabPoints_[i], QVector3D(grabFrom_[i * 3], grabFrom_[i * 3 + 1], grabFrom_[i * 3 + 2]) + offset * grabWeights_[i]);
    touched(grabPoints_);
}

bool SculptMesh::endStroke() {
    if (!stroking_) return false;
    stroking_ = false;
    if (textureStroke_) {
        textureStroke_ = false;
        const QRect dirty = textureDirty_;
        textureDirty_ = QRect();
        if (dirty.isEmpty()) {albedoBefore_ = QImage(); return false;}
        Step step;
        step.texels = dirty;
        step.texelsBefore = albedoBefore_.copy(dirty);
        step.texelsAfter = view_->albedo.copy(dirty);
        albedoBefore_ = QImage();
        push(std::move(step));
        albedoPainted_ = true;
        ++revision_;
        return true;
    }
    for (uint32_t p : strokePoints_) strokeIndex_[p] = -1;
    Step step;
    // Only the points that moved.
    for (size_t i = 0; i < strokePoints_.size(); ++i) {
        const uint32_t p = strokePoints_[i];
        bool moved = false;
        for (int a = 0; a < 3; ++a) moved = moved || points_[size_t(p) * 3 + a] != strokeBefore_[i * 3 + a];
        if (!moved) continue;
        step.points.push_back(p);
        for (int a = 0; a < 3; ++a) step.before.push_back(strokeBefore_[i * 3 + a]), step.after.push_back(points_[size_t(p) * 3 + a]);
    }
    strokePoints_.clear(), strokeBefore_.clear(), strokeReach_.clear(), grabPoints_.clear();
    if (step.points.empty()) return false;
    push(std::move(step));
    ++revision_;
    return true;
}

void SculptMesh::push(Step step) {
    step.id = nextStep_++;
    steps_.resize(size_t(undoAt_));
    steps_.push_back(std::move(step));
    undoAt_ = int(steps_.size());
}

// ---- Whole-mesh fixes
int SculptMesh::fixSpikes(float threshold) {
    if (stroking_) endStroke();
    const size_t count = points_.size() / 3;
    int first = 0;
    stroking_ = true;
    brush_ = Brush::Smooth;
    strokePoints_.clear(), strokeBefore_.clear();
    for (int round = 0; round < 5; ++round) {
        // The surface around each point: its normal averaged over two rings, so that a tangle of
        // folded faces does not decide its own reference.
        std::vector<float> around = pointNormals_;
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<float> spread(around.size(), 0.f);
            for (uint32_t p = 0; p < count; ++p) {
                for (int a = 0; a < 3; ++a) spread[p * 3 + a] = around[p * 3 + a];
                for (uint32_t k = neighbourStart_[p]; k < neighbourStart_[p + 1]; ++k)
                    for (int a = 0; a < 3; ++a) spread[p * 3 + a] += around[neighbours_[k] * 3 + a];
            }
            around.swap(spread);
        }
        std::vector<uint32_t> flagged;
        for (uint32_t p = 0; p < count; ++p) {
            const uint32_t from = neighbourStart_[p], to = neighbourStart_[p + 1];
            if (to - from < 2) continue;
            QVector3D mean;
            float edge = 0;
            for (uint32_t k = from; k < to; ++k) mean += point(neighbours_[k]), edge += (point(neighbours_[k]) - point(p)).length();
            mean /= float(to - from), edge /= float(to - from);
            bool spike = edge > 0 && (point(p) - mean).length() > threshold * edge;
            // A fold: one of its faces turned back against the surface around it.
            const QVector3D reference(around[p * 3], around[p * 3 + 1], around[p * 3 + 2]);
            if (!spike && reference.lengthSquared() > 1e-20f) spike = folded(p, reference.normalized());
            if (spike) flagged.push_back(p);
        }
        if (round == 0) first = int(flagged.size());
        if (flagged.empty()) break;
        // The flagged points fully, their two rings less: smoothed together.
        const uint32_t id = ++stampId_;
        std::vector<uint32_t> region;
        std::vector<float> weights;
        for (uint32_t p : flagged) stamp_[p] = id, region.push_back(p), weights.push_back(1.f);
        for (const float ring : {0.6f, 0.3f}) {
            const size_t end = region.size();
            for (size_t i = 0; i < end; ++i)
                for (uint32_t k = neighbourStart_[region[i]]; k < neighbourStart_[region[i] + 1]; ++k)
                    if (stamp_[neighbours_[k]] != id) stamp_[neighbours_[k]] = id, region.push_back(neighbours_[k]), weights.push_back(ring);
        }
        for (uint32_t p : region) remember(p);
        // A spike's points go back to where their neighbours are (a few times, for neighbouring
        // spikes), then what is around is blended in.
        std::vector<QVector3D> next(flagged.size());
        for (int pass = 0; pass < 3; ++pass) {
            for (size_t i = 0; i < flagged.size(); ++i) {
                const uint32_t p = flagged[i];
                QVector3D mean;
                for (uint32_t k = neighbourStart_[p]; k < neighbourStart_[p + 1]; ++k) mean += point(neighbours_[k]);
                next[i] = mean / float(neighbourStart_[p + 1] - neighbourStart_[p]);
            }
            for (size_t i = 0; i < flagged.size(); ++i) setPoint(flagged[i], next[i]);
        }
        smoothPoints(region, weights, 3);
        touched(region);
    }
    endStroke();
    return first;
}

int SculptMesh::removeLoosePieces(float fraction) {
    if (stroking_) endStroke();
    const size_t count = points_.size() / 3, triangles = tris_.size() / 3;
    std::vector<uint32_t> parent(count);
    std::iota(parent.begin(), parent.end(), 0u);
    auto root = [&](uint32_t p) {
        while (parent[p] != p) parent[p] = parent[parent[p]], p = parent[p];
        return p;
    };
    for (size_t t = 0; t < triangles; ++t) {
        const uint32_t a = root(tris_[t * 3]), b = root(tris_[t * 3 + 1]), c = root(tris_[t * 3 + 2]);
        parent[b] = a, parent[root(c)] = a;
    }
    std::vector<uint32_t> size(count, 0);
    for (size_t t = 0; t < triangles; ++t) ++size[root(tris_[t * 3])];
    const double least = double(fraction) * double(triangles);
    std::vector<uint32_t> kept;
    kept.reserve(indices_.size());
    for (size_t t = 0; t < triangles; ++t)
        if (double(size[root(tris_[t * 3])]) >= least)
            kept.insert(kept.end(), indices_.begin() + ptrdiff_t(t * 3), indices_.begin() + ptrdiff_t(t * 3 + 3));
    const int removed = int(triangles - kept.size() / 3);
    if (removed == 0 || kept.empty()) return 0;
    Step step;
    step.indicesBefore = indices_;
    step.indicesAfter = kept;
    push(std::move(step));
    setIndices(kept);
    return removed;
}

void SculptMesh::setIndices(const std::vector<uint32_t> &indices) {
    indices_ = indices;
    build();
    ++revision_;
}

void SculptMesh::applyPositions(const std::vector<uint32_t> &pts, const std::vector<float> &positions) {
    for (size_t i = 0; i < pts.size(); ++i)
        for (int a = 0; a < 3; ++a) points_[size_t(pts[i]) * 3 + a] = positions[i * 3 + a];
    touched(pts);
    ++revision_;
}

namespace {
void putTexels(QImage &image, const QRect &at, const QImage &texels) {
    for (int y = 0; y < at.height(); ++y)
        std::memcpy(image.scanLine(at.top() + y) + at.left() * 4, texels.constScanLine(y), size_t(at.width()) * 4);
}
} // namespace

QRect SculptMesh::undo() {
    if (!canUndo() || stroking_) return {};
    const Step &s = steps_[size_t(--undoAt_)];
    if (!s.texels.isNull()) {putTexels(view_->albedo, s.texels, s.texelsBefore);albedoPainted_ = true;++revision_;return s.texels;}
    if (!s.indicesBefore.empty()) setIndices(s.indicesBefore);
    else applyPositions(s.points, s.before);
    return {};
}

QRect SculptMesh::redo() {
    if (!canRedo() || stroking_) return {};
    const Step &s = steps_[size_t(undoAt_++)];
    if (!s.texels.isNull()) {putTexels(view_->albedo, s.texels, s.texelsAfter);albedoPainted_ = true;++revision_;return s.texels;}
    if (!s.indicesAfter.empty()) setIndices(s.indicesAfter);
    else applyPositions(s.points, s.after);
    return {};
}

MeshData SculptMesh::mesh() const {
    MeshData m = source_;
    for (size_t v = 0; v < m.vertices(); ++v)
        for (int a = 0; a < 3; ++a) {
            m.positions[v * 3 + a] = points_[size_t(vertexPoint_[v]) * 3 + a];
            m.normals[v * 3 + a] = pointNormals_[size_t(vertexPoint_[v]) * 3 + a];
        }
    m.indices = indices_;
    return m;
}

// ---- Painting the albedo (T4DEditor's TextureBrush, painted in place)
namespace {
struct Rgba { float r, g, b; };
Rgba texelAt(const QImage &image, int x, int y) {
    const uchar *p = image.constScanLine(y) + x * 4;
    return {p[0] / 255.f, p[1] / 255.f, p[2] / 255.f};
}
void putTexel(QImage &image, int x, int y, const Rgba &c, float a) {
    uchar *p = image.scanLine(y) + x * 4;
    const float v[3] = {c.r, c.g, c.b};
    for (int k = 0; k < 3; ++k) {
        const float now = p[k] / 255.f;
        p[k] = uchar(std::clamp(now + (v[k] - now) * a, 0.f, 1.f) * 255.f + 0.5f);
    }
}
// Every texel centre a UV triangle covers, with its barycentric weights (T4DEditor's Atlas::rasterize).
void rasterize(int width, int height, const float a[2], const float b[2], const float c[2], const std::function<void(int, int, float, float, float)> &visit) {
    const float ax = a[0] * width, ay = (1 - a[1]) * height, bx = b[0] * width, by = (1 - b[1]) * height, cx = c[0] * width, cy = (1 - c[1]) * height;
    const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (std::abs(area) < 1e-12f) return;
    const int x0 = std::max(0, int(std::floor(std::min({ax, bx, cx})))), x1 = std::min(width - 1, int(std::ceil(std::max({ax, bx, cx}))));
    const int y0 = std::max(0, int(std::floor(std::min({ay, by, cy})))), y1 = std::min(height - 1, int(std::ceil(std::max({ay, by, cy}))));
    const float inv = 1.f / area, eps = -1e-4f; // a small tolerance keeps shared edges watertight
    for (int y = y0; y <= y1; ++y) {
        const float py = y + 0.5f;
        for (int x = x0; x <= x1; ++x) {
            const float px = x + 0.5f;
            const float w1 = ((px - ax) * (cy - ay) - (py - ay) * (cx - ax)) * inv, w2 = ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) * inv;
            const float w0 = 1.f - w1 - w2;
            if (w0 >= eps && w1 >= eps && w2 >= eps) visit(x, y, w0, w1, w2);
        }
    }
}
} // namespace

void SculptMesh::setOriginalAlbedo(const QImage &albedo) {
    if (!albedo.isNull() && albedo.size() == view_->albedo.size()) albedoOriginal_ = albedo.convertToFormat(QImage::Format_RGBA8888);
}

void SculptMesh::beginTextureStroke(TextureBrush brush) {
    if (stroking_) endStroke();
    if (!hasAlbedo()) return;
    stroking_ = textureStroke_ = true;
    textureBrush_ = brush;
    textureDirty_ = QRect();
    albedoBefore_ = view_->albedo.copy();
}

QRect SculptMesh::textureDab(const TextureDab &dab) {
    if (!textureStroke_ || dab.radius <= 0) return {};
    QImage &image = view_->albedo;
    const int w = image.width(), h = image.height();
    // The faces near the dab (sphere reach, as T4DEditor's texture brushes).
    std::vector<uint32_t> near;
    gather(dab.centre, dab.radius, near);
    std::vector<uint32_t> faces;
    for (uint32_t p : near)
        for (uint32_t k = pointTriStart_[p]; k < pointTriStart_[p + 1]; ++k) faces.push_back(pointTris_[k]);
    std::sort(faces.begin(), faces.end());
    faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
    const Rgba colour{float(dab.colour.redF()), float(dab.colour.greenF()), float(dab.colour.blueF())};
    // Blur reads what was there before the dab, so it does not feed on itself.
    const QImage before = textureBrush_ == TextureBrush::Blur ? image.copy() : QImage();
    // Clone: where the offset point lands on the mesh, its texture coordinates.
    auto cloneSource = [&](const QVector3D &q, Rgba *out) {
        std::vector<uint32_t> around;
        const QVector3D at = q + dab.cloneOffset;
        gather(at, dab.radius * 0.5f, around);
        float best = std::numeric_limits<float>::max();
        int face = -1;
        float bary[3] = {1, 0, 0};
        for (uint32_t p : around)
            for (uint32_t k = pointTriStart_[p]; k < pointTriStart_[p + 1]; ++k) {
                const uint32_t f = pointTris_[k];
                const QVector3D a = point(tris_[f * 3]), b = point(tris_[f * 3 + 1]), c = point(tris_[f * 3 + 2]);
                const QVector3D mid = (a + b + c) / 3.f;
                const float d = (mid - at).lengthSquared();
                if (d >= best) continue;
                // Barycentric of the point projected on the face.
                const QVector3D v0 = b - a, v1 = c - a, v2 = at - a;
                const float d00 = QVector3D::dotProduct(v0, v0), d01 = QVector3D::dotProduct(v0, v1), d11 = QVector3D::dotProduct(v1, v1);
                const float d20 = QVector3D::dotProduct(v2, v0), d21 = QVector3D::dotProduct(v2, v1), den = d00 * d11 - d01 * d01;
                if (std::abs(den) < 1e-20f) continue;
                float v = (d11 * d20 - d01 * d21) / den, u = (d00 * d21 - d01 * d20) / den;
                v = std::clamp(v, 0.f, 1.f), u = std::clamp(u, 0.f, 1.f - v);
                best = d, face = int(f), bary[0] = 1 - v - u, bary[1] = v, bary[2] = u;
            }
        if (face < 0) return false;
        float uv[2] = {0, 0};
        for (int k = 0; k < 3; ++k)
            for (int a = 0; a < 2; ++a) uv[a] += source_.uvs[size_t(indices_[size_t(face) * 3 + k]) * 2 + a] * bary[k];
        *out = texelAt(image, std::clamp(int(uv[0] * w), 0, w - 1), std::clamp(int((1.f - uv[1]) * h), 0, h - 1));
        return true;
    };
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (uint32_t f : faces) {
        const QVector3D a = point(tris_[f * 3]), b = point(tris_[f * 3 + 1]), c = point(tris_[f * 3 + 2]);
        if (QVector3D::dotProduct(QVector3D::crossProduct(b - a, c - a), dab.eye - (a + b + c) / 3.f) <= 0) continue; // turned away
        const uint32_t *t = &indices_[size_t(f) * 3];
        const float ua[2] = {source_.uvs[t[0] * 2], source_.uvs[t[0] * 2 + 1]}, ub[2] = {source_.uvs[t[1] * 2], source_.uvs[t[1] * 2 + 1]},
                    uc[2] = {source_.uvs[t[2] * 2], source_.uvs[t[2] * 2 + 1]};
        rasterize(w, h, ua, ub, uc, [&](int x, int y, float b0, float b1, float b2) {
            const QVector3D q = a * b0 + b * b1 + c * b2;
            const float fall = brushFalloff((q - dab.centre).length() / dab.radius, std::clamp(dab.hardness, 0.f, 0.95f));
            if (fall <= 0) return;
            const float alpha = std::clamp(dab.opacity * fall, 0.f, 1.f);
            switch (textureBrush_) {
            case TextureBrush::Paint: putTexel(image, x, y, colour, alpha); break;
            case TextureBrush::Unpaint: putTexel(image, x, y, texelAt(albedoOriginal_, x, y), alpha); break;
            case TextureBrush::Clone: {Rgba s;if (!cloneSource(q, &s)) return;putTexel(image, x, y, s, alpha);break;}
            case TextureBrush::Blur: {
                Rgba sum{0, 0, 0};
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const Rgba v = texelAt(before, std::clamp(x + dx, 0, w - 1), std::clamp(y + dy, 0, h - 1));
                        sum = {sum.r + v.r, sum.g + v.g, sum.b + v.b};
                    }
                putTexel(image, x, y, {sum.r / 25, sum.g / 25, sum.b / 25}, alpha);
                break;
            }
            case TextureBrush::Lighten:
            case TextureBrush::Darken: {
                const Rgba v = texelAt(image, x, y);
                const float k = textureBrush_ == TextureBrush::Lighten ? 1.35f : 0.7f;
                putTexel(image, x, y, {std::min(1.f, v.r * k), std::min(1.f, v.g * k), std::min(1.f, v.b * k)}, alpha);
                break;
            }
            }
            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
        });
    }
    if (x1 < 0) return {};
    const QRect changed(QPoint(x0, y0), QPoint(x1, y1));
    textureDirty_ = textureDirty_.united(changed);
    return changed;
}

void SculptMesh::padAlbedo() {
    QImage &image = view_->albedo;
    if (image.isNull()) return;
    const int width = image.width(), height = image.height();
    if (padTo_.empty()) {
        // T4DEditor's DisplayPadding: ring by ring, every empty texel touching a filled one copies it.
        std::vector<uint8_t> covered(size_t(width) * height, 0);
        for (size_t t = 0; t + 2 < indices_.size(); t += 3) {
            const float *a = &source_.uvs[size_t(indices_[t]) * 2], *b = &source_.uvs[size_t(indices_[t + 1]) * 2], *c = &source_.uvs[size_t(indices_[t + 2]) * 2];
            rasterize(width, height, a, b, c, [&](int x, int y, float, float, float) { covered[size_t(y) * width + x] = 1; });
        }
        static constexpr int kDx[8] = {-1, 0, 1, -1, 1, -1, 0, 1}, kDy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
        std::vector<int32_t> frontier;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                if (!covered[size_t(y) * width + x])
                    for (int k = 0; k < 8; ++k) {
                        const int nx = x + kDx[k], ny = y + kDy[k];
                        if (nx >= 0 && ny >= 0 && nx < width && ny < height && covered[size_t(ny) * width + nx]) {frontier.push_back(y * width + x);break;}
                    }
        for (int ring = 0; ring < 8 && !frontier.empty(); ++ring) {
            const size_t begin = padTo_.size();
            for (const int32_t i : frontier) {
                const int x = i % width, y = i / width;
                for (int k = 0; k < 8; ++k) {
                    const int nx = x + kDx[k], ny = y + kDy[k];
                    if (nx >= 0 && ny >= 0 && nx < width && ny < height && covered[size_t(ny) * width + nx]) {padTo_.push_back(i);padFrom_.push_back(ny * width + nx);break;}
                }
            }
            std::vector<int32_t> next;
            for (size_t j = begin; j < padTo_.size(); ++j) covered[size_t(padTo_[j])] = 1;
            for (size_t j = begin; j < padTo_.size(); ++j) {
                const int x = padTo_[j] % width, y = padTo_[j] / width;
                for (int k = 0; k < 8; ++k) {
                    const int nx = x + kDx[k], ny = y + kDy[k];
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                    uint8_t &c = covered[size_t(ny) * width + nx];
                    if (!c) c = 2, next.push_back(ny * width + nx);
                }
            }
            for (const int32_t i : next) covered[size_t(i)] = 0;
            frontier.swap(next);
        }
        if (padTo_.empty()) padTo_.push_back(-1); // nothing to pad: made, though
    }
    auto *pixels = reinterpret_cast<uint32_t *>(image.bits());
    const qsizetype stride = image.bytesPerLine() / 4;
    for (size_t j = 0; j < padTo_.size(); ++j)
        if (padTo_[j] >= 0) pixels[(padTo_[j] / width) * stride + padTo_[j] % width] = pixels[(padFrom_[j] / width) * stride + padFrom_[j] % width];
}

// ---- The dense surface, moved alike
namespace {
struct PointCloud {
    const std::vector<float> *p = nullptr;
    size_t kdtree_get_point_count() const { return p->size() / 3; }
    float kdtree_get_pt(size_t i, size_t d) const { return (*p)[i * 3 + d]; }
    template <class Box> bool kdtree_get_bbox(Box &) const { return false; }
};
[[noreturn]] void failWith(const QString &message) { throw std::runtime_error(message.toStdString()); }
} // namespace

void transferEdits(const std::vector<float> &generated, const std::vector<float> &edited, const QString &denseIn, const QString &denseOut) {
    if (generated.size() != edited.size() || generated.empty()) failWith(QStringLiteral("The edited mesh does not match the generated one."));
    QFile in(denseIn);
    if (!in.open(QIODevice::ReadOnly)) failWith(QStringLiteral("Cannot read %1.").arg(denseIn));
    QByteArray data = in.readAll();
    const qsizetype end = data.indexOf("end_header\n");
    if (!data.startsWith("ply") || end < 0) failWith(QStringLiteral("%1 is not a PLY.").arg(denseIn));
    const qsizetype body = end + 11;
    // The vertex element: its count and its properties, all floats, x y z among them.
    qint64 vertices = -1;
    int stride = 0, x = -1;
    bool inVertex = false;
    for (const QByteArray &raw : data.left(end).split('\n')) {
        const QList<QByteArray> words = raw.trimmed().split(' ');
        if (words.value(0) == "format" && words.value(1) != "binary_little_endian") failWith(QStringLiteral("%1 is not a binary PLY.").arg(denseIn));
        if (words.value(0) == "element") {
            inVertex = words.value(1) == "vertex";
            if (inVertex) vertices = words.value(2).toLongLong();
        } else if (words.value(0) == "property" && inVertex) {
            if (words.value(1) != "float") failWith(QStringLiteral("%1: vertices other than floats.").arg(denseIn));
            if (words.value(2) == "x") x = stride;
            ++stride;
        }
    }
    if (vertices <= 0 || x < 0 || body + vertices * stride * 4 > data.size()) failWith(QStringLiteral("%1 has no vertices to move.").arg(denseIn));
    // Where points moved: only dense vertices near there are looked at.
    float reach = 0;
    QVector3D lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    bool any = false;
    for (size_t p = 0; p < generated.size() / 3; ++p) {
        const QVector3D a(generated[p * 3], generated[p * 3 + 1], generated[p * 3 + 2]), b(edited[p * 3], edited[p * 3 + 1], edited[p * 3 + 2]);
        const float moved = (b - a).length();
        if (moved <= 1e-7f) continue;
        any = true, reach = std::max(reach, moved);
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], a[k]), hi[k] = std::max(hi[k], a[k]);
    }
    if (any) {
        PointCloud cloud{&generated};
        using Tree = nanoflann::KDTreeSingleIndexAdaptor<nanoflann::L2_Simple_Adaptor<float, PointCloud>, PointCloud, 3>;
        Tree tree(3, cloud, nanoflann::KDTreeSingleIndexAdaptorParams(16));
        tree.buildIndex();
        lo -= QVector3D(reach, reach, reach) * 2, hi += QVector3D(reach, reach, reach) * 2;
        char *base = data.data() + body;
        const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                for (qint64 v = t; v < vertices; v += threads) {
                    float *q = reinterpret_cast<float *>(base + v * stride * 4) + x;
                    bool inside = true;
                    for (int k = 0; k < 3; ++k) inside = inside && q[k] >= lo[k] && q[k] <= hi[k];
                    if (!inside) continue;
                    size_t ids[4];
                    float d2[4];
                    nanoflann::KNNResultSet<float> found(4);
                    found.init(ids, d2);
                    tree.findNeighbors(found, q, nanoflann::SearchParameters());
                    double weight = 0, shift[3] = {0, 0, 0};
                    for (size_t k = 0; k < found.size(); ++k) {
                        const double w = 1.0 / (std::sqrt(double(d2[k])) + 1e-5);
                        for (int a = 0; a < 3; ++a) shift[a] += w * double(edited[ids[k] * 3 + a] - generated[ids[k] * 3 + a]);
                        weight += w;
                    }
                    if (weight > 0)
                        for (int a = 0; a < 3; ++a) q[a] += float(shift[a] / weight);
                }
            });
        for (auto &t : pool) t.join();
    }
    QSaveFile out(denseOut);
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit()) failWith(QStringLiteral("Cannot write %1: %2").arg(denseOut, out.errorString()));
}
