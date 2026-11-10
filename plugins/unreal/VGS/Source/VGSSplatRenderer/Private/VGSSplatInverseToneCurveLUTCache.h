// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/SortedMap.h"
#include "Engine/VolumeTexture.h"
#include "VGSSplatInverseToneCurveLUT.h"
#include "Templates/SharedPointer.h"
#include "UObject/StrongObjectPtr.h"

class FRHITexture;

namespace VGSSplat
{
	/** Render-thread-owned state for cached inverse-tone-curve LUT RHI references. */
	struct FInverseToneCurveLUTRenderThreadState;

	// Loads and caches the baked inverse-tone-curve LUTs. One shared, module-owned instance serves every view.
	// Render-thread only, apart from the one-shot game-thread asset load.
	// Owned via TSharedPtr so the game-thread load task can pin a weak pointer to the cache before touching it.
	class FInverseToneCurveLUTCache : public TSharedFromThis<FInverseToneCurveLUTCache, ESPMode::ThreadSafe>
	{
	public:
		FInverseToneCurveLUTCache();
		~FInverseToneCurveLUTCache();

		// Returns the variant's current 3D LUT texture, or nullptr while the first-request load is in flight.
		FRHITexture* GetRHI_RenderThread(bool bInvertExpandGamut);

	private:
		/** Keeps loaded LUT UObjects alive on the game thread. */
		TSortedMap<EInverseToneCurveLUTVariant, TStrongObjectPtr<UVolumeTexture>> Assets;

		/** Stores render-thread LUT lookup state and is released on the render thread. */
		TSharedPtr<FInverseToneCurveLUTRenderThreadState, ESPMode::ThreadSafe> RenderThreadState;
	};
}
