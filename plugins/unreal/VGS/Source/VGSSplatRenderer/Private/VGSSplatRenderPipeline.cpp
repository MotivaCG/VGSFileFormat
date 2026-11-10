// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#include "VGSSplatRenderPipeline.h"

#include "VGSSplatCluster.h"
#include "VGSSplatInverseToneCurveLUT.h"
#include "VGSSplatRenderStats.h"

#include "SceneView.h"
#include "ScreenPass.h"
#include "GPUSort.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "ShaderCompilerCore.h"
#include "RenderGraphUtils.h"
#include "RenderGraphResources.h"
#include "PixelShaderUtils.h"
#include "RHIStaticStates.h"
#include "CommonRenderResources.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "PixelFormat.h"
#include "RHIGlobals.h"
#include "BlueNoise.h"
#include "SystemTextures.h"
#include "SceneRendering.h"
#include "RenderUtils.h" // VGS CHANGE: IsForwardShadingEnabled, Substrate::IsSubstrateEnabled
#include "Runtime/Launch/Resources/Version.h" // VGS CHANGE: the copy builds against Unreal 5.6 and later

// VGS CHANGE: what Epic's 5.9 renderer uses that older engines lack. Before 5.9 there is no
// SortGPUBuffersIndirect (the GPU-driven pipeline sorts a count the GPU wrote), and no
// VET_UInt2 (vertex attribute fetch, for platforms whose vertex shaders cannot read buffers):
// the GPU-driven pipeline is off there, and vertex attribute fetch is never chosen, which on
// Win64, the only platform the plugins build for, it is not anyway.
#define VGSSPLAT_ENGINE_5_9 (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 9))

DECLARE_GPU_STAT(VGSSplat);

