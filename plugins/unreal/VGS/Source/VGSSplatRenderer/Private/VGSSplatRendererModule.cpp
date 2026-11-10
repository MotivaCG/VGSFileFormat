// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#include "VGSSplatRendererModule.h"

#include "VGSSplatInverseToneCurveLUTCache.h"
#include "VGSSplatRenderStats.h"
// VGS CHANGE: no FGaussianSplatRendererRegistry registration. Epic's UGaussianSplatComponents
// are drawn by Epic's renderer; this copy draws only the sources registered with it.

FVGSSplatRendererModule::~FVGSSplatRendererModule() = default;

void FVGSSplatRendererModule::StartupModule()
{
	InverseToneCurveLUTCache = MakeShared<VGSSplat::FInverseToneCurveLUTCache, ESPMode::ThreadSafe>();
	RenderStats = MakeUnique<VGSSplat::FRenderStats>();
}

void FVGSSplatRendererModule::ShutdownModule()
{
	InverseToneCurveLUTCache.Reset();

	RenderStats.Reset();
}

FRHITexture* FVGSSplatRendererModule::GetInverseToneCurveLUTRHI_RenderThread(bool bInvertExpandGamut)
{
	return InverseToneCurveLUTCache ? InverseToneCurveLUTCache->GetRHI_RenderThread(bInvertExpandGamut) : nullptr;
}

IMPLEMENT_MODULE(FVGSSplatRendererModule, VGSSplatRenderer)
