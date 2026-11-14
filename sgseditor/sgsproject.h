// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#pragma once
#include "gs2meshrunner.h"
#include "project.h"
#include "viewport.h"
#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QVector>

// One light of the scene the capture is previewed in.
struct SgsLight {
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
    SgsLight turned(float degrees) const;
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
    static SgsLight fromJson(const QJsonObject &);
    // A sun from above, front and left, or a bulb at head height in front, as new lights come.
    static SgsLight sun(const QVector3D &centre, float size);
    static SgsLight bulb(const QVector3D &centre, float size);
    // The lights a new capture comes with, both off: a magenta and a blue one behind it on either
    // side, for the rim-lit look of a club or a stage.
    static QVector<SgsLight> defaults(const QVector3D &centre, float size);
    // Ready-made lightings, all on, placed around a subject at `centre` of `size` (its box's
    // diagonal): neon rims, three-point, a sun, a sunset...
    static QStringList presetNames();
    static QVector<SgsLight> preset(int index, const QVector3D &centre, float size);
};

// What the SGS editor keeps of a capture between sessions (.sgsproj, JSON): the PLY, the
// COLMAP model of its photos, the lights, how the mesh is made and where it was written.
// Paths are stored relative to the project, so a project moves with its files.
struct SgsProject {
    QString ply;
    // The COLMAP folder of the PLY's photos; colmapDeclined when the user chose to make the
    // mesh from the splats alone, so the editor does not ask again.
    QString colmap;
    bool colmapDeclined = false;
    QVector<SgsLight> lights;
    // How much of the splats' colour glows on its own when lit (Unreal's Self-illumination
    // Amount, 0.1 by default there too).
    float selfIllumination = 0.1f;
    // Every light turning together, on top of each one's own turn (SgsLight::turnSeconds).
    float rigTurnSeconds = 0;
    bool rigClockwise = false;
    float rigTurnRate() const;
    // How far light i has turned at `seconds`, in degrees.
    float lightAngle(int i, double seconds) const;
    bool anyTurning() const;
    // Our estimate replaced the PLY's normals (or it had none): made again when opened.
    bool estimatedNormals = false;
    MeshSettings mesh;
    // The mesh gs2mesh made: its files (main .obj first, then the LODs), and the inputs it was
    // made from, to tell when it is out of date.
    QStringList meshFiles;
    QString meshInputs;
    // Which mesh an .sgs carries: -1 the lowest LOD, 0 the main one, n the nth LOD; and the
    // longest side of its textures there (0: as made).
    int sgsMesh = -1, sgsTextureSize = 2048;
    bool sgsIncludesMesh = true;
    Camera camera;
    bool cameraSet = false;

    QJsonObject json(const QString &projectPath) const;
    static SgsProject fromJson(const QJsonObject &, const QString &projectPath); // throws std::runtime_error
    // What the mesh depends on: the PLY (path, size, time), the COLMAP folder and the settings.
    QString currentMeshInputs() const;
    bool meshOutOfDate() const { return !meshFiles.isEmpty() && meshInputs != currentMeshInputs(); }
    // The .obj of the mesh an .sgs carries, or empty.
    QString sgsMeshFile() const;
};