static TAutoConsoleVariable<int32> CVarVGSSplatDebug(
	TEXT("r.VGSSplat.Debug"),
	0,
	TEXT("Enables debugging environment (default = 0)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatTileSize(
	TEXT("r.VGSSplat.TileSize"),
	8,
	TEXT("The dimension for tiled rasterization (default = 8)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatMaxSplatOverlap(
	TEXT("r.VGSSplat.MaxSplatOverlap"),
	16,
	TEXT("The maximum number of splat overlap to support (default = 16)\n")
	TEXT("Presented as the high-water mark for splat budget allocation: NumSplats * MaxSplatOverlap"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatHardwareRasterization(
	TEXT("r.VGSSplat.HardwareRasterization"),
	1,
	TEXT("Use hardware rasterization (VS+PS) instead of tiled compute (default = 1)"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarVGSSplatGPUDrivenPipeline(
	TEXT("r.VGSSplat.Debug.GPUDrivenPipeline"),
	1,
	TEXT("Enables GPU-Driven Pipeline (default = 1)\n")
	TEXT("Requires debug mode to toggle"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarVGSSplatBlueNoise(
	TEXT("r.VGSSplat.BlueNoise"),
	1,
	TEXT("Determines if Blue Noise is used (default = 1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatDepth(
	TEXT("r.VGSSplat.Depth"),
	0,
	TEXT("Determines if Gaussian Splats write to the depth buffer:\n")
	TEXT("0 = off (default)\n")
	TEXT("1 = hardware depth\n")
	TEXT("2 = stochastic depth\n")
	TEXT("3 = expected depth"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatStochasticDepthFormat(
	TEXT("r.VGSSplat.StochasticDepthFormat"),
	1,
	TEXT("Backing format for depth:\n")
	TEXT("0 = 8-bit unorm\n")
	TEXT("1 = 16-bit unorm (default)\n")
	TEXT("2 = 16-bit floating point\n")
	TEXT("3 = 32-bit floating point\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatVelocity(
	TEXT("r.VGSSplat.Velocity"),
	0,
	TEXT("Write into the scene velocity buffer (default = 0)\n")
	TEXT("Velocity writes require depth writes to also be enabled"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarVGSSplatHardwareRasterizationOpacityThreshold(
	TEXT("r.VGSSplat.HardwareRasterization.OpacityThreshold"),
	0.001f,
	TEXT("Minimum alpha for a fragment to be accepted (default = 0.001)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarVGSSplatShadingOpacityThreshold(
	TEXT("r.VGSSplat.Shading.OpacityThreshold"),
	0.0f,
	TEXT("Minimum alpha for a Gaussian to be shaded (default = 0)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatShadingFrustumCull(
	TEXT("r.VGSSplat.Debug.FrustumCull"),
	1,
	TEXT("Apply frustum culling during shading (default = 1)\n")
	TEXT("Requires debug mode to toggle"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatClusterCulling(
	TEXT("r.VGSSplat.Debug.ClusterCulling"),
	1,
	TEXT("Bundle splats into clusters and apply view-based culling (default = 1)\n")
	TEXT("Requires debug mode to toggle"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

static TAutoConsoleVariable<float> CVarVGSSplatExpectedDepthScale(
	TEXT("r.VGSSplat.ExpectedDepthScale"),
	100.0f,
	TEXT("The scaling divisor when computing expected depth (default = 100)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatSortQuality(
	TEXT("r.VGSSplat.SortQuality"),
	3,
	TEXT("Depth sort precision, lower bit-count leads to faster sorting with less stability\n")
	TEXT("0 = Reference, 32-bit precision \n")
	TEXT("1 = Low, 16-bit precision \n")
	TEXT("2 = Medium, 20-bit precision\n")
	TEXT("3 = High, 24-bit precision (default)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatGatherShadedSplats(
	TEXT("r.VGSSplat.GatherShadedSplats"),
	-1,
	TEXT("Gather shaded splats into sorted order. Required for ES 3.1, optional for SM5\n")
	TEXT("-1 = auto-detect for the current platform (default)\n")
	TEXT(" 0 = off\n")
	TEXT(" 1 = on"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatVertexAttributeFetch(
	TEXT("r.VGSSplat.VertexAttributeFetch"),
	-1,
	TEXT("Use per-instance vertex attributes instead of SRV input during hardware rasterization\n")
	TEXT("Required for ES 3.1, optional for SM5\n")
	TEXT("-1 = auto-detect for the current platform (default)\n")
	TEXT(" 0 = off\n")
	TEXT(" 1 = on"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatEmulateGLES31(
	TEXT("r.VGSSplat.EmulateGLES31"),
	0,
	TEXT("Emulate execution for GL ES 3.1 (default=0)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatLOD(
	TEXT("r.VGSSplat.LOD"),
	-1,
	TEXT("Determines active LOD setting\n")
	TEXT("-1: Screen-based LOD (default)\n")
	TEXT(" 0: LOD Level 0\n")
	TEXT(" 1: LOD Level 1\n")
	TEXT(" X: LOD Level X"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarVGSSplatErrorThreshold(
	TEXT("r.VGSSplat.ErrorThreshold"),
	1.0f,
	TEXT("Error Threshold for LOD in pixels (default=1)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatFalloff(
	TEXT("r.VGSSplat.Falloff"),
	1,
	TEXT("Gaussian falloff function used during rasterization\n")
	TEXT("0: Standard Gaussian falloff, truncated at the boundary\n")
	TEXT("1: C0 continuous, peak normalized. Drives falloff to 0 at the boundary, and normalized based on peak intensity (default)\n")
	TEXT("2: C0 continuous, mass normalized. Drives falloff to 0 at the boundary, and normalized based on integrated energy\n")
	TEXT("3: C1 continuous, peak normalized. Drives falloff and slope to 0 at the boundary, and normalized based on peak intensity\n")
	TEXT("4: C1 continuous, mass normalized. Drives falloff and slope to 0 at the boundary, and normalized based on integrated energy"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatDownsampleFactor(
	TEXT("r.VGSSplat.DownsampleFactor"),
	1,
	TEXT("Integer divisor applied to rendered viewport (default = 1)"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

namespace
{
	int32 GetTileSize()
	{
		return FMath::Max(CVarVGSSplatTileSize.GetValueOnRenderThread(), 1);
	}

	int32 GetMaxSplatOverlap()
	{
		return FMath::Max(CVarVGSSplatMaxSplatOverlap.GetValueOnRenderThread(), 1);
	}

	bool IsDebug()
	{
		return CVarVGSSplatDebug.GetValueOnRenderThread() != 0;
	}

	enum class EVGSSplatDepthMode : uint8
	{
		Off = 0,
		Hardware = 1,
		Stochastic = 2,
		Expected = 3
	};

	EVGSSplatDepthMode GetDepthMode()
	{
		return static_cast<EVGSSplatDepthMode>(FMath::Clamp(CVarVGSSplatDepth.GetValueOnRenderThread(), 0, 3));
	}

	bool IsBlueNoiseEnabled()
	{
		return CVarVGSSplatBlueNoise.GetValueOnRenderThread() != 0;
	}

	EPixelFormat GetStochasticDepthFormat()
	{
		switch (CVarVGSSplatStochasticDepthFormat.GetValueOnRenderThread())
		{
		case 0:  return PF_G8;
		case 1:  return PF_G16;
		case 2:  return PF_R16F;
		default: return PF_R32_FLOAT;
		}
	}

	bool IsVelocityEnabled()
	{
		return CVarVGSSplatVelocity.GetValueOnRenderThread() != 0;
	}

	float GetHardwareRasterizationOpacityThreshold()
	{
		return FMath::Max(CVarVGSSplatHardwareRasterizationOpacityThreshold.GetValueOnRenderThread(), 0.0f);
	}

	float GetShadingOpacityThreshold()
	{
		return FMath::Max(CVarVGSSplatShadingOpacityThreshold.GetValueOnRenderThread(), 0.0f);
	}

	bool IsFrustumCullingEnabledForShading()
	{
		return CVarVGSSplatShadingFrustumCull.GetValueOnRenderThread() != 0;
	}

	bool IsClusterCullingEnabled()
	{
		return CVarVGSSplatClusterCulling.GetValueOnRenderThread() != 0;
	}

	bool PlatformSupportsWaveOps(EShaderPlatform ShaderPlatform)
	{
		return FDataDrivenShaderPlatformInfo::GetSupportsWaveOperations(ShaderPlatform) == ERHIFeatureSupport::RuntimeGuaranteed;
	}

	int32 GetSortKeyBits(EShaderPlatform ShaderPlatform)
	{
		int32 SortKeyBits = 32;

		if (VGSSplat::IsHardwareRasterization(ShaderPlatform))
		{
			int32 SortKeyMode = CVarVGSSplatSortQuality.GetValueOnRenderThread();
			switch (SortKeyMode)
			{
			case 1:
				SortKeyBits = 16;
				break;
			case 2:
				SortKeyBits = 20;
				break;
			case 3:
				SortKeyBits = 24;
				break;
			//case 0:
			//default:
			//	SortKeyBits = 32;
			}
		}

		return SortKeyBits;
	}

	// Bits set in the radix-sort KeyMask (drives GetGPUSortPassCount): low N bits.
	uint32 GetSortKeyMask(EShaderPlatform ShaderPlatform)
	{
		const int32 Bits = GetSortKeyBits(ShaderPlatform);
		return (Bits >= 32) ? 0xFFFFFFFFu : ((1u << Bits) - 1u);
	}

	float GetExpectedDepthScale()
	{
		return FMath::Max(CVarVGSSplatExpectedDepthScale.GetValueOnRenderThread(), 0.001f);
	}

	enum class EVGSSplatFalloff : uint8
	{
		Standard = 0,
		C0_PeakNormalized = 1,
		C0_MassNormalized = 2,
		C1_PeakNormalized = 3,
		C1_MassNormalized = 4
	};

	EVGSSplatFalloff GetFalloff()
	{
		return static_cast<EVGSSplatFalloff>(FMath::Clamp(CVarVGSSplatFalloff.GetValueOnRenderThread(), 0, 4));
	}

	FVector3f GetFalloffParameters()
	{
		const float RadiusSquared = GAUSSIAN_SPLAT_RADIUS_SQUARED;
		const float E = FMath::Exp(-RadiusSquared);

		// Gaussian function falloff applied in the form:
		// A * exp(-r^2) + B * r^2 + C
		// E = exp(-R^2), R = GAUSSIAN_SPLAT_RADIUS
		switch (GetFalloff())
		{
			// (exp(-r^2) - E) / (1 - E)
			case EVGSSplatFalloff::C0_PeakNormalized:
			{
				const float Scale = 1.0f / (1.0f - E);
				return FVector3f(Scale, 0.0f, -E * Scale);
			}
			// (exp(-r^2) - E) / (1 - E * (1 + R^2))
			case EVGSSplatFalloff::C0_MassNormalized:
			{
				const float Scale = 1.0f / (1.0f - E * (1.0f + RadiusSquared));
				return FVector3f(Scale, 0.0f, -E * Scale);
			}
			// (exp(-r^2) + E * r^2 - E * (1 + R^2)) / (1 - E * (1 + R^2))
			case EVGSSplatFalloff::C1_PeakNormalized:
			{
				const float Scale = 1.0f / (1.0f - E * (1.0f + RadiusSquared));
				return FVector3f(Scale, E * Scale, -E * (1.0f + RadiusSquared) * Scale);
			}
			// (exp(-r^2) + E * r^2 - E * (1 + R^2)) / I, with I = (1 - E) + 0.5 * E * R^4 - E * (1 + R^2) * R^2
			case EVGSSplatFalloff::C1_MassNormalized:
			{
				const float Integral = (1.0f - E) + 0.5f * E * RadiusSquared * RadiusSquared - E * (1.0f + RadiusSquared) * RadiusSquared;
				const float Scale = 1.0f / Integral;
				return FVector3f(Scale, E * Scale, -E * (1.0f + RadiusSquared) * Scale);
			}
			// exp(-r^2)
			default:
				return FVector3f(1.0f, 0.0f, 0.0f);
		}
	}

	bool IsGLES31EmulationEnabled()
	{
		return CVarVGSSplatEmulateGLES31.GetValueOnRenderThread() != 0;
	}

	int32 GetDownsampleFactor()
	{
		return FMath::Clamp(CVarVGSSplatDownsampleFactor.GetValueOnRenderThread(), 1, 8);
	}

	bool IsFeatureLevelES31(EShaderPlatform ShaderPlatform)
	{
		return FDataDrivenShaderPlatformInfo::GetMaxFeatureLevel(ShaderPlatform) == ERHIFeatureLevel::ES3_1;
	}

	// ES 3.1 restricts thread group to 128
	constexpr int32 VGSSplatMaxES31ThreadGroupSize = 128;
	bool PlatformSupportsThreadGroupSize(EShaderPlatform ShaderPlatform, int32 ThreadGroupSize)
	{
		return ThreadGroupSize <= VGSSplatMaxES31ThreadGroupSize || !IsFeatureLevelES31(ShaderPlatform);
	}

	bool PlatformSupportsSoftwareRasterization(EShaderPlatform ShaderPlatform)
	{
		return !IsFeatureLevelES31(ShaderPlatform);
	}

	bool PlatformSupportsStochasticDepth(EShaderPlatform ShaderPlatform)
	{
		return !IsFeatureLevelES31(ShaderPlatform);
	}

	bool SupportsSplatScratch(bool bHWRaster)
	{
		const EPixelFormatCapabilities RequiredCapabilities = EPixelFormatCapabilities::Texture2D
			| EPixelFormatCapabilities::TextureSample
			| EPixelFormatCapabilities::TextureLoad
			| EPixelFormatCapabilities::RenderTarget
			| (bHWRaster ? EPixelFormatCapabilities::TextureBlendable
				: EPixelFormatCapabilities::UAV | EPixelFormatCapabilities::TypedUAVLoad | EPixelFormatCapabilities::TypedUAVStore);
		
		const bool bSupported = GSupportsRenderTargetFormat_PF_FloatRGBA
			&& GPixelFormats[PF_FloatRGBA].Supported
			&& UE::PixelFormat::HasCapabilities(PF_FloatRGBA, RequiredCapabilities);
		
		static bool bWarnedUnsupportedScratch = false;
		if (!bSupported && !bWarnedUnsupportedScratch)
		{
			UE_LOGF(LogVGSSplat, Warning, "Gaussian Splat RGBA16F scratch capabilities are unavailable. Inverse tone curve and expected depth are unavailable on unsupported rasterization paths.");
			bWarnedUnsupportedScratch = true;
		}

		return bSupported;
	}

	EVGSSplatDepthMode GetDepthMode(EShaderPlatform ShaderPlatform)
	{
		const EVGSSplatDepthMode Mode = GetDepthMode();
		if (Mode == EVGSSplatDepthMode::Stochastic
			&& (!PlatformSupportsStochasticDepth(ShaderPlatform) || IsGLES31EmulationEnabled()))
		{
			// Degrade to a plain depth write rather than silently rendering without depth.
			return EVGSSplatDepthMode::Hardware;
		}

		if (Mode == EVGSSplatDepthMode::Expected
			&& !SupportsSplatScratch(VGSSplat::IsHardwareRasterization(ShaderPlatform)))
		{
			// Expected depth uses RGBA16F for both color transmittance and depth moments.
			return EVGSSplatDepthMode::Hardware;
		}

		return Mode;
	}

	// Sorting splats is required if the platform does not support SRVs in Vertex Shading
	bool IsGatherShadedSplatsEnabled(EShaderPlatform ShaderPlatform)
	{
		if (IsGLES31EmulationEnabled())
		{
			return true;
		}

		const int32 Mode = CVarVGSSplatGatherShadedSplats.GetValueOnRenderThread();
		if (Mode >= 0)
		{
			return Mode != 0;
		}

		// TODO: Confirm that tiled GPUs perform better with the gather stage
		return !FDataDrivenShaderPlatformInfo::GetSupportsVertexShaderSRVs(ShaderPlatform)
			|| FDataDrivenShaderPlatformInfo::GetTargetsTiledGPU(ShaderPlatform);
	}

	// Vertex attribute fetch is required if the platform does not support SRVs in Vertex Shading
	bool IsVertexAttributeFetchEnabled(EShaderPlatform ShaderPlatform)
	{
#if !VGSSPLAT_ENGINE_5_9
		return false; // VGS CHANGE: no VET_UInt2 before 5.9 (above)
#else
		if (IsGLES31EmulationEnabled())
		{
			return true;
		}

		if (!FDataDrivenShaderPlatformInfo::GetSupportsVertexShaderSRVs(ShaderPlatform))
		{
			return true;
		}

		const int32 Mode = CVarVGSSplatVertexAttributeFetch.GetValueOnRenderThread();
		if (Mode >= 0)
		{
			return Mode != 0;
		}

		return false;
#endif
	}
}

static FScreenPassTextureViewport GetSplatViewport(const FScreenPassRenderTarget& Output, int32 DownsampleFactor)
{
	if (DownsampleFactor > 1)
	{
		const FIntRect ViewRect(FIntPoint::ZeroValue, FIntPoint::DivideAndRoundUp(Output.ViewRect.Size(), DownsampleFactor));
		return FScreenPassTextureViewport(ViewRect.Size(), ViewRect);
	}
	return FScreenPassTextureViewport(Output);
}

static TUniformBufferRef<FViewUniformShaderParameters> CreateDownsampledViewUniformBuffer(const FSceneView& InView, const FScreenPassTextureViewport& Viewport)
{
	const FViewInfo& View = static_cast<const FViewInfo&>(InView);

	FViewUniformShaderParameters DownsampledViewParameters = *View.CachedViewUniformShaderParameters;
	View.SetupViewRectUniformBufferParameters(DownsampledViewParameters, Viewport.Extent, Viewport.Rect, View.ViewMatrices, View.PrevViewInfo.ViewMatrices);
	return TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(DownsampledViewParameters, UniformBuffer_SingleFrame);
}

bool VGSSplat::IsHardwareRasterization()
{
	return CVarVGSSplatHardwareRasterization.GetValueOnRenderThread() != 0
		|| IsGLES31EmulationEnabled();
}

bool VGSSplat::IsHardwareRasterization(EShaderPlatform ShaderPlatform)
{
	return IsHardwareRasterization() || !PlatformSupportsSoftwareRasterization(ShaderPlatform);
}

bool VGSSplat::IsGPUDrivenPipeline()
{
#if !VGSSPLAT_ENGINE_5_9
	return false; // VGS CHANGE: no SortGPUBuffersIndirect before 5.9 (above)
#else
	return CVarVGSSplatHardwareRasterization.GetValueOnRenderThread() != 0 &&
		CVarVGSSplatGPUDrivenPipeline.GetValueOnRenderThread() != 0;
#endif
}

struct FShadedVGSSplatData
{
	FVector4f Position;
	FVector4f Axes;
	FVector4f Color;
};

struct FPackedShadedVGSSplatData
{
	uint32 Data[6];
};
// Mirrors FPackedShadedVGSSplatData in VGSSplatCommon.ush
static_assert(sizeof(FPackedShadedVGSSplatData) == 24, "Packed shaded splat stride must match the shader layout");

constexpr uint32 VGSSplatPackedRecordUInt32Count = sizeof(FPackedShadedVGSSplatData) / sizeof(uint32);

// Attribute layout must match FPackedShadedVGSSplatData
class FVGSSplatInstanceVertexDeclaration : public FRenderResource
{
public:
	FVertexDeclarationRHIRef VertexDeclarationRHI;

	virtual ~FVGSSplatInstanceVertexDeclaration() = default;

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
#if VGSSPLAT_ENGINE_5_9 // VGS CHANGE
		// TODO: Consider VET_Half2 for implicit floating-point conversion if platform supports it
		constexpr uint16 Stride = sizeof(FPackedShadedVGSSplatData);
		FVertexDeclarationElementList Elements;
		Elements.Add(FVertexElement(0,  0, VET_UInt2, 0, Stride, /*bUseInstanceIndex*/ true)); // ViewZ, NDC
		Elements.Add(FVertexElement(0,  8, VET_UInt2, 1, Stride, /*bUseInstanceIndex*/ true)); // Axes
		Elements.Add(FVertexElement(0, 16, VET_UInt2, 2, Stride, /*bUseInstanceIndex*/ true)); // Color
		VertexDeclarationRHI = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
#endif // else never used (IsVertexAttributeFetchEnabled)
	}

	virtual void ReleaseRHI() override
	{
		VertexDeclarationRHI.SafeRelease();
	}
};

static TGlobalResource<FVGSSplatInstanceVertexDeclaration> GVGSSplatInstanceVertexDeclaration;

// Mirrors FVGSSplatCluster in VGSSplatCommon.ush
static_assert(sizeof(FVGSSplatCluster) == 48, "Cluster stride must match the shader layout");

class FVGSSplatCullClustersCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVGSSplatCullClustersCS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatCullClustersCS, FGlobalShader);

	// Permutations
	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene data
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		// Transform data
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)

		// Cluster data
		SHADER_PARAMETER(uint32, NumClusters)
		SHADER_PARAMETER(float, PointScaleFactor)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVGSSplatCluster>, ClusterBuffer)

		// LOD data
		SHADER_PARAMETER(int32, LOD)
		SHADER_PARAMETER(float, ErrorThreshold)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWClusterOutputBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWClusterCount)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize1D() { return 64; }

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());

		// Unconditional, unlike the shading pass this has no non-compacting path to opt out of
		if (PlatformSupportsWaveOps(Parameters.Platform))
		{
			OutEnvironment.SetDefine(TEXT("GAUSSIAN_SPLAT_USE_WAVE_OPS"), 1);
			OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FVGSSplatCullClustersCS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatCullClusters.usf", "MainCS", SF_Compute);

class FVGSSplatShadeIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVGSSplatShadeIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatShadeIndirectArgsCS, FGlobalShader);

	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ClusterCountBuffer)
		SHADER_PARAMETER(FUintVector2, DimensionLimit)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWShadeIndirectArgs)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FVGSSplatShadeIndirectArgsCS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatShadeIndirectArgs.usf", "MainCS", SF_Compute);


class FShadeVGSSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FShadeVGSSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FShadeVGSSplatCS, FGlobalShader);

	// Permutations
	class FShaderDebug : SHADER_PERMUTATION_BOOL("SHADER_DEBUG");
	class FSHDegree : SHADER_PERMUTATION_INT ("SH_DEGREE", 4);
	using FPermutationDomain = TShaderPermutationDomain<FShaderDebug, FSHDegree>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene data
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		// Need private dependency on the Renderer module in your Build.cs:
		// PrivateDependencyModuleNames.Add("Renderer");
		// SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		// Debug pipeline configurations require FShaderDebug=1
		SHADER_PARAMETER(uint32, DebugGpuDrivenPipeline)
		SHADER_PARAMETER(uint32, DebugClustering)
		SHADER_PARAMETER(uint32, DebugFrustumCull)
		SHADER_PARAMETER(uint32, DebugWriteSortKeys)

		// Transform data
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(FMatrix44f, WorldToLocal)

		// Buffer data
		SHADER_PARAMETER(uint32, NumSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, VGSSplatBuffer)

		// Cluster dispatch
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVGSSplatCluster>, ClusterBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ClusterIndexBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ClusterCountBuffer)
		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)

		// Compression
		SHADER_PARAMETER(uint32, BufferFormat)
		SHADER_PARAMETER(uint32, BufferStride)
		SHADER_PARAMETER(FVector3f, PositionScale)
		SHADER_PARAMETER(FVector3f, PositionBias)
		SHADER_PARAMETER(FVector3f, ScaleScale)
		SHADER_PARAMETER(FVector3f, ScaleBias)
		SHADER_PARAMETER(FVector3f, SHScale)
		SHADER_PARAMETER(FVector3f, SHBias)
		SHADER_PARAMETER(FVector4f, ColorScale)
		SHADER_PARAMETER(FVector4f, ColorBias)

		// Spherical harmonics
		SHADER_PARAMETER(uint32, SHCoordinateSpace)
		SHADER_PARAMETER(uint32, BlendInSRGB) // VGS CHANGE

		// VGS CHANGE: a lit proxy's normals (VGSSplatShading.usf, VGSShadedNormal)
		SHADER_PARAMETER(uint32, Lit)
		SHADER_PARAMETER(uint32, HasNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NormalBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWShadedNormalBuffer)

		// Per-component overrides
		SHADER_PARAMETER(float, PointScaleFactor)
		SHADER_PARAMETER(float, OpacityScale)

		SHADER_PARAMETER(float, OpacityThreshold)
		SHADER_PARAMETER(float, CovarianceScale)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FPackedShadedVGSSplatData>, RWShadedVGSSplatBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWSplatCount)

		// Optional sort-key data
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWSortKeys)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWSortValues)
		SHADER_PARAMETER(uint32, SortKeyFrontToBack)
		SHADER_PARAMETER(uint32, SortKeyBits)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize1D() { return 64; }

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());

		if (PlatformSupportsWaveOps(Parameters.Platform))
		{
			OutEnvironment.SetDefine(TEXT("GAUSSIAN_SPLAT_USE_WAVE_OPS"), 1);
			OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FShadeVGSSplatCS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatShading.usf", "MainCS", SF_Compute);

// Applies the inverse tone curve to the splat scratch, then composites onto SceneColor
// via ROP premultiplied alpha-over with transmittance carried in .a to match UE
// SceneColor convention.
class FVGSSplatInverseToneCurvePostBlendPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVGSSplatInverseToneCurvePostBlendPS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatInverseToneCurvePostBlendPS, FGlobalShader);

	class FApplyInverseToneCurve : SHADER_PERMUTATION_BOOL("APPLY_INVERSE_TONE_CURVE");
	using FPermutationDomain = TShaderPermutationDomain<FApplyInverseToneCurve>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT(FScreenPassTextureInput, SplatColor)
		SHADER_PARAMETER_TEXTURE(Texture3D, InverseToneCurveLUT)
		SHADER_PARAMETER_SAMPLER(SamplerState, InverseToneCurveLUTSampler)
		SHADER_PARAMETER(FScreenTransform, SvPositionToInputTextureUV)
		SHADER_PARAMETER(float, Strength)
		SHADER_PARAMETER(float, InverseToneCurveLUTEdge)
		SHADER_PARAMETER(float, Intensity) // VGS CHANGE
		SHADER_PARAMETER(uint32, BlendInSRGB) // VGS CHANGE
		SHADER_PARAMETER(FVector3f, Grade) // VGS CHANGE
		SHADER_PARAMETER(FVector3f, Tint) // VGS CHANGE
		SHADER_PARAMETER(uint32, GradeActive) // VGS CHANGE
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FVGSSplatInverseToneCurvePostBlendPS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatInverseToneCurvePostBlend.usf", "MainPS", SF_Pixel);

struct FTileCountDebugData
{
	FIntPoint Tile0;
	FIntPoint Tile1;
};

class FTileCountVGSSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTileCountVGSSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FTileCountVGSSplatCS, FGlobalShader);

	// Permutations
	class FShaderDebug : SHADER_PERMUTATION_BOOL("SHADER_DEBUG");
	using FPermutationDomain = TShaderPermutationDomain<FShaderDebug>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		SHADER_PARAMETER(uint32, NumSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FShadedVGSSplatData>, ShadedVGSSplatBuffer)

		SHADER_PARAMETER(FIntPoint, TileSize)
		SHADER_PARAMETER(FIntPoint, TileDimension)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWTileCountBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWMinDepthPerTileBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWMaxDepthPerTileBuffer)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FTileCountDebugData>, RWTileDebugBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize1D() { return 64; }

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsSoftwareRasterization(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
	}
};

IMPLEMENT_GLOBAL_SHADER(FTileCountVGSSplatCS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatTileCount.usf", "MainCS", SF_Compute);

class FPrefixSumVGSSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FPrefixSumVGSSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FPrefixSumVGSSplatCS, FGlobalShader);

	// Permutations
	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, NumItems)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ItemBuffer)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWPrefixSumBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize1D() { return 1024; }

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsSoftwareRasterization(Parameters.Platform)
			&& PlatformSupportsThreadGroupSize(Parameters.Platform, GetThreadGroupSize1D());
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("NUM_THREADS_PER_GROUP"), GetThreadGroupSize1D());
	}
};

IMPLEMENT_GLOBAL_SHADER(FPrefixSumVGSSplatCS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatPrefixSum.usf", "MainCS", SF_Compute);

class FFillSplatIndexListVGSSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFillSplatIndexListVGSSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FFillSplatIndexListVGSSplatCS, FGlobalShader);

	// Permutations
	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		// Splat data
		SHADER_PARAMETER(uint32, NumSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FShadedVGSSplatData>, ShadedVGSSplatBuffer)

		SHADER_PARAMETER(FIntPoint, TileSize)
		SHADER_PARAMETER(FIntPoint, TileDimension)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatOffsetPerTileBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MinDepthPerTileBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MaxDepthPerTileBuffer)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWNumSplatsPerTileBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWSortKeyBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWSplatIndexBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize1D() { return 64; }

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsSoftwareRasterization(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
	}
};

IMPLEMENT_GLOBAL_SHADER(FFillSplatIndexListVGSSplatCS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatFillSplatIndexBuffer.usf", "MainCS", SF_Compute);

class FVGSSplatDrawIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVGSSplatDrawIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatDrawIndirectArgsCS, FGlobalShader);

	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatCountBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWDrawIndirectArgs)
		SHADER_PARAMETER_STRUCT_INCLUDE(FVGSSplatStatsParameters, Stats)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FVGSSplatDrawIndirectArgsCS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatDrawIndirectArgs.usf", "MainCS", SF_Compute);

BEGIN_SHADER_PARAMETER_STRUCT(FGPUSortPassParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RemoteKeySRVs0)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RemoteKeySRVs1)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RemoteKeyUAVs0)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RemoteKeyUAVs1)

	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RemoteValueSRVs0)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RemoteValueSRVs1)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RemoteValueUAVs0)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RemoteValueUAVs1)

	// Bound only on the indirect path (SortGPUBuffersIndirect); left null for the direct path.
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatCountSRV)
END_SHADER_PARAMETER_STRUCT()

class FRasterizeVGSSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRasterizeVGSSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FRasterizeVGSSplatCS, FGlobalShader);

	// Permutations
	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene data
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		// Need private dependency on the Renderer module in your Build.cs:
		// PrivateDependencyModuleNames.Add("Renderer");
		// SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		// Splat data
		SHADER_PARAMETER(uint32, NumSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FShadedVGSSplatData>, ShadedVGSSplatBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint32>, SortedIndexBuffer)
		SHADER_PARAMETER(FVector3f, FalloffParameters)

		// Output
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWColorTexture)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize2D() { return 8; }

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsSoftwareRasterization(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}
};

IMPLEMENT_GLOBAL_SHADER(FRasterizeVGSSplatCS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatRasterization.usf", "MainCS", SF_Compute);

class FTileRasterizeVGSSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTileRasterizeVGSSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FTileRasterizeVGSSplatCS, FGlobalShader);

	// Permutations
	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene data
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		// Need private dependency on the Renderer module in your Build.cs:
		// PrivateDependencyModuleNames.Add("Renderer");
		// SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		// Splat data
		SHADER_PARAMETER(uint32, NumSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FShadedVGSSplatData>, ShadedVGSSplatBuffer)

		SHADER_PARAMETER(FIntPoint, TileSize)
		SHADER_PARAMETER(FIntPoint, TileDimension)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, NumSplatsPerTileBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatOffsetPerTileBuffer)

		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatIndexBuffer)
		SHADER_PARAMETER(FVector3f, FalloffParameters)

		// Output
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWColorTexture)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize2D() { return 8; }

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsSoftwareRasterization(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}
};

IMPLEMENT_GLOBAL_SHADER(FTileRasterizeVGSSplatCS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatTileRasterize.usf", "MainCS", SF_Compute);

class FGatherShadedVGSSplatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGatherShadedVGSSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FGatherShadedVGSSplatCS, FGlobalShader);

	class FGpuDrivenPipeline : SHADER_PERMUTATION_BOOL("GAUSSIAN_SPLAT_GPU_DRIVEN_PIPELINE");
	class FVertexAttributeFetch : SHADER_PERMUTATION_BOOL("GAUSSIAN_SPLAT_VERTEX_ATTRIBUTE_FETCH");
	using FPermutationDomain = TShaderPermutationDomain<FGpuDrivenPipeline, FVertexAttributeFetch>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		SHADER_PARAMETER(uint32, NumSplats)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FShadedVGSSplatData>, ShadedVGSSplatBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SortedIndexBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SplatCountBuffer)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FPackedShadedVGSSplatData>, RWGatheredShadedVGSSplatBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWGatheredShadedVGSSplatVertexBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static int32 GetThreadGroupSize1D() { return 64; }

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
	}
};

IMPLEMENT_GLOBAL_SHADER(FGatherShadedVGSSplatCS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatGatherShadedSplats.usf", "MainCS", SF_Compute);

class FHardwareRasterizeVGSSplatVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHardwareRasterizeVGSSplatVS);
	SHADER_USE_PARAMETER_STRUCT(FHardwareRasterizeVGSSplatVS, FGlobalShader);

	class FGatherShadedSplats : SHADER_PERMUTATION_BOOL("GAUSSIAN_SPLAT_GATHER_SHADED_SPLATS");
	class FVertexAttributeFetch : SHADER_PERMUTATION_BOOL("GAUSSIAN_SPLAT_VERTEX_ATTRIBUTE_FETCH");
	class FLit : SHADER_PERMUTATION_BOOL("VGS_LIT"); // VGS CHANGE
	using FPermutationDomain = TShaderPermutationDomain<FGatherShadedSplats, FVertexAttributeFetch, FLit>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FShadedVGSSplatData>, ShadedVGSSplatBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SortedIndexBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ShadedNormalBuffer) // VGS CHANGE
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const FPermutationDomain PermutationVector(Parameters.PermutationId);

		// VGS CHANGE: a lit proxy reads its normals by record, so its records stay in place.
		if (PermutationVector.Get<FLit>() && (PermutationVector.Get<FGatherShadedSplats>() || PermutationVector.Get<FVertexAttributeFetch>()))
		{
			return false;
		}

		if (PermutationVector.Get<FVertexAttributeFetch>() && !PermutationVector.Get<FGatherShadedSplats>())
		{
			return false;
		}

		if (!FDataDrivenShaderPlatformInfo::GetSupportsVertexShaderSRVs(Parameters.Platform)
			&& !PermutationVector.Get<FVertexAttributeFetch>())
		{
			return false;
		}

		return true;
	}
};

class FHardwareRasterizeVGSSplatPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHardwareRasterizeVGSSplatPS);
	SHADER_USE_PARAMETER_STRUCT(FHardwareRasterizeVGSSplatPS, FGlobalShader);

	class FWriteStochasticDepth : SHADER_PERMUTATION_BOOL("WRITE_STOCHASTIC_DEPTH");
	class FWriteVelocity : SHADER_PERMUTATION_BOOL("WRITE_VELOCITY");
	class FUseBlueNoise : SHADER_PERMUTATION_BOOL("USE_BLUE_NOISE");
	class FWriteExpectedDepth : SHADER_PERMUTATION_BOOL("WRITE_EXPECTED_DEPTH");
	class FSuperSample : SHADER_PERMUTATION_BOOL("GAUSSIAN_SPLAT_SUPERSAMPLE");
	class FLit : SHADER_PERMUTATION_BOOL("VGS_LIT"); // VGS CHANGE
	using FPermutationDomain = TShaderPermutationDomain<FWriteStochasticDepth, FWriteVelocity, FUseBlueNoise, FWriteExpectedDepth, FSuperSample, FLit>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_REF(FBlueNoise, BlueNoise)
		SHADER_PARAMETER(float, OpacityThreshold)
		SHADER_PARAMETER(float, ExpectedDepthScale)
		SHADER_PARAMETER(FVector3f, FalloffParameters)
		SHADER_PARAMETER(int32, SuperSampleGrid)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		// Stochastic depth needs per-render-target blend state
		if (PermutationVector.Get<FWriteStochasticDepth>() && !PlatformSupportsStochasticDepth(Parameters.Platform))
		{
			return false;
		}
		// Blue Noise is only conditionally used when stochastic depth is also active
		if (!PermutationVector.Get<FWriteStochasticDepth>() && PermutationVector.Get<FUseBlueNoise>())
		{
			return false;
		}
		if (PermutationVector.Get<FWriteStochasticDepth>() && PermutationVector.Get<FWriteVelocity>())
		{
			return false;
		}
		if (PermutationVector.Get<FWriteExpectedDepth>() &&
			(PermutationVector.Get<FWriteStochasticDepth>() || PermutationVector.Get<FWriteVelocity>() || PermutationVector.Get<FUseBlueNoise>()))
		{
			return false;
		}
		// VGS CHANGE: a lit proxy accumulates its normals beside the expected depth, at full resolution.
		if (PermutationVector.Get<FLit>() && (!PermutationVector.Get<FWriteExpectedDepth>() || PermutationVector.Get<FSuperSample>()))
		{
			return false;
		}
		return true;
	}
};

BEGIN_SHADER_PARAMETER_STRUCT(FHardwareRasterizeVGSSplatPassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FHardwareRasterizeVGSSplatVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FHardwareRasterizeVGSSplatPS::FParameters, PS)
	RDG_BUFFER_ACCESS(DrawIndirectArgs, ERHIAccess::IndirectArgs)
	RDG_BUFFER_ACCESS(GatheredSplatVertexBuffer, ERHIAccess::VertexOrIndexBuffer)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

IMPLEMENT_GLOBAL_SHADER(FHardwareRasterizeVGSSplatVS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatHardwareRasterize.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FHardwareRasterizeVGSSplatPS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatHardwareRasterize.usf", "MainPS", SF_Pixel);

// Resolves the stochastic splat-depth target into scene depth (writes SV_Depth).
class FVGSSplatResolveDepthPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVGSSplatResolveDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatResolveDepthPS, FGlobalShader);

	class FWriteVelocity : SHADER_PERMUTATION_BOOL("WRITE_VELOCITY");
	using FPermutationDomain = TShaderPermutationDomain<FWriteVelocity>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DepthTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsStochasticDepth(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVGSSplatResolveDepthPS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatHardwareRasterize.usf", "MainPS_ResolveDepth", SF_Pixel);

class FVGSSplatExpectedDepthPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVGSSplatExpectedDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatExpectedDepthPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DepthMomentsTexture)
		SHADER_PARAMETER(float, ExpectedDepthScale)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FVGSSplatExpectedDepthPS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatHardwareRasterize.usf", "MainPS_ExpectedDepth", SF_Pixel);

// VGS CHANGE: writes a lit proxy into the GBuffer (VGSSplatGBuffer.usf).
static TAutoConsoleVariable<int32> CVarVGSSplatLitDither(
	TEXT("r.VGSSplat.Lit.Dither"),
	0,
	TEXT("Lit captures: where splats only partly cover a pixel, write it where they cover half or more (0, default); or at random as much as they cover (1),") LINE_TERMINATOR
	TEXT("for temporal anti-aliasing to smooth. TSR does not smooth it away (2026-11-11, UE 5.9): edges and faint splats stay speckled."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarVGSSplatLitMinCoverage(
	TEXT("r.VGSSplat.Lit.MinCoverage"),
	0.02f,
	TEXT("Lit captures: pixels the splats cover less than this are never written."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarVGSSplatLitNormalSmoothing(
	TEXT("r.VGSSplat.Lit.NormalSmoothing"),
	-1.0f,
	TEXT("Lit captures: overrides every capture's Normal Smoothing, in cm (0 off); below 0 (default) each capture's own."),
	ECVF_RenderThreadSafe);

class FVGSSplatGBufferPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVGSSplatGBufferPS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatGBufferPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SplatColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DepthMomentsTexture)
		SHADER_PARAMETER(float, ExpectedDepthScale)
		SHADER_PARAMETER(uint32, BlendInSRGB)
		SHADER_PARAMETER(float, SelfIllumination)
		SHADER_PARAMETER(float, Intensity)
		SHADER_PARAMETER(float, Roughness)
		SHADER_PARAMETER(float, Specular)
		SHADER_PARAMETER(float, MinCoverage)
		SHADER_PARAMETER(uint32, Dither)
		SHADER_PARAMETER(float, NormalSmoothing)
		SHADER_PARAMETER(FVector3f, Grade)
		SHADER_PARAMETER(FVector3f, Tint)
		SHADER_PARAMETER(uint32, GradeActive)
		// Substrate's material buffer, when it has its own (not the blendable GBuffer).
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<uint>, MaterialTextureArrayUAV)
		SHADER_PARAMETER(uint32, MaxBytesPerPixel)
		SHADER_PARAMETER(uint32, bRoughDiffuse)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SUBSTRATE_INLINE_SHADING"), 1); // as the engine's debug probes
	}
};

