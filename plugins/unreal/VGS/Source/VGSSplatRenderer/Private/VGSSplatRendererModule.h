// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Templates/SharedPointer.h"

class FRHITexture;
namespace VGSSplat
{
    class FInverseToneCurveLUTCache;
    class FRenderStats;
}

class FVGSSplatRendererModule : public IModuleInterface
{
public:
	// Out-of-line so the InverseToneCurveLUTCache TSharedPtr can hold a forward-declared type.
	virtual ~FVGSSplatRendererModule() override;

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	// Returns the requested inverse-tone-curve LUT's RHI once resident, or nullptr while its async load is
	// still in flight (or the cache is unavailable). Render-thread only; gate the post-blend pass on non-null.
	FRHITexture* GetInverseToneCurveLUTRHI_RenderThread(bool bInvertExpandGamut);

	VGSSplat::FRenderStats& GetRenderStats_RenderThread() { return *RenderStats; }

private:
	TSharedPtr<VGSSplat::FInverseToneCurveLUTCache, ESPMode::ThreadSafe> InverseToneCurveLUTCache;
	TUniquePtr<VGSSplat::FRenderStats> RenderStats;
};
