// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Templates/RefCounting.h"
#include "GaussianSplatCommon.h"
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
	EGaussianSplatCoordinateSpace SHCoordinateSpace = EGaussianSplatCoordinateSpace::LH_FRU;

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
	float InverseToneCurveStrength = 0.0f;
	bool bInvertExpandGamutLUT = true;

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
}