IMPLEMENT_GLOBAL_SHADER(FVGSSplatGBufferPS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatGBuffer.usf", "MainPS", SF_Pixel);

class FVGSSplatResolveVelocityPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVGSSplatResolveVelocityPS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatResolveVelocityPS, FGlobalShader);

	class FUseBlueNoise : SHADER_PERMUTATION_BOOL("USE_BLUE_NOISE");
	using FPermutationDomain = TShaderPermutationDomain<FUseBlueNoise>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_REF(FBlueNoise, BlueNoise)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, DepthMomentsTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FVGSSplatResolveVelocityPS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatHardwareRasterize.usf", "MainPS_ResolveVelocity", SF_Pixel);

class FVGSSplatDownsampleDepthPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVGSSplatDownsampleDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatDownsampleDepthPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
		SHADER_PARAMETER(FScreenTransform, SvPositionToSourcePixel)
		SHADER_PARAMETER(FIntPoint, SourcePixelMax)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FVGSSplatDownsampleDepthPS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatDownsample.usf", "MainPS_DownsampleDepth", SF_Pixel);

class FVGSSplatUpsampleDepthPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVGSSplatUpsampleDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatUpsampleDepthPS, FGlobalShader);

	class FWriteVelocity : SHADER_PERMUTATION_BOOL("WRITE_VELOCITY");
	using FPermutationDomain = TShaderPermutationDomain<FWriteVelocity>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SplatDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
		SHADER_PARAMETER(FScreenTransform, SvPositionToDownsampledPixel)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FVGSSplatUpsampleDepthPS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatDownsample.usf", "MainPS_UpsampleDepth", SF_Pixel);

class FVGSSplatHardwareRasterizeStochasticDepthPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVGSSplatHardwareRasterizeStochasticDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatHardwareRasterizeStochasticDepthPS, FGlobalShader);

	class FUseBlueNoise : SHADER_PERMUTATION_BOOL("USE_BLUE_NOISE");
	class FWriteVelocity : SHADER_PERMUTATION_BOOL("WRITE_VELOCITY");
	using FPermutationDomain = TShaderPermutationDomain<FUseBlueNoise, FWriteVelocity>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_REF(FBlueNoise, BlueNoise)
		SHADER_PARAMETER(FVector3f, FalloffParameters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return PlatformSupportsStochasticDepth(Parameters.Platform);
	}
};

BEGIN_SHADER_PARAMETER_STRUCT(FHardwareRasterizeStochasticDepthPassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FHardwareRasterizeVGSSplatVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FVGSSplatHardwareRasterizeStochasticDepthPS::FParameters, PS)
	RDG_BUFFER_ACCESS(DrawIndirectArgs, ERHIAccess::IndirectArgs)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

