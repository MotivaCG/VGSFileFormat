// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#pragma once
#include <vector>

struct MeshData;

// MikkTSpace tangents of the mesh, per vertex: x, y, z and the bitangent sign w (bitangent = w * cross(normal,
// tangent)). The same tangent space gs2mesh bakes its normal maps in (gs2mesh's vendored MikkTSpace, on the OBJ's
// texture coordinates, v up), so a viewer that uses them decodes the normal map as it was baked; a glTF keeps
// them unchanged although its texture coordinates count v down.
std::vector<float> meshTangents(const MeshData &mesh);
