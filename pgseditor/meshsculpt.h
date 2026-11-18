// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of PGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#pragma once
#include "meshio.h"
#include "viewport.h"
#include <QColor>
#include <QImage>
#include <QRect>
#include <QVector3D>
#include <memory>
#include <vector>

// The main mesh gs2mesh made, being cleaned up by hand: spikes, noise and stray pieces. Only its
// vertices move and faces go; nothing is added, so its UVs, and the albedo laid out on them, stay
// valid. The vertices of a UV seam share an OBJ position (a point here) and move together, so the
// seam does not open.
//
// The brushes are T4DEditor's (its Deform::brushDab), so both editors sculpt alike, plus Relax:
// each dab acts on the points within the brush's radius of where it lands, measured along the
// surface (so another part passing close, the other side of a finger, is left alone) or in
// space, at full strength inside the hardness share of the radius and fading out beyond it.
// Move drags them with the cursor; Draw pushes them out along the area's normal (inverted, in);
// Inflate pushes each along its normal averaged over two rings, keeping the surface even;
// Pinch pulls them towards the stroke (inverted, apart); Smooth moves each towards its
// neighbours' middle; Relax slides them along the surface to even the triangles out, the shape
// kept, and where faces are folded over takes the points to their neighbours' middle until they
// unfold (it always reaches in space: along a torn, folded surface distances mean little); Flatten pulls them onto the plane fitted through them; Erase takes them back to where
// gs2mesh made them. A dab moves a twentieth of the radius times the strength; one stroke of
// Draw, Inflate or Pinch builds up to a height set by the strength, however often it passes. A
// stroke, from press to release, is one undo step; so is each whole-mesh fix.
// The dense surface gs2mesh made the main mesh from, moved as the main mesh was edited, so that
// the normal maps baked from it (the LODs', the main mesh's again) agree with the edits: each of
// its vertices by the displacement of the four nearest points of the main mesh, weighed by
// inverse distance; those far from every moved point stay. `generated` and `edited`: one
// position per OBJ position (SculptMesh::originalPoints, points). A binary PLY of float
// vertices, written to denseOut. Throws std::runtime_error.
void transferEdits(const std::vector<float> &generated, const std::vector<float> &edited, const QString &denseIn, const QString &denseOut);

class SculptMesh {
public:
    enum class Brush { Move, Draw, Inflate, Pinch, Smooth, Relax, Flatten, Erase };
    // Where a dab lands and how: the point and the triangle it is on, the radius (mesh units) and
    // the brush's settings, as T4DEditor's.
    struct Dab {
        QVector3D centre;
        int triangle = -1;
        float radius = 0.03f;
        float strength = 0.5f;   // 0..1
        float hardness = 0.3f;   // the share of the radius at full strength, 0..0.95
        bool alongSurface = true;
        bool invert = false;     // Draw in, Inflate deflates, Pinch spreads
        bool smooth = false;     // any brush smooths meanwhile (Alt)
    };

    // The mesh as read (readObj, with positionIds) and its albedo, for the view.
    SculptMesh(const MeshData &mesh, const QImage &albedo);

    // What the viewport draws, kept up to date by every edit (the same object).
    std::shared_ptr<Viewport::ViewMesh> view() const { return view_; }
    // The nearest point of the mesh along a ray, the face's normal there and which face it is.
    bool raycast(const QVector3D &origin, const QVector3D &direction, QVector3D *hit, QVector3D *normal = nullptr, int *triangle = nullptr) const;

    // A stroke of `brush`: begun where it lands (Move catches the points under it there), given
    // dabs (Move: offsets from where it began), ended.
    void beginStroke(Brush brush, const Dab &at);
    void dab(const Dab &at);
    void moveBy(const QVector3D &offset);
    // False when the stroke moved nothing (no undo step).
    bool endStroke();
    bool stroking() const { return stroking_; }
    Brush strokeBrush() const { return brush_; }

    // Whole-mesh fixes, each one undo step.
    // Spikes and folds: points far off their neighbours' surface (more than `threshold` times the
    // length of their edges) and those of faces turned back on each other, smoothed with their
    // surroundings. Returns how many points were found.
    int fixSpikes(float threshold = 0.9f);
    // Pieces not joined to the rest with fewer than `fraction` of the triangles: removed. Returns
    // the triangles removed.
    int removeLoosePieces(float fraction = 0.02f);

    // Painting the albedo, with T4DEditor's texture brushes: Paint the colour, Unpaint back to
    // the albedo as gs2mesh made it, Clone what is elsewhere (at the clone offset), Blur, Lighten,
    // Darken. Only the texels of faces turned to `eye` are painted, as much as the brush's falloff
    // at their place on the surface times the opacity. A stroke is one undo step, as the brushes'.
    enum class TextureBrush { Paint, Unpaint, Clone, Blur, Lighten, Darken };
    struct TextureDab {
        QVector3D centre, eye, cloneOffset;
        float radius = 0.03f, hardness = 0.3f, opacity = 0.5f;
        QColor colour = QColor(220, 60, 60);
    };
    bool hasAlbedo() const { return !view_->albedo.isNull(); }
    const QImage &albedo() const { return view_->albedo; }
    // What Unpaint goes back to (the albedo gs2mesh made; by default the one it was built with).
    void setOriginalAlbedo(const QImage &albedo);
    void beginTextureStroke(TextureBrush brush);
    // The texels it changed (image rows from the top), or a null rectangle.
    QRect textureDab(const TextureDab &dab);
    // The albedo painted since it was built, or since markAlbedoSaved.
    bool albedoPainted() const { return albedoPainted_; }
    void markAlbedoSaved() { albedoPainted_ = false; }
    // The gutters around the UV islands filled from their edges (8 texels), so that filtering
    // does not bleed old colours into the seams: after a stroke and before writing.
    void padAlbedo();