IMPLEMENT_GLOBAL_SHADER(FVGSSplatHardwareRasterizeStochasticDepthPS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatHardwareRasterize.usf", "MainPS_StochasticDepth", SF_Pixel);

FIntPoint CalcTileDimension(const FIntRect& ViewRect)
{
	FIntPoint TileDimension = FIntPoint(
		FMath::DivideAndRoundUp(ViewRect.Width(), GetTileSize()),
		FMath::DivideAndRoundUp(ViewRect.Height(), GetTileSize())
	);
	return TileDimension;
}

uint32 CalcTiles(const FIntRect& ViewRect)
{
	const int32 TilesX = FMath::DivideAndRoundUp(ViewRect.Width(), GetTileSize());
	const int32 TilesY = FMath::DivideAndRoundUp(ViewRect.Height(), GetTileSize());
	const uint32 NumTiles = TilesX * TilesY;
	return NumTiles;
}

void RasterizeHardware(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer,
	int32 DownsampleFactor,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneVelocityTexture,
	uint32 NumSplats,
	FRDGBufferRef ShadedVGSSplatBuffer,
	const FScreenPassRenderTarget& Output,
	FRDGBufferRef SplatCountBuffer,
	FRDGBufferRef SortKeyBuffer,
	FRDGBufferRef SplatIndexBuffer,
	uint32 NumClusters,
	FRDGBufferRef ClusterCountBuffer,
	VGSSplat::FRenderStats& RenderStats,
	ERenderTargetLoadAction ColorLoadAction = ERenderTargetLoadAction::ELoad,
	// VGS CHANGE: for a lit proxy, its normals by record; the depth moments come back in
	// OutDepthMoments for VGSSplatGBuffer.usf instead of being resolved into scene depth.
	FRDGBufferRef ShadedNormalBuffer = nullptr,
	FRDGTextureRef* OutDepthMoments = nullptr
)
{
	const bool bGpuDrivenPipeline = (SplatCountBuffer != nullptr);
	const bool bLit = ShadedNormalBuffer != nullptr; // VGS CHANGE

	const EVGSSplatDepthMode DepthMode = GetDepthMode(View.GetShaderPlatform());
	const uint32 SortElementCount = NumSplats;
	FRDGBufferRef DrawIndirectArgs = nullptr;

	if (bGpuDrivenPipeline)
	{
		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetShaderPlatform());

		DrawIndirectArgs = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateIndirectDesc<FRHIDrawIndirectParameters>(1),
			TEXT("VGSSplat.DrawIndirectArgs")
		);

		FVGSSplatDrawIndirectArgsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FVGSSplatDrawIndirectArgsCS::FParameters>();
		Parameters->SplatCountBuffer = GraphBuilder.CreateSRV(SplatCountBuffer, PF_R32_UINT);
		Parameters->RWDrawIndirectArgs = GraphBuilder.CreateUAV(DrawIndirectArgs, PF_R32_UINT);
		RenderStats.SetShaderParameters(GraphBuilder, NumClusters, NumSplats, ClusterCountBuffer, Parameters->Stats);

		TShaderMapRef<FVGSSplatDrawIndirectArgsCS> SetupCS(ShaderMap);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("VGSSplat.DrawIndirectArgs"),
			SetupCS,
			Parameters,
			FIntVector(1, 1, 1));
	}

	// Back-to-front sort.
	const FRDGBufferDesc ScratchDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), SortElementCount);
	// GPU-driven pipeline does not require secondary sort buffers to be initialized, so mark as external to avoid RDG errors
	const bool bExternalScratch = bGpuDrivenPipeline;

	FRDGBufferRef GPUSortKeysBuffer[] = {
		SortKeyBuffer,
		bExternalScratch
			? GraphBuilder.RegisterExternalBuffer(AllocatePooledBuffer(ScratchDesc, TEXT("VGSSplat.SortKeys1")))
			: GraphBuilder.CreateBuffer(ScratchDesc, TEXT("VGSSplat.SortKeys1")),
	};
	FRDGBufferRef GPUSortValuesBuffer[] = {
		SplatIndexBuffer,
		bExternalScratch
			? GraphBuilder.RegisterExternalBuffer(AllocatePooledBuffer(ScratchDesc, TEXT("VGSSplat.SortValues1")))
			: GraphBuilder.CreateBuffer(ScratchDesc, TEXT("VGSSplat.SortValues1")),
	};

	if (!bExternalScratch)
	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GPUSortKeysBuffer[1], PF_R32_UINT), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GPUSortValuesBuffer[1], PF_R32_UINT), 0);
	}

	const uint32 KeyMask = GetSortKeyMask(View.GetShaderPlatform());
	const int32 SortedBufferIndex = GetGPUSortPassCount(KeyMask) % 2;

	{
		FGPUSortPassParameters* PassParameters = GraphBuilder.AllocParameters<FGPUSortPassParameters>();
		PassParameters->RemoteKeySRVs0 = GraphBuilder.CreateSRV(GPUSortKeysBuffer[0], PF_R32_UINT);
		PassParameters->RemoteKeySRVs1 = GraphBuilder.CreateSRV(GPUSortKeysBuffer[1], PF_R32_UINT);
		PassParameters->RemoteKeyUAVs0 = GraphBuilder.CreateUAV(GPUSortKeysBuffer[0], PF_R32_UINT);
		PassParameters->RemoteKeyUAVs1 = GraphBuilder.CreateUAV(GPUSortKeysBuffer[1], PF_R32_UINT);
		PassParameters->RemoteValueSRVs0 = GraphBuilder.CreateSRV(GPUSortValuesBuffer[0], PF_R32_UINT);
		PassParameters->RemoteValueSRVs1 = GraphBuilder.CreateSRV(GPUSortValuesBuffer[1], PF_R32_UINT);
		PassParameters->RemoteValueUAVs0 = GraphBuilder.CreateUAV(GPUSortValuesBuffer[0], PF_R32_UINT);
		PassParameters->RemoteValueUAVs1 = GraphBuilder.CreateUAV(GPUSortValuesBuffer[1], PF_R32_UINT);
		PassParameters->SplatCountSRV = SplatCountBuffer ? GraphBuilder.CreateSRV(SplatCountBuffer, PF_R32_UINT) : nullptr;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VGSSplat.SortGPU%s", SplatCountBuffer ? TEXT(".Indirect") : TEXT("")),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, bGpuDrivenPipeline, SortElementCount, KeyMask, SortedBufferIndex, FeatureLevel = View.GetFeatureLevel()]
			(FRDGAsyncTask, FRHICommandList& RHICmdList)
			{
				FGPUSortBuffers SortBuffers;
				SortBuffers.RemoteKeySRVs[0] = PassParameters->RemoteKeySRVs0->GetRHI();
				SortBuffers.RemoteKeySRVs[1] = PassParameters->RemoteKeySRVs1->GetRHI();
				SortBuffers.RemoteKeyUAVs[0] = PassParameters->RemoteKeyUAVs0->GetRHI();
				SortBuffers.RemoteKeyUAVs[1] = PassParameters->RemoteKeyUAVs1->GetRHI();
				SortBuffers.RemoteValueSRVs[0] = PassParameters->RemoteValueSRVs0->GetRHI();
				SortBuffers.RemoteValueSRVs[1] = PassParameters->RemoteValueSRVs1->GetRHI();
				SortBuffers.RemoteValueUAVs[0] = PassParameters->RemoteValueUAVs0->GetRHI();
				SortBuffers.RemoteValueUAVs[1] = PassParameters->RemoteValueUAVs1->GetRHI();

				const FRHITransitionInfo SortBufferToUAV[] = {
					FRHITransitionInfo(SortBuffers.RemoteKeyUAVs[0],   ERHIAccess::Unknown, ERHIAccess::UAVCompute),
					FRHITransitionInfo(SortBuffers.RemoteKeyUAVs[1],   ERHIAccess::Unknown, ERHIAccess::UAVCompute),
					FRHITransitionInfo(SortBuffers.RemoteValueUAVs[0], ERHIAccess::Unknown, ERHIAccess::UAVCompute),
					FRHITransitionInfo(SortBuffers.RemoteValueUAVs[1], ERHIAccess::Unknown, ERHIAccess::UAVCompute),
				};
				RHICmdList.Transition(MakeArrayView(SortBufferToUAV));

#if VGSSPLAT_ENGINE_5_9 // VGS CHANGE
				const int32 ResultIndex = bGpuDrivenPipeline
					? SortGPUBuffersIndirect(RHICmdList, SortBuffers, 0, KeyMask, PassParameters->SplatCountSRV->GetRHI(), FeatureLevel)
					: SortGPUBuffers(RHICmdList, SortBuffers, 0, KeyMask, SortElementCount, FeatureLevel);
#else
				const int32 ResultIndex = SortGPUBuffers(RHICmdList, SortBuffers, 0, KeyMask, SortElementCount, FeatureLevel);
#endif
				check(ResultIndex == SortedBufferIndex);
				RHICmdList.Transition(MakeArrayView(SortBufferToUAV));
			});
	}

	const bool bVertexAttributeFetch = IsVertexAttributeFetchEnabled(View.GetShaderPlatform());
	// VGS CHANGE: a lit proxy's records stay in place, for its normals (VGSSplat::SupportsLit
	// keeps it off platforms that need vertex attribute fetch).
	const bool bGatherShadedSplats = !bLit && (bVertexAttributeFetch || IsGatherShadedSplatsEnabled(View.GetShaderPlatform()));
	if (bGatherShadedSplats)
	{
		FRDGBufferRef GatheredShadedVGSSplatBuffer = GraphBuilder.CreateBuffer(
			bVertexAttributeFetch
				? FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats * VGSSplatPackedRecordUInt32Count)
				: FRDGBufferDesc::CreateStructuredDesc(sizeof(FPackedShadedVGSSplatData), NumSplats),
			TEXT("VGSSplat.GatheredShadedBuffer")
		);

		FGatherShadedVGSSplatCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FGatherShadedVGSSplatCS::FGpuDrivenPipeline>(bGpuDrivenPipeline);
		PermutationVector.Set<FGatherShadedVGSSplatCS::FVertexAttributeFetch>(bVertexAttributeFetch);
		TShaderMapRef<FGatherShadedVGSSplatCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);

		FGatherShadedVGSSplatCS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FGatherShadedVGSSplatCS::FParameters>();
		PassParameters->View = ViewUniformBuffer;
		PassParameters->NumSplats = NumSplats;
		PassParameters->ShadedVGSSplatBuffer = GraphBuilder.CreateSRV(ShadedVGSSplatBuffer);
		PassParameters->SortedIndexBuffer = GraphBuilder.CreateSRV(GPUSortValuesBuffer[SortedBufferIndex], PF_R32_UINT);
		PassParameters->SplatCountBuffer = bGpuDrivenPipeline ? GraphBuilder.CreateSRV(SplatCountBuffer, PF_R32_UINT) : nullptr;
		if (bVertexAttributeFetch)
		{
			PassParameters->RWGatheredShadedVGSSplatVertexBuffer = GraphBuilder.CreateUAV(GatheredShadedVGSSplatBuffer, PF_R32_UINT);
		}
		else
		{
			PassParameters->RWGatheredShadedVGSSplatBuffer = GraphBuilder.CreateUAV(GatheredShadedVGSSplatBuffer);
		}

		const FIntVector GroupCount = FComputeShaderUtils::GetGroupCountWrapped(NumSplats, FGatherShadedVGSSplatCS::GetThreadGroupSize1D());
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("VGSSplat.GatherShadedSplats"),
			ERDGPassFlags::Compute,
			ComputeShader,
			PassParameters,
			GroupCount);
		ShadedVGSSplatBuffer = GatheredShadedVGSSplatBuffer;
	}

	const bool bVelocity = !bLit && IsVelocityEnabled() // VGS CHANGE: not lit
		&& DepthMode != EVGSSplatDepthMode::Off
		&& SceneVelocityTexture != nullptr
		&& EnumHasAnyFlags(SceneVelocityTexture->Desc.Flags, TexCreate_RenderTargetable);

	// VGS CHANGE: a lit proxy is drawn as expected depth is, whatever r.VGSSplat.Depth says.
	const bool bHardwareDepth = !bLit && (DepthMode == EVGSSplatDepthMode::Hardware);
	const bool bStochasticDepth = !bLit && (DepthMode == EVGSSplatDepthMode::Stochastic);
	const bool bExpectedDepth = bLit || (DepthMode == EVGSSplatDepthMode::Expected);

	FRDGTextureRef DepthTexture = nullptr;
	if (bStochasticDepth) // VGS CHANGE: was DepthMode == Stochastic
	{
		const FRDGTextureDesc SplatDepthDesc = FRDGTextureDesc::Create2D(
			Output.Texture->Desc.Extent,
			GetStochasticDepthFormat(),
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_RenderTargetable);
		DepthTexture = GraphBuilder.CreateTexture(SplatDepthDesc, TEXT("VGSSplat.StochasticDepth"));
	}

	FRDGTextureRef DepthMomentsTexture = nullptr;
	if (bExpectedDepth)
	{
		const FRDGTextureDesc MomentsDesc = FRDGTextureDesc::Create2D(
			Output.Texture->Desc.Extent,
			PF_FloatRGBA,
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_RenderTargetable);
		DepthMomentsTexture = GraphBuilder.CreateTexture(MomentsDesc, TEXT("VGSSplat.DepthMoments"));
	}

	// Rasterization
	{
		FHardwareRasterizeVGSSplatVS::FPermutationDomain VSPermutationVector;
		VSPermutationVector.Set<FHardwareRasterizeVGSSplatVS::FGatherShadedSplats>(bGatherShadedSplats);
		VSPermutationVector.Set<FHardwareRasterizeVGSSplatVS::FVertexAttributeFetch>(bVertexAttributeFetch && !bLit);
		VSPermutationVector.Set<FHardwareRasterizeVGSSplatVS::FLit>(bLit); // VGS CHANGE
		TShaderMapRef<FHardwareRasterizeVGSSplatVS> VertexShader(GetGlobalShaderMap(View.GetShaderPlatform()), VSPermutationVector);

		FHardwareRasterizeVGSSplatPS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FHardwareRasterizeVGSSplatPS::FWriteStochasticDepth>(bStochasticDepth);
		PermutationVector.Set<FHardwareRasterizeVGSSplatPS::FWriteVelocity>(bVelocity && bHardwareDepth);
		PermutationVector.Set<FHardwareRasterizeVGSSplatPS::FUseBlueNoise>(bStochasticDepth && IsBlueNoiseEnabled());
		PermutationVector.Set<FHardwareRasterizeVGSSplatPS::FWriteExpectedDepth>(bExpectedDepth);
		PermutationVector.Set<FHardwareRasterizeVGSSplatPS::FSuperSample>(DownsampleFactor > 1);
		PermutationVector.Set<FHardwareRasterizeVGSSplatPS::FLit>(bLit); // VGS CHANGE
		TShaderMapRef<FHardwareRasterizeVGSSplatPS> PixelShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);

		FHardwareRasterizeVGSSplatPassParameters* PassParameters = GraphBuilder.AllocParameters<FHardwareRasterizeVGSSplatPassParameters>();
		{
			PassParameters->VS.View = ViewUniformBuffer;
			PassParameters->VS.ShadedVGSSplatBuffer = bVertexAttributeFetch
				? nullptr
				: GraphBuilder.CreateSRV(ShadedVGSSplatBuffer);
			PassParameters->VS.SortedIndexBuffer = bGatherShadedSplats
				? nullptr
				: GraphBuilder.CreateSRV(GPUSortValuesBuffer[SortedBufferIndex], PF_R32_UINT);
			PassParameters->GatheredSplatVertexBuffer = bVertexAttributeFetch ? ShadedVGSSplatBuffer : nullptr;
			PassParameters->VS.ShadedNormalBuffer = bLit ? GraphBuilder.CreateSRV(ShadedNormalBuffer) : nullptr; // VGS CHANGE
			PassParameters->PS.View = ViewUniformBuffer;
			PassParameters->PS.BlueNoise = CreateUniformBufferImmediate(GetBlueNoiseGlobalParameters(), EUniformBufferUsage::UniformBuffer_SingleDraw);
			PassParameters->PS.OpacityThreshold = GetHardwareRasterizationOpacityThreshold();
			PassParameters->PS.ExpectedDepthScale = GetExpectedDepthScale();
			PassParameters->PS.FalloffParameters = GetFalloffParameters();
			PassParameters->PS.SuperSampleGrid = DownsampleFactor;
			PassParameters->RenderTargets[0] = FRenderTargetBinding(Output.Texture, ColorLoadAction);
			if (bStochasticDepth)
			{
				PassParameters->RenderTargets[1] = FRenderTargetBinding(DepthTexture, ERenderTargetLoadAction::EClear);
			}
			if (bVelocity && bHardwareDepth)
			{
				PassParameters->RenderTargets[1] = FRenderTargetBinding(SceneVelocityTexture, ERenderTargetLoadAction::ELoad);
			}
			if (bExpectedDepth)
			{
				PassParameters->RenderTargets[1] = FRenderTargetBinding(DepthMomentsTexture, ERenderTargetLoadAction::EClear);
			}
			const FExclusiveDepthStencil::Type DepthStencilAccess = bHardwareDepth
				? FExclusiveDepthStencil::DepthWrite_StencilNop
				: FExclusiveDepthStencil::DepthRead_StencilNop;
			PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, DepthStencilAccess);

			// Optional GPU-Driven Pipeline binding
			PassParameters->DrawIndirectArgs = DrawIndirectArgs;
		}

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VGSSplat.HardwareRasterize"),
			PassParameters,
			ERDGPassFlags::Raster,
			[PassParameters, bGpuDrivenPipeline, bStochasticDepth, bHardwareDepth, bExpectedDepth, bVertexAttributeFetch, NumSplats, VertexShader, PixelShader, ViewRect = Output.ViewRect](FRDGAsyncTask, FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

				// RT0: premultiplied alpha OVER
				// RT1: stochastic depth
				if (bStochasticDepth)
				{
					GraphicsPSOInit.BlendState = TStaticBlendState<
						CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_Zero, BF_InverseSourceAlpha,
						CW_RED,  BO_Max, BF_One, BF_One, BO_Max, BF_One, BF_One
					>::GetRHI();
				}
				else if (bExpectedDepth)
				{
					// UNDER operator:
					// rgb += src * DestAlpha
					// a = (1 - srcAlpha) * DestAlpha
					GraphicsPSOInit.BlendState = TStaticBlendState<
						CW_RGBA, BO_Add, BF_DestAlpha, BF_One, BO_Add, BF_Zero, BF_InverseSourceAlpha,
						CW_RGBA, BO_Add, BF_DestAlpha, BF_One, BO_Add, BF_Zero, BF_InverseSourceAlpha
					>::GetRHI();
				}
				else
				{
					GraphicsPSOInit.BlendState = TStaticBlendState<
						CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_Zero, BF_InverseSourceAlpha
					>::GetRHI();
				}
				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				GraphicsPSOInit.DepthStencilState = bHardwareDepth
					? TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI()
					: TStaticDepthStencilState<false, CF_GreaterEqual>::GetRHI();
				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = bVertexAttributeFetch
					? GVGSSplatInstanceVertexDeclaration.VertexDeclarationRHI
					: GEmptyVertexDeclaration.VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;
				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

				RHICmdList.SetViewport(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->VS);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);

				if (bVertexAttributeFetch)
				{
					PassParameters->GatheredSplatVertexBuffer->MarkResourceAsUsed();
					RHICmdList.SetStreamSource(0, PassParameters->GatheredSplatVertexBuffer->GetRHI(), 0);
				}

				if (PassParameters->DrawIndirectArgs != nullptr)
				{
					PassParameters->DrawIndirectArgs->MarkResourceAsUsed();
					RHICmdList.DrawPrimitiveIndirect(PassParameters->DrawIndirectArgs->GetIndirectRHICallBuffer(), 0);
				}
				else
				{
					// 2 triangles (triangle strip, 4 vertices) per splat instance
					RHICmdList.DrawPrimitive(0, 2, NumSplats);
				}
			}
		);
	}

	if (bStochasticDepth)
	{
		FVGSSplatResolveDepthPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVGSSplatResolveDepthPS::FParameters>();
		PassParameters->DepthTexture = DepthTexture;
		if (bVelocity)
		{
			PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneVelocityTexture, ERenderTargetLoadAction::ELoad);
		}
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);

		FVGSSplatResolveDepthPS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FVGSSplatResolveDepthPS::FWriteVelocity>(bVelocity);
		TShaderMapRef<FVGSSplatResolveDepthPS> PixelShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);
		FRHIBlendState* BlendState = nullptr;
		FRHIRasterizerState* RasterizerState = nullptr;
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			GetGlobalShaderMap(View.GetShaderPlatform()),
			RDG_EVENT_NAME("VGSSplat.ResolveDepth"),
			PixelShader,
			PassParameters,
			Output.ViewRect,
			BlendState,
			RasterizerState,
			TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI());
	}
	else if (bLit)
	{
		// VGS CHANGE: resolved by VGSSplatGBuffer.usf, which writes scene depth itself.
		check(OutDepthMoments != nullptr);
		*OutDepthMoments = DepthMomentsTexture;
	}
	else if (bExpectedDepth)
	{
		FVGSSplatExpectedDepthPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVGSSplatExpectedDepthPS::FParameters>();
		PassParameters->View = ViewUniformBuffer;
		PassParameters->DepthMomentsTexture = DepthMomentsTexture;
		PassParameters->ExpectedDepthScale = GetExpectedDepthScale();
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);

		TShaderMapRef<FVGSSplatExpectedDepthPS> PixelShader(GetGlobalShaderMap(View.GetShaderPlatform()));
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			GetGlobalShaderMap(View.GetShaderPlatform()),
			RDG_EVENT_NAME("VGSSplat.ExpectedDepth"),
			PixelShader,
			PassParameters,
			Output.ViewRect,
			nullptr,
			nullptr,
			TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI());

		if (bVelocity)
		{
			FVGSSplatResolveVelocityPS::FParameters* VelocityParams = GraphBuilder.AllocParameters<FVGSSplatResolveVelocityPS::FParameters>();
			VelocityParams->View = ViewUniformBuffer;
			VelocityParams->BlueNoise = CreateUniformBufferImmediate(GetBlueNoiseGlobalParameters(), EUniformBufferUsage::UniformBuffer_SingleDraw);
			VelocityParams->DepthMomentsTexture = DepthMomentsTexture;
			VelocityParams->RenderTargets[0] = FRenderTargetBinding(SceneVelocityTexture, ERenderTargetLoadAction::ELoad);

			FVGSSplatResolveVelocityPS::FPermutationDomain VelocityPermutationVector;
			VelocityPermutationVector.Set<FVGSSplatResolveVelocityPS::FUseBlueNoise>(IsBlueNoiseEnabled());
			TShaderMapRef<FVGSSplatResolveVelocityPS> VelocityShader(GetGlobalShaderMap(View.GetShaderPlatform()), VelocityPermutationVector);
			FPixelShaderUtils::AddFullscreenPass(
				GraphBuilder,
				GetGlobalShaderMap(View.GetShaderPlatform()),
				RDG_EVENT_NAME("VGSSplat.ResolveVelocity"),
				VelocityShader,
				VelocityParams,
				Output.ViewRect);
		}
	}
