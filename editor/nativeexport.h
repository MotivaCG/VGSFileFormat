#pragma once
#include "exportcapture.h"

struct NativeChunkPlan { size_t sourceIndex=0; int first=0,intervals=0; };
bool supportsNativeTransform(const Project &);
std::vector<NativeChunkPlan> nativeExportPlan(const vgs::Header &, int first, int frames);
vgs::DecodedChunk assembleNativeChunk(vgs::DecodedChunk);
void setNativeShDegree(vgs::DecodedChunk &, int sourceDegree, int targetDegree);
// `sampleModels`, when given, holds the world model at each of the plan's samples
// sourceStartFrame is the source frame of the plan's first sample: animated crops are
// evaluated there, sample by sample, like animated transforms.
// (intervals + 1) for a capture that moves: modifiers then see where every splat really is
// at that instant, while project.transform is still what is baked into the data.
vgs::DecodedChunk editNativeChunk(vgs::DecodedChunk, const NativeChunkPlan &, const Project &,
                                 ExportResult *, const ExportProgress &, const vgs::DecodedChunk *classificationSource=nullptr,
                                 const std::vector<QMatrix4x4> *sampleModels=nullptr,int sourceStartFrame=0);
