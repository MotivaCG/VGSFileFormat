#pragma once
#include <algorithm>
#include "exportcapture.h"

struct NativeChunkPlan { size_t sourceIndex=0; int first=0,intervals=0; };
bool supportsNativeTransform(const Project &);
std::vector<NativeChunkPlan> nativeExportPlan(const vgs::Header &, int first, int frames);
vgs::DecodedChunk assembleNativeChunk(vgs::DecodedChunk);
void setNativeShDegree(vgs::DecodedChunk &, int sourceDegree, int targetDegree);
// `sampleModels`, when given, holds the world model at each of the plan's samples
// sourceStartFrame is the source frame of the plan's first sample: animated crops are
// evaluated there, sample by sample, like animated transforms.
// `pruned`, when given, is Prune low contribution's decision for the source chunk's records.
// (intervals + 1) for a capture that moves: modifiers then see where every splat really is
// at that instant, while project.transform is still what is baked into the data.
// Bake anti-aliasing's opacity factor, from each axis's s/sqrt(s^2+b^2): the product over
// the two largest axes. A capture trained with screen-space anti-aliasing is faded by the
// footprint a view projects, which for most views spans a splat's two largest axes: a flat
// splat seen face on keeps its opacity, a needle fades as seen from the side, a speck as a
// speck. Mip-Splatting's full 3D ratio would fade every flat splat by its thickness.
inline float bakedOpacityFactor(const float ratios[3]) {
    const float smallest=std::min({ratios[0],ratios[1],ratios[2]});
    return smallest>0 ? ratios[0]*ratios[1]*ratios[2]/smallest : std::max({ratios[0]*ratios[1],ratios[0]*ratios[2],ratios[1]*ratios[2]});
}
vgs::DecodedChunk editNativeChunk(vgs::DecodedChunk, const NativeChunkPlan &, const Project &,
                                 ExportResult *, const ExportProgress &, const vgs::DecodedChunk *classificationSource=nullptr,
                                 const std::vector<QMatrix4x4> *sampleModels=nullptr,int sourceStartFrame=0,
                                 const std::vector<uint8_t> *pruned=nullptr);