#if 0
	// TODO: Consider re-enabling two-pass approach for animated splats
	if VelocityMode == 2
	{
		TShaderMapRef<FHardwareRasterizeVGSSplatVS> VertexShader(GetGlobalShaderMap(View.GetShaderPlatform()));

		FVGSSplatHardwareRasterizeStochasticDepthPS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FVGSSplatHardwareRasterizeStochasticDepthPS::FUseBlueNoise>(IsBlueNoiseEnabled());
		PermutationVector.Set<FVGSSplatHardwareRasterizeStochasticDepthPS::FWriteVelocity>(bVelocity);
		TShaderMapRef<FVGSSplatHardwareRasterizeStochasticDepthPS> PixelShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);

		FHardwareRasterizeStochasticDepthPassParameters* PassParameters = GraphBuilder.AllocParameters<FHardwareRasterizeStochasticDepthPassParameters>();
		{
			PassParameters->VS.View = ViewUniformBuffer;
			PassParameters->VS.ShadedVGSSplatBuffer = GraphBuilder.CreateSRV(ShadedVGSSplatBuffer);
			PassParameters->VS.SortedIndexBuffer = GraphBuilder.CreateSRV(GPUSortValuesBuffer[SortedBufferIndex], PF_R32_UINT);
			PassParameters->PS.View = ViewUniformBuffer;
			PassParameters->PS.BlueNoise = CreateUniformBufferImmediate(GetBlueNoiseGlobalParameters(), EUniformBufferUsage::UniformBuffer_SingleDraw);
			PassParameters->PS.FalloffParameters = GetFalloffParameters();
			if (bVelocity)
			{
				PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneVelocityTexture, ERenderTargetLoadAction::ELoad);
			}
			PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);
			PassParameters->DrawIndirectArgs = DrawIndirectArgs;
		}

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VGSSplat.StochasticDepth"),
			PassParameters,
			ERDGPassFlags::Raster,
			[PassParameters, NumSplats, VertexShader, PixelShader, ViewRect = Output.ViewRect](FRDGAsyncTask, FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

				GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI();
				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;
				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

				RHICmdList.SetViewport(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->VS);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);

				if (PassParameters->DrawIndirectArgs != nullptr)
				{
					PassParameters->DrawIndirectArgs->MarkResourceAsUsed();
					RHICmdList.DrawPrimitiveIndirect(PassParameters->DrawIndirectArgs->GetIndirectRHICallBuffer(), 0);
				}
				else
				{
					RHICmdList.DrawPrimitive(0, 2, NumSplats);
				}
			}
		);
	}
#endif
}

void RasterizeTiledCompute(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer,
	uint32 NumSplats,
	FRDGBufferRef ShadedVGSSplatBuffer,
	const FScreenPassRenderTarget& Output
)
{
	FIntPoint TileDimension = CalcTileDimension(Output.ViewRect);
	uint32 NumTiles = TileDimension.X * TileDimension.Y;

	FRDGBufferRef TileCountBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumTiles),
		TEXT("VGSSplat.TileCount")
	);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(TileCountBuffer, PF_R32_UINT), 0);

	FRDGBufferRef MinDepthPerTileBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumTiles),
		TEXT("VGSSplat.MinDepthPerTile")
	);
	const uint32 asuint_FLOAT_MAX = 0x7F7FFFFF;
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(MinDepthPerTileBuffer, PF_R32_UINT), asuint_FLOAT_MAX);

	FRDGBufferRef MaxDepthPerTileBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumTiles),
		TEXT("VGSSplat.MaxDepthPerTile")
	);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(MaxDepthPerTileBuffer, PF_R32_UINT), 0);

	FRDGBufferRef TileDebugBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FShadedVGSSplatData), NumSplats),
		TEXT("VGSSplat.TileDebugBuffer")
	);

	{
		FTileCountVGSSplatCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FTileCountVGSSplatCS::FShaderDebug>(IsDebug());
		TShaderMapRef<FTileCountVGSSplatCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);

		FTileCountVGSSplatCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FTileCountVGSSplatCS::FParameters>();
		{
			PassParameters->View = ViewUniformBuffer;
			PassParameters->NumSplats = NumSplats;
			PassParameters->ShadedVGSSplatBuffer = GraphBuilder.CreateSRV(ShadedVGSSplatBuffer);
			PassParameters->TileSize = GetTileSize();
			PassParameters->TileDimension = TileDimension;
			PassParameters->RWTileCountBuffer = GraphBuilder.CreateUAV(TileCountBuffer, PF_R32_UINT);
			PassParameters->RWMinDepthPerTileBuffer = GraphBuilder.CreateUAV(MinDepthPerTileBuffer, PF_R32_UINT);
			PassParameters->RWMaxDepthPerTileBuffer = GraphBuilder.CreateUAV(MaxDepthPerTileBuffer, PF_R32_UINT);
			PassParameters->RWTileDebugBuffer = GraphBuilder.CreateUAV(TileDebugBuffer);
		}

		FIntVector GroupCount = FComputeShaderUtils::GetGroupCountWrapped(PassParameters->NumSplats, FTileCountVGSSplatCS::GetThreadGroupSize1D());
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VGSSplat.TileCount"), ERDGPassFlags::Compute, ComputeShader, PassParameters, GroupCount);
	}

	// Prefix sum
	FRDGBufferRef SplatOffsetPerTileBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumTiles),
		TEXT("VGSSplat.SplatOffsetPerTileBuffer")
	);

	{
		TShaderMapRef<FPrefixSumVGSSplatCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()));
		FPrefixSumVGSSplatCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FPrefixSumVGSSplatCS::FParameters>();
		{
			PassParameters->NumItems = NumTiles;
			PassParameters->ItemBuffer = GraphBuilder.CreateSRV(TileCountBuffer, PF_R32_UINT);
			PassParameters->RWPrefixSumBuffer = GraphBuilder.CreateUAV(SplatOffsetPerTileBuffer, PF_R32_UINT);
		}

		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VGSSplat.PrefixSum"), ERDGPassFlags::Compute, ComputeShader, PassParameters, FIntVector(1, 1, 1));
	}

	// Allocate and fill sort buffers
	int32 MaxSplatOverlap = GetMaxSplatOverlap();
	FRDGBufferRef SortKeyBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats * MaxSplatOverlap),
		TEXT("VGSSplat.SortKeyBuffer")
	);
	FRDGBufferRef SplatIndexBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats * MaxSplatOverlap),
		TEXT("VGSSplat.SplatIndexBuffer")
	);
	FRDGBufferRef NumSplatsPerTileBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumTiles),
		TEXT("VGSSplat.SplatIndexAllocBuffer")
	);
	const uint32 SortElementCount = NumSplats * MaxSplatOverlap;

	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SortKeyBuffer, PF_R32_UINT), 0xffffffff);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(NumSplatsPerTileBuffer, PF_R32_UINT), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SplatIndexBuffer, PF_R32_UINT), 0xffffffff);

		TShaderMapRef<FFillSplatIndexListVGSSplatCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()));
		FFillSplatIndexListVGSSplatCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FFillSplatIndexListVGSSplatCS::FParameters>();
		{
			PassParameters->View = ViewUniformBuffer;
			PassParameters->NumSplats = NumSplats;
			PassParameters->ShadedVGSSplatBuffer = GraphBuilder.CreateSRV(ShadedVGSSplatBuffer);
			PassParameters->TileSize = GetTileSize();
			PassParameters->TileDimension = TileDimension;
			PassParameters->SplatOffsetPerTileBuffer = GraphBuilder.CreateSRV(SplatOffsetPerTileBuffer, PF_R32_UINT);
			PassParameters->MinDepthPerTileBuffer = GraphBuilder.CreateSRV(MinDepthPerTileBuffer, PF_R32_UINT);
			PassParameters->MaxDepthPerTileBuffer = GraphBuilder.CreateSRV(MaxDepthPerTileBuffer, PF_R32_UINT);
			PassParameters->RWNumSplatsPerTileBuffer = GraphBuilder.CreateUAV(NumSplatsPerTileBuffer, PF_R32_UINT);
			PassParameters->RWSortKeyBuffer = GraphBuilder.CreateUAV(SortKeyBuffer, PF_R32_UINT);
			PassParameters->RWSplatIndexBuffer = GraphBuilder.CreateUAV(SplatIndexBuffer, PF_R32_UINT);
		}

		FIntVector GroupCount = FComputeShaderUtils::GetGroupCountWrapped(NumSplats, FFillSplatIndexListVGSSplatCS::GetThreadGroupSize1D());
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VGSSplat.FillSplatIndexList"), ERDGPassFlags::Compute, ComputeShader, PassParameters, GroupCount);
	}

	// GPU Radix sort
	FRDGBufferRef GPUSortKeysBuffer[] = {
		SortKeyBuffer,
		GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), SortElementCount), TEXT("VGSSplat.SortKeys1")),
	};
	FRDGBufferRef GPUSortValuesBuffer[] = {
		SplatIndexBuffer,
		GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), SortElementCount), TEXT("VGSSplat.SortValues1")),
	};
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GPUSortKeysBuffer[1], PF_R32_UINT), 0);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GPUSortValuesBuffer[1], PF_R32_UINT), 0);

	const uint32 KeyMask = 0xFFFFFFFF;
	const int32 SortedBufferIndex = GetGPUSortPassCount(KeyMask) % 2;
	{
		FGPUSortPassParameters* PassParameters = GraphBuilder.AllocParameters<FGPUSortPassParameters>();
		{
			PassParameters->RemoteKeySRVs0 = GraphBuilder.CreateSRV(GPUSortKeysBuffer[0], PF_R32_UINT);
			PassParameters->RemoteKeySRVs1 = GraphBuilder.CreateSRV(GPUSortKeysBuffer[1], PF_R32_UINT);
			PassParameters->RemoteKeyUAVs0 = GraphBuilder.CreateUAV(GPUSortKeysBuffer[0], PF_R32_UINT);
			PassParameters->RemoteKeyUAVs1 = GraphBuilder.CreateUAV(GPUSortKeysBuffer[1], PF_R32_UINT);
			PassParameters->RemoteValueSRVs0 = GraphBuilder.CreateSRV(GPUSortValuesBuffer[0], PF_R32_UINT);
			PassParameters->RemoteValueSRVs1 = GraphBuilder.CreateSRV(GPUSortValuesBuffer[1], PF_R32_UINT);
			PassParameters->RemoteValueUAVs0 = GraphBuilder.CreateUAV(GPUSortValuesBuffer[0], PF_R32_UINT);
			PassParameters->RemoteValueUAVs1 = GraphBuilder.CreateUAV(GPUSortValuesBuffer[1], PF_R32_UINT);
		}

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VGSSplat.SortGPU"),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, SortElementCount, KeyMask, SortedBufferIndex, FeatureLevel = View.GetFeatureLevel()](FRDGAsyncTask, FRHICommandList& RHICmdList)
			{
				FGPUSortBuffers SortBuffers;
				SortBuffers.RemoteKeySRVs[0] = PassParameters->RemoteKeySRVs0->GetRHI();
				SortBuffers.RemoteKeySRVs[1] = PassParameters->RemoteKeySRVs1->GetRHI();
				SortBuffers.RemoteKeyUAVs[0] = PassParameters->RemoteKeyUAVs0->GetRHI();
				SortBuffers.RemoteKeyUAVs[1] = PassParameters->RemoteKeyUAVs1->GetRHI();
				SortBuffers.RemoteValueSRVs[0] = PassParameters->RemoteValueSRVs0->GetRHI();
				SortBuffers.RemoteValueSRVs[1] = PassParameters->RemoteValueSRVs1->GetRHI();
				SortBuffers.RemoteValueUAVs[0] = PassParameters->RemoteValueUAVs0->GetRHI();
				SortBuffers.RemoteValueUAVs[1] = PassParameters->RemoteValueUAVs1->GetRHI();

				int RetVal = SortGPUBuffers(RHICmdList, SortBuffers, 0, KeyMask, SortElementCount, FeatureLevel);
				check(RetVal == SortedBufferIndex);
			}
		);
	}

	// Tile rasterize
	{
		TShaderMapRef<FTileRasterizeVGSSplatCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()));
		FTileRasterizeVGSSplatCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FTileRasterizeVGSSplatCS::FParameters>();
		{
			PassParameters->View = ViewUniformBuffer;
			PassParameters->NumSplats = NumSplats;
			PassParameters->ShadedVGSSplatBuffer = GraphBuilder.CreateSRV(ShadedVGSSplatBuffer);
			PassParameters->TileSize = GetTileSize();
			PassParameters->TileDimension = TileDimension;
			PassParameters->NumSplatsPerTileBuffer = GraphBuilder.CreateSRV(NumSplatsPerTileBuffer, PF_R32_UINT);
			PassParameters->SplatOffsetPerTileBuffer = GraphBuilder.CreateSRV(SplatOffsetPerTileBuffer, PF_R32_UINT);
			PassParameters->SplatIndexBuffer = GraphBuilder.CreateSRV(GPUSortValuesBuffer[SortedBufferIndex], PF_R32_UINT);
			PassParameters->FalloffParameters = GetFalloffParameters();
			PassParameters->RWColorTexture = GraphBuilder.CreateUAV(Output.Texture);
		}

		FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(Output.ViewRect.Size(), FTileRasterizeVGSSplatCS::GetThreadGroupSize2D());
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VGSSplat.TileRasterize"), ERDGPassFlags::Compute, ComputeShader, PassParameters, GroupCount);
	}
}

