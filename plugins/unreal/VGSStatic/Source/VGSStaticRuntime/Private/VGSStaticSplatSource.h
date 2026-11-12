#pragma once

#include "CoreMinimal.h"
#include "VGSStaticSplatRenderPipeline.h" // FVGSStaticSplatRenderProxy (VGSStaticSplatRenderer)

class FRDGBuilder;
class FSceneView;
class FVGSStaticSplatGpu;

/**
 * How a component draws its asset: plain values only, zeroed before they are set, so two of
 * them compare byte for byte (the component sends one only when it changed).
 */
struct FVGSStaticLook
{
	FMatrix44f LocalToWorld; // file axes and units to world
	FMatrix44f WorldToLocal;
	uint32 bVisible;
	uint32 bSphericalHarmonics;
	float PointScaleFactor;
	float OpacityScale;
	float InverseToneCurveStrength;
	uint32 bInvertExpandGamut;
	float Intensity;
	uint32 bBlendInSRGB;
	FVector3f Grade; // gamma, contrast, saturation
	FVector3f Tint; // sRGB values
	uint32 bLit;
	float SelfIllumination;
	float Roughness;
	float Specular;
	float NormalSmoothing;
	uint32 bCastShadow;
};

/**
 * One component as the splat renderer sees it: a source of one proxy, registered with
 * VGSStaticSplat::RegisterExternalProxySource, pointing at the asset's shared GPU copy.
 * Render thread only, apart from construction; the binding expires with this object.
 */
class FVGSStaticSplatSource : public TSharedFromThis<FVGSStaticSplatSource, ESPMode::ThreadSafe>
{
public:
	void SetLook_RenderThread(const FVGSStaticLook& InLook);
	void SetGpu_RenderThread(TSharedPtr<FVGSStaticSplatGpu, ESPMode::ThreadSafe> InGpu);

	void Collect_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, TArray<const FVGSStaticSplatRenderProxy*>& OutProxies);

private:
	FVGSStaticSplatRenderProxy Proxy;
	FVGSStaticLook Look = {};
	TSharedPtr<FVGSStaticSplatGpu, ESPMode::ThreadSafe> Gpu;
};
