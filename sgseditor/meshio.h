// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#pragma once
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QVector3D>
#include <cstdint>
#include <vector>

// A triangle mesh as gs2mesh writes it (an OBJ with its MTL): one vertex per position,
// texture coordinate and normal used together, and the textures its material names.
struct MeshData {
    std::vector<float> positions, normals, uvs; // 3, 3 and 2 per vertex; uv as in OBJ, v up
    std::vector<uint32_t> indices;              // 3 per triangle
    // Per vertex, the OBJ position (v) it uses: the vertices of a UV seam share one. And how many
    // positions the OBJ has, so that a mesh written back keeps their order (writeObj).
    std::vector<uint32_t> positionIds;
    size_t positionCount = 0;
    QString albedo, normalMap, occlusion;       // the material's textures, absolute; empty for none
                                                // (occlusion: ambient occlusion, map_Ka)
    QString material;                           // the .mtl it names, absolute; empty for none
    size_t vertices() const { return positions.size()/3; }
    size_t triangles() const { return indices.size()/3; }
    QVector3D minimum() const;
    QVector3D maximum() const;
};
// Throws std::runtime_error. Normals missing from the file are worked out from the faces.
MeshData readObj(const QString &path);
// A triangle mesh as a binary little-endian PLY (gs2mesh's <name>_dense.ply: float x, y, z and any other
// float or uchar vertex properties, faces as uchar-counted int lists): positions and normals worked out
// from the faces, no texture coordinates. Throws std::runtime_error.
MeshData readPlyMesh(const QString &path);
// The mesh as gs2mesh writes one, which it reads back (for LODs): its positions in the order of
// positionIds (one per OBJ position, with its normal), a texture coordinate per vertex, faces
// v/vt/vn, and `mtl` (a file name beside it) as its material, `materialName` in it. Throws
// std::runtime_error.
void writeObj(const QString &path, const MeshData &mesh, const QString &mtl, const QString &materialName);
// The name of the first material an .mtl defines (newmtl), or "material_0".
QString materialName(const QString &mtl);
// The albedo an .mtl names (map_Kd), absolute; empty for none.
QString materialAlbedo(const QString &mtl);
// The .mtl with its albedo (map_Kd) named `file`, beside it. Throws std::runtime_error.
void setMaterialAlbedo(const QString &mtl, const QString &file);

// The mesh as one glTF 2.0 binary (.glb), in the same space and units (Y up, metres, as
// the captures are): positions, normals and texture coordinates, its albedo as the base
// colour (JPEG), its normal map (PNG, tangent space) and its ambient occlusion (PNG), all
// scaled to `textureSize` on their longer side when larger (0 keeps them). A matte
// dielectric, as the splats are lit.
struct GlbOptions {
    int textureSize = 2048;
    int jpegQuality = 90;
    bool normalMap = true;
    bool occlusion = true;
};
QByteArray writeGlb(const MeshData &, const QImage &albedo, const QImage &normalMap, const GlbOptions & = {},
                    const QImage &occlusion = QImage());

// What a .glb holds, read back: for tests and for the export's report.
struct GlbInfo {
    QString error; // empty when it is a valid glTF 2.0 binary
    size_t vertices = 0, triangles = 0;
    int images = 0, materials = 0;
    QJsonObject json;
    QByteArray binary;
};
GlbInfo inspectGlb(const QByteArray &);