static void RenderProxyInternal(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer,
	int32 DownsampleFactor,
	const FVGSSplatRenderProxy& RenderProxy,
	FRHITexture* ProxyLUTRHI,
	VGSSplat::FRenderStats& RenderStats,
	const FScreenPassRenderTarget& Output,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneVelocityTexture,
	const VGSSplat::FLitTargets* LitTargets = nullptr) // VGS CHANGE: drawn into the GBuffer
{
	if (RenderProxy.NumSplats == 0)
	{
		return;
	}

	const bool bHWRaster = VGSSplat::IsHardwareRasterization(View.GetShaderPlatform());
	const bool bLit = LitTargets != nullptr; // VGS CHANGE
	const bool bProxyUsesInverseTC = !bLit && ProxyLUTRHI != nullptr && SupportsSplatScratch(bHWRaster);
	// VGS CHANGE: a lit proxy is sorted and blended front to back, as expected depth is.
	const bool bExpectedDepth = bLit || (GetDepthMode(View.GetShaderPlatform()) == EVGSSplatDepthMode::Expected);
	const bool bDownsampled = DownsampleFactor > 1;
	// VGS CHANGE: always through the scratch target, where splats blend in sRGB values; the
	// post-blend pass makes the result linear. Epic only uses it for the inverse tone curve,
	// expected depth or downsampling. Without scratch support (an RGBA16F render target)
	// the colours are composited as they are, sRGB values taken as linear.
	const bool bUsesScratch = SupportsSplatScratch(bHWRaster) || bProxyUsesInverseTC || bExpectedDepth || bDownsampled || bLit;

	FScreenPassRenderTarget ProxyScratchRT;
	if (bUsesScratch)
	{
		// TODO: scissor the scratch + post-blend to the proxy's screen-space bounds
		// rather than the full SceneColor extent. Currently this is one full-screen RT
		// and one full-screen pass per inverse-TC proxy.
		const FScreenPassTextureViewport ScratchViewport = GetSplatViewport(Output, DownsampleFactor);
		ETextureCreateFlags ScratchFlags = TexCreate_ShaderResource | TexCreate_RenderTargetable;
		if (!bHWRaster)
		{
			ScratchFlags |= TexCreate_UAV;
		}
		const FRDGTextureDesc ScratchDesc = FRDGTextureDesc::Create2D(
			ScratchViewport.Extent,
			// The post-blend pass requires transmittance in alpha even when SceneColor
			// uses an RGB-only format (for example, at lower effects quality settings).
			PF_FloatRGBA,
			FClearValueBinding::Black,
			ScratchFlags);
		FRDGTextureRef ScratchTexture = GraphBuilder.CreateTexture(ScratchDesc, TEXT("VGSSplat.SplatScratch"));

		// HW raster consumes the desc's clear binding on first bind below (EClear).
		// Tiled-compute writes via UAV so we clear up-front via the binding here.
		if (!bHWRaster)
		{
			AddClearRenderTargetPass(GraphBuilder, ScratchTexture);
		}
		ProxyScratchRT = FScreenPassRenderTarget(ScratchTexture, ScratchViewport.Rect, ERenderTargetLoadAction::ELoad);
	}

	const FScreenPassRenderTarget& RasterTarget = bUsesScratch ? ProxyScratchRT : Output;

	FRDGBufferRef VGSSplatBuffer = GraphBuilder.RegisterExternalBuffer(RenderProxy.GpuBuffer);
	uint32 NumSplats = RenderProxy.NumSplats;

	// Shaded splats are always stored in the packed 24B view.z layout (FPackedShadedVGSSplatData).
	const uint32 ShadedSplatStride = sizeof(FPackedShadedVGSSplatData);

	FRDGBufferRef ShadedVGSSplatBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(ShadedSplatStride, NumSplats),
		TEXT("VGSSplat.ShadedBuffer")
	);

	// VGS CHANGE: a lit proxy's view-space normals, by record (VGSSplatShading.usf).
	FRDGBufferRef ShadedNormalBuffer = bLit
		? GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSplats), TEXT("VGSSplat.ShadedNormals"))
		: nullptr;

	// Debug pipeline configurations
	const bool bDebug = IsDebug();
	const bool bShadeDebug = bDebug || !RenderProxy.HasClusters() || !bHWRaster;
	const bool bGpuDrivenPipeline = bHWRaster && (!bDebug || VGSSplat::IsGPUDrivenPipeline());
	const bool bClusterDispatch = bGpuDrivenPipeline && RenderProxy.HasClusters() && (!bDebug || IsClusterCullingEnabled());
	const bool bShadeFrustumCull = !bDebug || IsFrustumCullingEnabledForShading();
	if (!bClusterDispatch)
	{
		NumSplats = RenderProxy.GetNumLOD0Splats();
	}
	FRDGBufferRef SplatCountBuffer = nullptr;

	FRDGBufferRef SortKeyBuffer = nullptr;
	FRDGBufferRef SplatIndexBuffer = nullptr;
	if (bHWRaster)
	{
		SortKeyBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats), TEXT("VGSSplat.SortKeyBuffer"));
		SplatIndexBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplats), TEXT("VGSSplat.SplatIndexBuffer"));
	}

	// Cluster culling
	FRDGBufferRef ClusterCountBuffer = nullptr;
	FRDGBufferRef ClusterIndexBuffer = nullptr;
	FRDGBufferRef ClusterBuffer = nullptr;
	if (bClusterDispatch)
	{
		ClusterIndexBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), RenderProxy.NumClusters), TEXT("VGSSplat.ClusterIndexBuffer"));
		ClusterCountBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("VGSSplat.ClusterCount"));

		ClusterBuffer = GraphBuilder.RegisterExternalBuffer(RenderProxy.ClusterBuffer);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ClusterCountBuffer, PF_R32_UINT), 0u);

		TShaderMapRef<FVGSSplatCullClustersCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()));
		FVGSSplatCullClustersCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVGSSplatCullClustersCS::FParameters>();
		{
			// Scene
			PassParameters->View = ViewUniformBuffer;
			PassParameters->LocalToWorld = RenderProxy.LocalToWorld;

			// Cluster info
			PassParameters->NumClusters = RenderProxy.NumClusters;
			PassParameters->PointScaleFactor = RenderProxy.PointScaleFactor;
			PassParameters->ClusterBuffer = GraphBuilder.CreateSRV(ClusterBuffer);

			PassParameters->LOD = CVarVGSSplatLOD.GetValueOnRenderThread();
			PassParameters->ErrorThreshold = CVarVGSSplatErrorThreshold.GetValueOnRenderThread();

			// Output
			PassParameters->RWClusterOutputBuffer = GraphBuilder.CreateUAV(ClusterIndexBuffer, PF_R32_UINT);
			PassParameters->RWClusterCount = GraphBuilder.CreateUAV(ClusterCountBuffer, PF_R32_UINT);
		}

		const FIntVector GroupCount = FComputeShaderUtils::GetGroupCountWrapped(RenderProxy.NumClusters, FVGSSplatCullClustersCS::GetThreadGroupSize1D());

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("VGSSplat.CullClusters"),
			ERDGPassFlags::Compute,
			ComputeShader,
			PassParameters,
			GroupCount);
	}

	// Shade splats
	{
		FRDGBufferRef ShadeIndirectArgs = nullptr;
		if (bClusterDispatch)
		{
			ShadeIndirectArgs = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("VGSSplat.ShadeIndirectArgs")
			);

			FVGSSplatShadeIndirectArgsCS::FParameters* Parameters = GraphBuilder.AllocParameters<FVGSSplatShadeIndirectArgsCS::FParameters>();
			Parameters->ClusterCountBuffer = GraphBuilder.CreateSRV(ClusterCountBuffer, PF_R32_UINT);
			Parameters->DimensionLimit = FUintVector2(GRHIMaxDispatchThreadGroupsPerDimension.X, GRHIMaxDispatchThreadGroupsPerDimension.Y);
			Parameters->RWShadeIndirectArgs = GraphBuilder.CreateUAV(ShadeIndirectArgs, PF_R32_UINT);

			TShaderMapRef<FVGSSplatShadeIndirectArgsCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()));
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("VGSSplat.ShadeIndirectArgs"),
				ComputeShader,
				Parameters,
				FIntVector(1, 1, 1));
		}

		if (bGpuDrivenPipeline)
		{
			SplatCountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("VGSSplat.SplatCount"));
			const uint32 SplatCount = 0u;
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SplatCountBuffer, PF_R32_UINT), SplatCount);
		}

		FShadeVGSSplatCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FShadeVGSSplatCS::FShaderDebug>(bShadeDebug);
		PermutationVector.Set<FShadeVGSSplatCS::FSHDegree>(FMath::Clamp(RenderProxy.SHDegree, 0, 3));
		TShaderMapRef<FShadeVGSSplatCS> ComputeShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);
		FShadeVGSSplatCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FShadeVGSSplatCS::FParameters>();

		auto CreateDummyUAV = [&GraphBuilder](const TCHAR* Name)
		{
			return GraphBuilder.CreateUAV(GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), Name), PF_R32_UINT);
		};
		{
			// Scene
			PassParameters->View = ViewUniformBuffer;

			// Debug pipeline configurations
			PassParameters->DebugGpuDrivenPipeline = bGpuDrivenPipeline ? 1u : 0u;
			PassParameters->DebugClustering = bClusterDispatch ? 1u : 0u;
			PassParameters->DebugFrustumCull = bShadeFrustumCull ? 1u : 0u;
			PassParameters->DebugWriteSortKeys = bHWRaster ? 1u : 0u;

			// Transform
			PassParameters->LocalToWorld = RenderProxy.LocalToWorld;
			PassParameters->WorldToLocal = RenderProxy.WorldToLocal;

			// Splats
			PassParameters->NumSplats = NumSplats;
			PassParameters->VGSSplatBuffer = GraphBuilder.CreateSRV(VGSSplatBuffer);

			// Cluster dispatch
			if (bClusterDispatch)
			{
				PassParameters->ClusterBuffer = GraphBuilder.CreateSRV(ClusterBuffer);
				PassParameters->ClusterIndexBuffer = GraphBuilder.CreateSRV(ClusterIndexBuffer, PF_R32_UINT);
				PassParameters->ClusterCountBuffer = GraphBuilder.CreateSRV(ClusterCountBuffer, PF_R32_UINT);
				PassParameters->IndirectArgs = ShadeIndirectArgs;
			}
			else
			{
				// Default bindings when disabled
				PassParameters->ClusterBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FVGSSplatCluster)));
				PassParameters->ClusterIndexBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultBuffer(GraphBuilder, sizeof(uint32)), PF_R32_UINT);
				PassParameters->ClusterCountBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultBuffer(GraphBuilder, sizeof(uint32)), PF_R32_UINT);
			}

			// Compression
			PassParameters->BufferFormat = RenderProxy.BufferFormat;
			PassParameters->BufferStride = RenderProxy.GpuDataStride;
			PassParameters->PositionScale = RenderProxy.PositionScale;
			PassParameters->PositionBias = RenderProxy.PositionBias;
			PassParameters->ScaleScale = RenderProxy.ScaleScale;
			PassParameters->ScaleBias = RenderProxy.ScaleBias;
			PassParameters->ColorScale = RenderProxy.ColorScale;
			PassParameters->ColorBias = RenderProxy.ColorBias;
			PassParameters->SHScale = RenderProxy.SHScale;
			PassParameters->SHBias = RenderProxy.SHBias;

			// Spherical harmonics
			PassParameters->SHCoordinateSpace = static_cast<uint32>(RenderProxy.SHCoordinateSpace);
			PassParameters->BlendInSRGB = RenderProxy.bBlendInSRGB ? 1u : 0u; // VGS CHANGE

			// VGS CHANGE: a lit proxy's normals
			const bool bHasNormals = bLit && RenderProxy.NormalBuffer.IsValid();
			PassParameters->Lit = bLit ? 1u : 0u;
			PassParameters->HasNormals = bHasNormals ? (RenderProxy.bNormalsFaceCamera ? 2u : 1u) : 0u;
			PassParameters->NormalBuffer = bHasNormals
				? GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(RenderProxy.NormalBuffer))
				: GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(uint32)));
			PassParameters->RWShadedNormalBuffer = GraphBuilder.CreateUAV(bLit
				? ShadedNormalBuffer
				: GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("VGSSplat.ShadedNormals.Dummy")));

			// Per-component overrides
			PassParameters->PointScaleFactor = RenderProxy.PointScaleFactor;
			PassParameters->OpacityScale     = RenderProxy.OpacityScale;

			PassParameters->OpacityThreshold = GetShadingOpacityThreshold();
			PassParameters->CovarianceScale = bHWRaster ? 1.0f / float(DownsampleFactor * DownsampleFactor) : 1.0f;

			// Output
			PassParameters->RWShadedVGSSplatBuffer = GraphBuilder.CreateUAV(ShadedVGSSplatBuffer);
			PassParameters->RWSplatCount = SplatCountBuffer
				? GraphBuilder.CreateUAV(SplatCountBuffer, PF_R32_UINT)
				: CreateDummyUAV(TEXT("VGSSplat.SplatCount.Dummy"));

			PassParameters->RWSortKeys   = bHWRaster ? GraphBuilder.CreateUAV(SortKeyBuffer, PF_R32_UINT)    : CreateDummyUAV(TEXT("VGSSplat.SortKeyBuffer.Dummy"));
			PassParameters->RWSortValues = bHWRaster ? GraphBuilder.CreateUAV(SplatIndexBuffer, PF_R32_UINT) : CreateDummyUAV(TEXT("VGSSplat.SplatIndexBuffer.Dummy"));
			PassParameters->SortKeyFrontToBack = bExpectedDepth ? 1u : 0u;
			PassParameters->SortKeyBits        = GetSortKeyBits(View.GetShaderPlatform());
		}

		if (bClusterDispatch)
		{
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VGSSplat.Shade(Clusters)"), ERDGPassFlags::Compute, ComputeShader, PassParameters, ShadeIndirectArgs, 0);
		}
		else
		{
			const FIntVector GroupCount = FComputeShaderUtils::GetGroupCountWrapped(PassParameters->NumSplats, FShadeVGSSplatCS::GetThreadGroupSize1D());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VGSSplat.Shade"), ERDGPassFlags::Compute, ComputeShader, PassParameters, GroupCount);
		}
	}

	// Rasterization
	FRDGTextureRef DepthMoments = nullptr; // VGS CHANGE: a lit proxy's, for the GBuffer pass
	if (bHWRaster)
	{
		const ERenderTargetLoadAction ColorLoadAction = bUsesScratch ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad;
		const uint32 NumClusters = bClusterDispatch ? RenderProxy.NumClusters : 0u;
		RasterizeHardware(GraphBuilder, View, ViewUniformBuffer, DownsampleFactor, SceneDepthTexture, SceneVelocityTexture, NumSplats, ShadedVGSSplatBuffer, RasterTarget, SplatCountBuffer,
			SortKeyBuffer, SplatIndexBuffer, NumClusters, ClusterCountBuffer, RenderStats, ColorLoadAction,
			ShadedNormalBuffer, bLit ? &DepthMoments : nullptr); // VGS CHANGE
	}
	else
	{
		// TODO: Missing indirect dispatch and depth + velocity support
		RasterizeTiledCompute(GraphBuilder, View, ViewUniformBuffer, NumSplats, ShadedVGSSplatBuffer, RasterTarget);
	}

	// VGS CHANGE: a lit proxy goes into the GBuffer instead of being composited.
	if (bLit)
	{
		if (DepthMoments == nullptr)
		{
			return;
		}
		FVGSSplatGBufferPS::FParameters* Params = GraphBuilder.AllocParameters<FVGSSplatGBufferPS::FParameters>();
		Params->View = View.ViewUniformBuffer;
		Params->SplatColorTexture = ProxyScratchRT.Texture;
		Params->DepthMomentsTexture = DepthMoments;
		Params->ExpectedDepthScale = GetExpectedDepthScale();
		Params->BlendInSRGB = RenderProxy.bBlendInSRGB ? 1u : 0u;
		Params->SelfIllumination = RenderProxy.SelfIllumination;
		Params->Intensity = RenderProxy.Intensity;
		Params->Roughness = RenderProxy.Roughness;
		Params->Specular = RenderProxy.Specular;
		Params->MinCoverage = FMath::Clamp(CVarVGSSplatLitMinCoverage.GetValueOnRenderThread(), 0.0f, 1.0f);
		Params->Dither = CVarVGSSplatLitDither.GetValueOnRenderThread() != 0 ? 1u : 0u;
		const float SmoothingOverride = CVarVGSSplatLitNormalSmoothing.GetValueOnRenderThread();
		Params->NormalSmoothing = FMath::Max(SmoothingOverride >= 0.0f ? SmoothingOverride : RenderProxy.NormalSmoothing, 0.0f);
		Params->Grade = RenderProxy.Grade;
		Params->Tint = RenderProxy.Tint;
		Params->GradeActive = (RenderProxy.Grade.Equals(FVector3f(1.0f, 1.0f, 1.0f), 1e-4f) && RenderProxy.Tint.Equals(FVector3f(1.0f, 1.0f, 1.0f), 1e-4f)) ? 0u : 1u;
		Params->RenderTargets[0] = FRenderTargetBinding(LitTargets->SceneColor, ERenderTargetLoadAction::ELoad);
		if (LitTargets->bSubstrateMaterial)
		{
			// The order VGSSplatGBuffer.usf writes: the material's first three slices, the top layer.
			for (int32 Slice = 0; Slice < 3; ++Slice)
			{
				Params->RenderTargets[1 + Slice] = FRenderTargetBinding(LitTargets->SubstrateMaterial, ERenderTargetLoadAction::ELoad, 0, int16(Slice));
			}
			Params->RenderTargets[4] = FRenderTargetBinding(LitTargets->SubstrateTopLayer, ERenderTargetLoadAction::ELoad);
			Params->MaterialTextureArrayUAV = LitTargets->SubstrateMaterialUAV;
			Params->MaxBytesPerPixel = LitTargets->SubstrateMaxBytesPerPixel;
			Params->bRoughDiffuse = LitTargets->bSubstrateRoughDiffuse ? 1u : 0u;
		}
		else
		{
			Params->RenderTargets[1] = FRenderTargetBinding(LitTargets->GBufferA, ERenderTargetLoadAction::ELoad);
			Params->RenderTargets[2] = FRenderTargetBinding(LitTargets->GBufferB, ERenderTargetLoadAction::ELoad);
			Params->RenderTargets[3] = FRenderTargetBinding(LitTargets->GBufferC, ERenderTargetLoadAction::ELoad);
		}
		Params->RenderTargets.DepthStencil = FDepthStencilBinding(LitTargets->SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthWrite_StencilNop);

		TShaderMapRef<FVGSSplatGBufferPS> PixelShader(GetGlobalShaderMap(View.GetShaderPlatform()));
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			GetGlobalShaderMap(View.GetShaderPlatform()),
			RDG_EVENT_NAME("VGSSplat.GBuffer"),
			PixelShader,
			Params,
			Output.ViewRect,
			nullptr,
			nullptr,
			TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI());
		return;
	}

	// Post-processing
	if (bUsesScratch)
	{
		FVGSSplatInverseToneCurvePostBlendPS::FParameters* Params = GraphBuilder.AllocParameters<FVGSSplatInverseToneCurvePostBlendPS::FParameters>();
		Params->RenderTargets[0]           = FRenderTargetBinding(Output.Texture, ERenderTargetLoadAction::ELoad);
		Params->View                       = View.ViewUniformBuffer;
		Params->SplatColor                 = GetScreenPassTextureInput(
			FScreenPassTexture(ProxyScratchRT.Texture, ProxyScratchRT.ViewRect),
			bDownsampled ? TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI() : TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());
		Params->SvPositionToInputTextureUV =
			FScreenTransform::ChangeTextureBasisFromTo(FScreenPassTextureViewport(Output), FScreenTransform::ETextureBasis::TexelPosition, FScreenTransform::ETextureBasis::ViewportUV) *
			FScreenTransform::ChangeTextureBasisFromTo(FScreenPassTextureViewport(ProxyScratchRT.Texture, ProxyScratchRT.ViewRect), FScreenTransform::ETextureBasis::ViewportUV, FScreenTransform::ETextureBasis::TextureUV);
		if (bProxyUsesInverseTC)
		{
			Params->InverseToneCurveLUT        = ProxyLUTRHI;
			Params->InverseToneCurveLUTSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			Params->Strength                   = RenderProxy.InverseToneCurveStrength;
			Params->InverseToneCurveLUTEdge    = static_cast<float>(ProxyLUTRHI->GetSizeX());
		}
		Params->Intensity = RenderProxy.Intensity; // VGS CHANGE
		Params->BlendInSRGB = RenderProxy.bBlendInSRGB ? 1u : 0u; // VGS CHANGE
		Params->Grade = RenderProxy.Grade; // VGS CHANGE
		Params->Tint = RenderProxy.Tint; // VGS CHANGE
		Params->GradeActive = (RenderProxy.Grade.Equals(FVector3f(1.0f, 1.0f, 1.0f), 1e-4f) && RenderProxy.Tint.Equals(FVector3f(1.0f, 1.0f, 1.0f), 1e-4f)) ? 0u : 1u; // VGS CHANGE

		FVGSSplatInverseToneCurvePostBlendPS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FVGSSplatInverseToneCurvePostBlendPS::FApplyInverseToneCurve>(bProxyUsesInverseTC);
		TShaderMapRef<FVGSSplatInverseToneCurvePostBlendPS> PixelShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			GetGlobalShaderMap(View.GetShaderPlatform()),
			RDG_EVENT_NAME("VGSSplat.InverseToneCurvePostBlend.%s",
				bProxyUsesInverseTC ? (RenderProxy.bInvertExpandGamutLUT ? TEXT(".FilmicTC_Full") : TEXT(".FilmicTC")) : TEXT("")),
			PixelShader,
			Params,
			Output.ViewRect,
			// PS outputs (Color, Transmittance): color uses premult alpha-over via
			// SrcAlpha (= transmittance); alpha multiplies transmittances to match UE
			// SceneColor convention.
			TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_SourceAlpha, BO_Add, BF_Zero, BF_SourceAlpha>::GetRHI());
	}
}

