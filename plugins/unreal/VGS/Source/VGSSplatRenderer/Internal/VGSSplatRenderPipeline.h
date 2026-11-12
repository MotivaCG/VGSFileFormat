// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Templates/RefCounting.h"
#include "VGSSplatCommon.h"
#include "RenderGraphFwd.h"
#include "RHIShaderPlatform.h"

class FRDGBuilder;
class FSceneView;
class FRHITexture;
class FRDGPooledBuffer;
struct FScreenPassRenderTarget;

class FVGSSplatRenderProxy
{
public:
	FVGSSplatRenderProxy() = default;

	int32 SplatID = INDEX_NONE;
	uint32 NumSplats = 0;
	uint32 NumLOD0Splats = 0;
	int32  SHDegree = 0;
	EVGSSplatCoordinateSpace SHCoordinateSpace = EVGSSplatCoordinateSpace::LH_FRU;

	TRefCountPtr<FRDGPooledBuffer> GpuBuffer; // set on render thread in RegisterComponent
	uint32 GpuDataStride = 0;
	uint32 BufferFormat = 0;

	// Cluster table, null with a count of zero when the asset was built without clustering
	TRefCountPtr<FRDGPooledBuffer> ClusterBuffer; // set on render thread in RegisterComponent
	uint32 NumClusters = 0;

	FVector3f PositionScale = FVector3f::One();
	FVector3f PositionBias  = FVector3f::Zero();
	FVector3f ScaleScale	= FVector3f::One();
	FVector3f ScaleBias		= FVector3f::Zero();
	FVector3f SHScale		= FVector3f::One();
	FVector3f SHBias		= FVector3f::Zero();
	FVector4f ColorScale	= FVector4f::One();
	FVector4f ColorBias		= FVector4f::Zero();

	FMatrix44f LocalToWorld = FMatrix44f::Identity;
	FMatrix44f WorldToLocal = FMatrix44f::Identity;

	FBox3f LocalBounds = FBox3f(EForceInit::ForceInit);

	float PointScaleFactor = 1.0f;
	float OpacityScale = 1.0f;

	bool bVisible = true;
	// VGS CHANGE: a scale on the splats' linear colour, applied when they are composited
	// (VGSSplatInverseToneCurvePostBlend.usf). VGS's Intensity (EV) is 2^EV.
	float Intensity = 1.0f;
	// VGS CHANGE: blend splats in sRGB values, as most captures are trained and as the VGS
	// viewer draws them, rather than in linear light as Epic's renderer does.
	bool bBlendInSRGB = true;
	// VGS CHANGE: a grade on the capture's display colour (its sRGB values), applied when
	// it is composited: gamma, contrast about 0.5 and saturation, then a tint that multiplies
	// those values. (1, 1, 1) leaves it alone.
	FVector3f Grade = FVector3f(1.0f, 1.0f, 1.0f);
	FVector3f Tint = FVector3f(1.0f, 1.0f, 1.0f);

	// VGS CHANGE: lit by the scene. The splats are drawn after Unreal's base pass into the
	// GBuffer, as an opaque Default Lit surface (VGSSplatGBuffer.usf), so every light, shadow,
	// skylight, Lumen and fog reaches them; the rest of the settings above that are about
	// compositing (inverse tone curve, Intensity) do not apply. The colour is the base colour,
	// and SelfIllumination of it is kept as emissive, so shade never turns the capture black.
	bool bLit = false;
	float SelfIllumination = 0.1f;
	float Roughness = 0.8f;
	float Specular = 0.5f;
	float NormalSmoothing = 2.0f; // VGS CHANGE: the lit pass's normal filter radius, in Unreal units
	// Optional: per splat one word, the outward normal in the capture's axes (octahedral, 16
	// bits a component biased so that 0 means none). Without it, or for a splat with 0, the
	// normal is the splat's shortest axis, turned to face the camera.
	TRefCountPtr<FRDGPooledBuffer> NormalBuffer;
	// VGS CHANGE: NormalBuffer holds directions whose side is not known (a splat's shortest
	// axis, as PGSR takes it): each is turned to face the camera, as the shortest axis is.
	bool bNormalsFaceCamera = false;
	float InverseToneCurveStrength = 0.0f;
	bool bInvertExpandGamutLUT = true;
	// VGS CHANGE: casts a shadow (its component's Cast Shadow), for the renderer's own shadow
	// map (VGSSplatLightShadow.h); Unreal's shadow maps take it through its scene proxy.
	bool bCastShadow = false;

	bool HasClusters() const { return NumClusters > 0 && ClusterBuffer.IsValid(); }
	uint32 GetNumLOD0Splats() const { return NumLOD0Splats > 0 ? FMath::Min(NumLOD0Splats, NumSplats) : NumSplats; }
};

struct FVGSSplatRenderItem
{
	const FVGSSplatRenderProxy* Proxy = nullptr;
	FRHITexture* InverseToneCurveLUTRHI = nullptr;
};

namespace VGSSplat
{
	class FRenderStats;

	bool IsHardwareRasterization();
	bool IsHardwareRasterization(EShaderPlatform ShaderPlatform);
	bool IsGPUDrivenPipeline();

	void Render(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		TConstArrayView<FVGSSplatRenderItem> RenderItems,
		FRenderStats& RenderStats,
		const FScreenPassRenderTarget& Output,
		FRDGTextureRef SceneDepthTexture,
		FRDGTextureRef SceneVelocityTexture
	);

	// VGS CHANGE: proxies with bLit, drawn into the GBuffer after the base pass
	// (FVGSSplatViewExtension::PostRenderBasePassDeferred_RenderThread). Where SupportsLit is
	// false (no deferred GBuffer of the usual layout: forward shading, Substrate, mobile), they
	// are drawn in the post-process pass like the others, self-illuminated.
	struct FLitTargets
	{
		FRDGTextureRef SceneColor = nullptr;
		FRDGTextureRef SceneDepth = nullptr;
		FRDGTextureRef GBufferA = nullptr;
		FRDGTextureRef GBufferB = nullptr;
		FRDGTextureRef GBufferC = nullptr;
		// With Substrate's own material buffer, that instead of GBuffer A-C: its array (the
		// first three slices are render targets), a UAV for the rest, and its top layer.
		bool bSubstrateMaterial = false;
		FRDGTextureRef SubstrateMaterial = nullptr;
		FRDGTextureUAVRef SubstrateMaterialUAV = nullptr;
		FRDGTextureRef SubstrateTopLayer = nullptr;
		uint32 SubstrateMaxBytesPerPixel = 0;
		bool bSubstrateRoughDiffuse = false;
		FIntRect ViewRect;
	};
	bool SupportsLit(const FSceneView& View);
	void RenderLit(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		TConstArrayView<FVGSSplatRenderItem> RenderItems,
		FRenderStats& RenderStats,
		const FLitTargets& Targets
	);
}
