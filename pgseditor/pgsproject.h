// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of PGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#pragma once
#include "gs2meshrunner.h"
#include "project.h"
#include "viewport.h"
#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QVector>

// One light of the scene the capture is previewed in.
struct PgsLight {
    QString name;
    bool directional = false, enabled = true;
    // A point light's position; a directional one's icon and gizmo, which do not light.
    QVector3D position{0, 2, 1.5f};
    // A directional light's turn, Euler XYZ in degrees as the editor's transforms: its light
    // travels along -Y turned by it (straight down with none).
    QVector3D rotation{0, 0, 0};
    QColor colour = Qt::white;
    // Directional: the light a splat facing it gets. Point: the same at one metre, falling
    // with the square of the distance (and to nothing at `range`, when it has one).
    float intensity = 1, range = 0;
    // Turning about the scene's vertical axis (x = 0, z = 0): one turn every turnSeconds, 0 for
    // none; counter-clockwise seen from above unless clockwise. The pose above is the one at
    // time 0; what is lit at time t is turned(angle at t).
    float turnSeconds = 0;
    bool clockwise = false;
    // Degrees per second, signed (+ counter-clockwise from above); 0 when it does not turn.
    float turnRate() const;
    // The light turned by `degrees` about the vertical axis through the origin: a point light's
    // position, a directional one's way and icon.
    PgsLight turned(float degrees) const;
    QVector3D direction() const;
    void setDirection(const QVector3D &way);
    // Where a directional light comes from, in degrees: azimuth about +Y from the front (+Z,
    // where captures face) towards +X, and elevation above the horizon.
    void angles(float *azimuth, float *elevation) const;
    void setAngles(float azimuth, float elevation);
    Transform transform() const;
    void setTransform(const Transform &);
    Viewport::ViewLight view(bool selected) const;
    QJsonObject json() const;
    static PgsLight fromJson(const QJsonObject &);
    // A sun from above, front and left, or a bulb at head height in front, as new lights come.
    static PgsLight sun(const QVector3D &centre, float size);
    static PgsLight bulb(const QVector3D &centre, float size);
    // The lights a new capture comes with, both off: a magenta and a blue one behind it on either
    // side, for the rim-lit look of a club or a stage.
    static QVector<PgsLight> defaults(const QVector3D &centre, float size);
    // Ready-made lightings, all on, placed around a subject at `centre` of `size` (its box's
    // diagonal): neon rims, three-point, a sun, a sunset...
    static QStringList presetNames();
    static QVector<PgsLight> preset(int index, const QVector3D &centre, float size);
};

// What the PGS editor keeps of a capture between sessions (.pgsproj, JSON): the PLY, the
// COLMAP model of its photos, the lights, how the mesh is made and where it was written.
// Paths are stored relative to the project, so a project moves with its files.
struct PgsProject {
    QString ply;
    // The COLMAP folder of the PLY's photos; colmapDeclined when the user chose to make the
    // mesh from the splats alone, so the editor does not ask again.
    QString colmap;
    bool colmapDeclined = false;
    QVector<PgsLight> lights;
    // How much of the splats' colour glows on its own when lit (Unreal's Self-illumination
    // Amount, 0.1 by default there too).
    float selfIllumination = 0.1f;
    // Every light turning together, on top of each one's own turn (PgsLight::turnSeconds): once
    // every 5 s for a new capture, ready for Play (the clock does not start by itself).
    float rigTurnSeconds = 5;
    bool rigClockwise = false;
    float rigTurnRate() const;
    // How far light i has turned at `seconds`, in degrees.
    float lightAngle(int i, double seconds) const;
    bool anyTurning() const;
    // Our estimate replaced the PLY's normals (or it had none): made again when opened; the
    // fine one (Estimate normals) when that is what was made.
    bool estimatedNormals = false;
    bool fineNormals = false;
    MeshSettings mesh;
    // The mesh gs2mesh made: its files (main .obj first, then the LODs), and the inputs it was
    // made from, to tell when it is out of date.
    QStringList meshFiles;
    QString meshInputs;
    // The main mesh as edited by hand (SculptMesh), <main>_edited.obj beside it; empty when it
    // is as gs2mesh made it. And what the LODs were made from (fileSignature of the main mesh
    // then), to tell when they no longer follow it.
    QString meshEdited;
    QString lodsSource;
    // Which mesh a .pgs carries: -1 the lowest LOD, 0 the main one, n the nth LOD; and the
    // longest side of its textures there (0: as made).
    int pgsMesh = -1, pgsTextureSize = 2048;
    bool pgsIncludesMesh = true;
    Camera camera;
    bool cameraSet = false;

    QJsonObject json(const QString &projectPath) const;
    static PgsProject fromJson(const QJsonObject &, const QString &projectPath); // throws std::runtime_error
    // What the mesh depends on: the PLY (path, size, time), the COLMAP folder and the settings
    // (but the LODs, made from it afterwards).
    QString currentMeshInputs() const;
    bool meshOutOfDate() const { return !meshFiles.isEmpty() && meshInputs != currentMeshInputs(); }
    // The .obj of the mesh a .pgs carries, or empty.
    QString pgsMeshFile() const;
    // The main mesh's .obj (as edited, when it is), then its LODs'.
    QStringList meshObjs() const;
    // The main mesh as gs2mesh made it.
    QString generatedObj() const;
    // A file's name, size and time, to tell when it changed.
    static QString fileSignature(const QString &path);
    // LODs made from another main mesh than the one there is now (it was edited since).
    bool lodsOutOfDate() const;
};
