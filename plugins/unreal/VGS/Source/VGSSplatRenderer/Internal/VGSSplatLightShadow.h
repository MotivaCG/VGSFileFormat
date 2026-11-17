// VGS: not Epic's. The splat renderer's own shadow of the scene's directional light.
#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"

class FLightSceneInfo;
class FScene;
class FSceneView;
class FVGSSplatRenderProxy;
class FTextureRenderTargetResource;

namespace VGSSplat
{
	// One world-space grid shared by the views of a family. Resources belong to the world
	// subsystem and are made externally readable before UE evaluates the light function.
	struct FDeepShadowFrame
	{
		FTextureRenderTargetResource* Atlas = nullptr;
		FTextureRenderTargetResource* Receivers = nullptr;
		FVector Origin = FVector::ZeroVector;
		FVector X = FVector::ForwardVector, Y = FVector::RightVector, Z = FVector::UpVector;
		FVector Extent = FVector::OneVector;
		int32 Resolution = 512;
		bool IsValid() const { return Atlas != nullptr && Receivers != nullptr; }
	};
	void RenderDeepShadow(FRDGBuilder& GraphBuilder, const FDeepShadowFrame& Frame, TConstArrayView<const FVGSSplatRenderProxy*> Proxies);
	FRDGTextureRef RegisterReceiverMask(FRDGBuilder& GraphBuilder, const FDeepShadowFrame& Frame);
	/**
	 * The light the splats' own shadow map is drawn from: of the scene's directional lights
	 * that cast dynamic shadows, the atmosphere's sun (Atmosphere Sun Light, index 0) if one
	 * is, otherwise the brightest; null if none casts shadows.
	 */
	const FLightSceneInfo* FindMainLight(const FScene& Scene);

	/** The way the main light goes (unit). */
	FVector GetMainLightDirection(const FLightSceneInfo& Light);

}
