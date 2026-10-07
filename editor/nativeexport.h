#pragma once
#include "exportcapture.h"

struct NativeChunkPlan { size_t sourceIndex=0; int first=0,intervals=0; };
bool supportsNativeTransform(const Project &);
std::vector<NativeChunkPlan> nativeExportPlan(const vgs::Header &, int first, int frames);
vgs::DecodedChunk assembleNativeChunk(vgs::DecodedChunk);
void setNativeShDegree(vgs::DecodedChunk &, int sourceDegree, int targetDegree);
vgs::DecodedChunk editNativeChunk(vgs::DecodedChunk, const NativeChunkPlan &, const Project &,
                                 ExportResult *, const ExportProgress &, const vgs::DecodedChunk *classificationSource=nullptr);
