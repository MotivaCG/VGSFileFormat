// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of PGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#include "meshtangents.h"
#include "meshio.h"

#include <array>
#include <cstdint>

// mikktspace.hh uses Blender's uint and branch hints
using uint = unsigned int;
#ifndef LIKELY
#define LIKELY(x) (x)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) (x)
#endif
#include "mikktspace.hh"

namespace {

struct MikkMesh {
    const MeshData &m;
    std::vector<float> &out;
    uint GetNumFaces() const {return uint(m.triangles());}
    uint GetNumVerticesOfFace(uint) const {return 3;}
    uint corner(uint f,uint v) const {return m.indices[size_t(f)*3+v];}
    mikk::float3 GetPosition(uint f,uint v) const {const float *p=&m.positions[size_t(corner(f,v))*3];return {p[0],p[1],p[2]};}
    mikk::float3 GetNormal(uint f,uint v) const {const float *n=&m.normals[size_t(corner(f,v))*3];return mikk::float3(n[0],n[1],n[2]).normalize();}
    mikk::float3 GetTexCoord(uint f,uint v) const {const float *t=&m.uvs[size_t(corner(f,v))*2];return {t[0],t[1],1.0f};}
    bool has_uv() const {return true;}
    void SetTangentSpace(uint f,uint v,mikk::float3 t,bool orientPreserving) {
        // a vertex shared by faces gets the same tangent from each (MikkTSpace splits vertices, not faces)
        float *o=&out[size_t(corner(f,v))*4];
        o[0]=t.x;o[1]=t.y;o[2]=t.z;o[3]=orientPreserving ? 1.f : -1.f;
    }
};

}

std::vector<float> meshTangents(const MeshData &mesh) {
    std::vector<float> out(mesh.vertices()*4,0.f);
    for (size_t i=0;i<mesh.vertices();++i) out[i*4]=1.f,out[i*4+3]=1.f;
    MikkMesh m{mesh,out};
    mikk::Mikktspace<MikkMesh> generator(m);
    generator.genTangSpace();
    return out;
}
