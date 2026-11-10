// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/SortedMap.h"
#include "RHIFwd.h"
#include "VGSSplatInverseToneCurveLUT.h"
#include "Templates/SharedPointer.h"

class FRHITexture;

namespace VGSSplat
{
	// VGS CHANGE: the LUTs are built from the plugin's Resources/InverseToneCurve files (Epic's
	// LUT assets, dumped and packed by plugins/unreal/tools/pack_inverse_tone_curve_luts.py),
	// not loaded as Epic's UVolumeTexture assets: those are saved by Unreal 5.9 and are part of
	// Epic's GaussianSplat plugin, which the copy no longer needs. Render thread only.
	class FInverseToneCurveLUTCache : public TSharedFromThis<FInverseToneCurveLUTCache, ESPMode::ThreadSafe>
	{
	public:
		FInverseToneCurveLUTCache();
		~FInverseToneCurveLUTCache();

		// Returns the variant's 3D LUT texture, built on the first request; nullptr if its file
		// is missing or unreadable (the post-blend pass then skips the inverse tone curve).
		FRHITexture* GetRHI_RenderThread(bool bInvertExpandGamut);

	private:
		struct FSlot
		{
			FTextureRHIRef Texture;
			bool bTried = false;
		};
		TSortedMap<EInverseToneCurveLUTVariant, FSlot> Slots;
	};
}