    bool canUndo() const { return undoAt_ > 0; }
    bool canRedo() const { return undoAt_ < int(steps_.size()); }
    // Each returns the albedo's texels it changed, or a null rectangle (the shape changed).
    QRect undo();
    QRect redo();
    // Changes as made, undone or redone: a number that differs whenever the mesh does.
    quint64 revision() const { return revision_; }
    // Which edit the mesh is at: the same after undoing back to it (0: as built).
    quint64 state() const { return undoAt_ > 0 ? steps_[size_t(undoAt_ - 1)].id : 0; }

    // The mesh as it is now, to write (writeObj): the same vertices, UVs and position ids.
    MeshData mesh() const;
    // The positions as built (generated) and as they are now, one per OBJ position.
    const std::vector<float> &originalPoints() const { return original_; }
    const std::vector<float> &points() const { return points_; }
    size_t triangles() const { return indices_.size() / 3; }

private:
    struct Node { float lo[3], hi[3]; int first = 0, count = 0, left = -1, right = -1; };
    struct Step {
        quint64 id = 0;
        std::vector<uint32_t> points;
        std::vector<float> before, after;            // 3 per point
        std::vector<uint32_t> indicesBefore, indicesAfter; // when faces went
        QRect texels;                                     // when the albedo was painted
        QImage texelsBefore, texelsAfter;
    };

    void build();             // adjacency, normals, view, BVH, from indices_
    void buildBvh();
    void refit();
    void gather(const QVector3D &centre, float radius, std::vector<uint32_t> &out) const;
    // The points a dab reaches and how much (T4DEditor's Deform::footprint).
    void footprint(const Dab &at, std::vector<uint32_t> &points, std::vector<float> &falloff) const;
    void touched(const std::vector<uint32_t> &points); // normals, view, bounds after they moved
    void remember(uint32_t point);
    void applyPositions(const std::vector<uint32_t> &points, const std::vector<float> &positions);
    void setIndices(const std::vector<uint32_t> &indices);
    QVector3D point(uint32_t p) const { return {points_[p * 3], points_[p * 3 + 1], points_[p * 3 + 2]}; }
    void setPoint(uint32_t p, const QVector3D &v) { points_[p * 3] = v.x(), points_[p * 3 + 1] = v.y(), points_[p * 3 + 2] = v.z(); }
    void smoothPoints(const std::vector<uint32_t> &points, const std::vector<float> &weights, int passes);
    void relaxPoints(const std::vector<uint32_t> &points, const std::vector<float> &weights, int passes);
    QVector3D faceNormal(uint32_t triangle) const;
    // A point with one of its faces turned against the surface around it.
    bool folded(uint32_t point, const QVector3D &reference) const;

    MeshData source_;                 // uvs, ids and paths, as read
    std::vector<float> original_, points_, pointNormals_;
    std::vector<uint32_t> vertexPoint_;          // render vertex -> point
    std::vector<uint32_t> pointVertexStart_, pointVertices_; // point -> render vertices
    std::vector<uint32_t> indices_;              // render triangles
    std::vector<uint32_t> tris_;                 // the same over points
    std::vector<uint32_t> pointTriStart_, pointTris_;
    std::vector<uint32_t> neighbourStart_, neighbours_;
    std::vector<Node> nodes_;
    std::vector<uint32_t> nodeTris_;
    mutable std::vector<uint32_t> stamp_;
    std::vector<int> local_; // relaxPoints: a point's place among those it moves, else -1
    mutable std::vector<float> distance_; // footprint: along the surface from where the dab landed
    mutable uint32_t stampId_ = 0;
    std::shared_ptr<Viewport::ViewMesh> view_;

    // The stroke going on: its brush, the points it moved and where they were before it.
    bool stroking_ = false;
    Brush brush_ = Brush::Smooth;
    std::vector<uint32_t> strokePoints_;
    std::vector<float> strokeBefore_, strokeReach_; // where they were; how near the middle the brush passed
    std::vector<int> strokeIndex_;                   // a point's place in strokePoints_, else -1
    std::vector<uint32_t> grabPoints_;
    std::vector<float> grabWeights_, grabFrom_;
    // Painting: the brush, the albedo before the stroke and what it changed; the albedo gs2mesh
    // made; the padding map (gutter texel <- island texel), made when first wanted.
    bool textureStroke_ = false, albedoPainted_ = false;
    TextureBrush textureBrush_ = TextureBrush::Paint;
    QImage albedoBefore_, albedoOriginal_;
    QRect textureDirty_;
    std::vector<int32_t> padTo_, padFrom_;

    std::vector<Step> steps_;
    int undoAt_ = 0;
    quint64 revision_ = 0, nextStep_ = 1;
    void push(Step step);
};
