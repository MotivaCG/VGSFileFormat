// VGS: not Epic's. The splat renderer's own shadow of the scene's directional light.
#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"

class FLightSceneInfo;
class FScene;
class FSceneView;
class FVGSSplatRenderProxy;
struct FPostProcessingInputs;

namespace VGSSplat
{
	/**
	 * The light the splats' own shadow map is drawn from: of the scene's directional lights
	 * that cast dynamic shadows, the atmosphere's sun (Atmosphere Sun Light, index 0) if one
	 * is, otherwise the brightest; null if none casts shadows.
	 */
	const FLightSceneInfo* FindMainLight(const FScene& Scene);

	/** The way the main light goes (unit). */
	FVector GetMainLightDirection(const FLightSceneInfo& Light);

	/**
	 * With r.VGSSplat.Shadow.Method 0 or 1: draws the casting proxies (bCastShadow) from the
	 * main directional light (FindMainLight) into a shadow map of their own, as Gaussians with
	 * their real opacity,
	 * and takes that light's direct contribution off the lit scene behind them
	 * (Shaders/Private/SplatRenderer/VGSSplatLightShadow.usf). Before post-processing, so the
	 * captures that are not lit (composited later) are never darkened; lit ones only with
	 * r.VGSSplat.Shadow.Self 1.
	 */
	void RenderLightShadow(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs, TConstArrayView<const FVGSSplatRenderProxy*> Proxies);
}