// VGS CHANGE: lit proxies, drawn into the GBuffer after the base pass.
bool VGSSplat::SupportsLit(const FSceneView& View)
{
	const EShaderPlatform Platform = View.GetShaderPlatform();
	return View.bIsViewInfo
		&& View.GetFeatureLevel() >= ERHIFeatureLevel::SM5
		&& IsHardwareRasterization(Platform)
		&& SupportsSplatScratch(true)
		&& FDataDrivenShaderPlatformInfo::GetSupportsVertexShaderSRVs(Platform)
		&& !IsVertexAttributeFetchEnabled(Platform)
		&& !IsForwardShadingEnabled(Platform);
}

void VGSSplat::RenderLit(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	TConstArrayView<FVGSSplatRenderItem> RenderItems,
	VGSSplat::FRenderStats& RenderStats,
	const FLitTargets& Targets)
{
	RDG_EVENT_SCOPE_STAT(GraphBuilder, VGSSplat, "GaussianSplat.Lit");
	if (View.Family == nullptr || View.Family->Scene == nullptr || RenderItems.IsEmpty()
		|| !Targets.SceneColor || !Targets.SceneDepth
		|| (Targets.bSubstrateMaterial ? (!Targets.SubstrateMaterial || !Targets.SubstrateMaterialUAV || !Targets.SubstrateTopLayer) : (!Targets.GBufferA || !Targets.GBufferB || !Targets.GBufferC)))
	{
		return;
	}
	const FScreenPassRenderTarget Output(Targets.SceneColor, Targets.ViewRect, ERenderTargetLoadAction::ELoad);
	for (const FVGSSplatRenderItem& Item : RenderItems)
	{
		// Full resolution, the base pass's: no downsampling for a lit proxy.
		RenderProxyInternal(GraphBuilder, View, View.ViewUniformBuffer, 1, *Item.Proxy, nullptr, RenderStats, Output, Targets.SceneDepth, nullptr, &Targets);
	}
}

void VGSSplat::Render(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	TConstArrayView<FVGSSplatRenderItem> RenderItems,
	VGSSplat::FRenderStats& RenderStats,
	const FScreenPassRenderTarget& Output,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneVelocityTexture)
{
	RDG_EVENT_SCOPE_STAT(GraphBuilder, VGSSplat, "GaussianSplat");

	if (View.Family == nullptr || View.Family->Scene == nullptr || RenderItems.IsEmpty())
	{
		return;
	}

	TUniformBufferRef<FViewUniformShaderParameters> ViewUniformBuffer = View.ViewUniformBuffer;
	FRDGTextureRef DepthTexture = SceneDepthTexture;
	FRDGTextureRef VelocityTexture = SceneVelocityTexture;
	FRDGTextureRef DownsampledSceneDepthTexture = nullptr;

	const int32 DownsampleFactor = View.bIsViewInfo ? GetDownsampleFactor() : 1;
	if (DownsampleFactor > 1)
	{
		const FScreenPassTextureViewport DownsampledViewport = GetSplatViewport(Output, DownsampleFactor);
		ViewUniformBuffer = CreateDownsampledViewUniformBuffer(View, DownsampledViewport);
		DepthTexture = nullptr;
		VelocityTexture = nullptr;

		if (SceneDepthTexture != nullptr && VGSSplat::IsHardwareRasterization(View.GetShaderPlatform()))
		{
			const FRDGTextureDesc DepthDesc = FRDGTextureDesc::Create2D(
				DownsampledViewport.Extent,
				SceneDepthTexture->Desc.Format,
				FClearValueBinding::DepthFar,
				TexCreate_DepthStencilTargetable | TexCreate_ShaderResource);
			DepthTexture = GraphBuilder.CreateTexture(DepthDesc, TEXT("VGSSplat.DownsampledDepth"));

			FVGSSplatDownsampleDepthPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVGSSplatDownsampleDepthPS::FParameters>();
			PassParameters->SceneDepthTexture = SceneDepthTexture;
			PassParameters->SvPositionToSourcePixel = FScreenTransform(FVector2f(Output.ViewRect.Size()) / FVector2f(DownsampledViewport.Extent), FVector2f(Output.ViewRect.Min));
			PassParameters->SourcePixelMax = Output.ViewRect.Max - FIntPoint(1, 1);
			PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(DepthTexture, ERenderTargetLoadAction::ENoAction, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);

			TShaderMapRef<FVGSSplatDownsampleDepthPS> PixelShader(GetGlobalShaderMap(View.GetShaderPlatform()));
			FPixelShaderUtils::AddFullscreenPass(
				GraphBuilder,
				GetGlobalShaderMap(View.GetShaderPlatform()),
				RDG_EVENT_NAME("VGSSplat.DownsampleDepth(%dx)", DownsampleFactor),
				PixelShader,
				PassParameters,
				DownsampledViewport.Rect,
				nullptr,
				nullptr,
				TStaticDepthStencilState<true, CF_Always>::GetRHI());

			if (GetDepthMode(View.GetShaderPlatform()) != EVGSSplatDepthMode::Off)
			{
				DownsampledSceneDepthTexture = GraphBuilder.CreateTexture(DepthDesc, TEXT("VGSSplat.DownsampledSceneDepth"));
				AddCopyTexturePass(GraphBuilder, DepthTexture, DownsampledSceneDepthTexture);
			}
		}
	}

	for (const FVGSSplatRenderItem& Item : RenderItems)
	{
		RenderProxyInternal(GraphBuilder, View, ViewUniformBuffer, DownsampleFactor, *Item.Proxy, Item.InverseToneCurveLUTRHI, RenderStats, Output, DepthTexture, VelocityTexture);
	}

	if (DownsampledSceneDepthTexture != nullptr)
	{
		const bool bVelocity = IsVelocityEnabled()
			&& SceneVelocityTexture != nullptr
			&& EnumHasAnyFlags(SceneVelocityTexture->Desc.Flags, TexCreate_RenderTargetable);

		FVGSSplatUpsampleDepthPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVGSSplatUpsampleDepthPS::FParameters>();
		PassParameters->SplatDepthTexture = DepthTexture;
		PassParameters->SceneDepthTexture = DownsampledSceneDepthTexture;
		PassParameters->SvPositionToDownsampledPixel = (FScreenTransform::Identity - Output.ViewRect.Min) * (FVector2f(DepthTexture->Desc.Extent) / FVector2f(Output.ViewRect.Size()));
		if (bVelocity)
		{
			PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneVelocityTexture, ERenderTargetLoadAction::ELoad);
		}
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);

		FVGSSplatUpsampleDepthPS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FVGSSplatUpsampleDepthPS::FWriteVelocity>(bVelocity);
		TShaderMapRef<FVGSSplatUpsampleDepthPS> PixelShader(GetGlobalShaderMap(View.GetShaderPlatform()), PermutationVector);
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			GetGlobalShaderMap(View.GetShaderPlatform()),
			RDG_EVENT_NAME("VGSSplat.UpsampleDepth(%dx)", DownsampleFactor),
			PixelShader,
			PassParameters,
			Output.ViewRect,
			nullptr,
			nullptr,
			TStaticDepthStencilState<true, CF_GreaterEqual>::GetRHI());
	}
}
