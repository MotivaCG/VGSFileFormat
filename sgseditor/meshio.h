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
    QString albedo, normalMap;                  // the material's textures, absolute; empty for none
    size_t vertices() const { return positions.size()/3; }
    size_t triangles() const { return indices.size()/3; }
    QVector3D minimum() const;
    QVector3D maximum() const;
};
// Throws std::runtime_error. Normals missing from the file are worked out from the faces.
MeshData readObj(const QString &path);

// The mesh as one glTF 2.0 binary (.glb), in the same space and units (Y up, metres, as
// the captures are): positions, normals and texture coordinates, its albedo as the base
// colour (JPEG) and its normal map (PNG, tangent space), both scaled to `textureSize` on
// their longer side when larger (0 keeps them). A matte dielectric, as the splats are lit.
struct GlbOptions {
    int textureSize = 2048;
    int jpegQuality = 90;
    bool normalMap = true;
};
QByteArray writeGlb(const MeshData &, const QImage &albedo, const QImage &normalMap, const GlbOptions & = {});

// What a .glb holds, read back: for tests and for the export's report.
struct GlbInfo {
    QString error; // empty when it is a valid glTF 2.0 binary
    size_t vertices = 0, triangles = 0;
    int images = 0, materials = 0;
    QJsonObject json;
    QByteArray binary;
};
GlbInfo inspectGlb(const QByteArray &);
