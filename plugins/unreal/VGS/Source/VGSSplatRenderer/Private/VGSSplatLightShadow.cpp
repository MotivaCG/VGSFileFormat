// VGS: not Epic's. See VGSSplatLightShadow.h.
#include "VGSSplatLightShadow.h"

#include "VGSSplatRenderPipeline.h"

#include "CommonRenderResources.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "LightSceneInfo.h"
#include "LightSceneProxy.h"
#include "PixelShaderUtils.h"
#include "PostProcess/PostProcessInputs.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "RHIStaticStates.h"
#include "ScenePrivate.h"
#include "SceneRendering.h"
#include "SceneTexturesConfig.h"
#include "ShaderParameterStruct.h"
#include "Substrate/Substrate.h"
#include "SystemTextures.h"

static TAutoConsoleVariable<float> CVarVGSSplatShadowTexelSize(
	TEXT("r.VGSSplat.Shadow.TexelSize"),
	0.5f,
	TEXT("With r.VGSSplat.Shadow.Method 0 or 1: the size (cm) of a texel of the splats' own shadow map, which covers all the casting splats\n")
	TEXT("(up to r.VGSSplat.Shadow.MaxMapSize texels a side; coarser beyond)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatShadowMaxMapSize(
	TEXT("r.VGSSplat.Shadow.MaxMapSize"),
	2048,
	TEXT("With r.VGSSplat.Shadow.Method 0 or 1: the largest side of the splats' own shadow map, in texels."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatShadowSelf(
	TEXT("r.VGSSplat.Shadow.Self"),
	0,
	TEXT("With r.VGSSplat.Shadow.Method 0 or 1: 1 also shadows the lit captures themselves (self-shadowing); 0 only what is behind them."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarVGSSplatShadowBias(
	TEXT("r.VGSSplat.Shadow.Bias"),
	3.0f,
	TEXT("With r.VGSSplat.Shadow.Method 0 or 1: how far (cm) behind the splats a surface must be to be shadowed."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarVGSSplatShadowSoftness(
	TEXT("r.VGSSplat.Shadow.Softness"),
	1.0f,
	TEXT("With r.VGSSplat.Shadow.Method 0 or 1: the penumbra, as a multiple of what the light's source angle gives."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarVGSSplatShadowDepthOpacity(
	TEXT("r.VGSSplat.Shadow.DepthOpacity"),
	0.1f,
	TEXT("With r.VGSSplat.Shadow.Method 0 or 1: how opaque a splat must be over a texel to say where the capture is along the light."),
	ECVF_RenderThreadSafe);

BEGIN_SHADER_PARAMETER_STRUCT(FVGSSplatLightShadowMapParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, Splats)
	SHADER_PARAMETER(FMatrix44f, LocalToLight)
	SHADER_PARAMETER(FVector3f, PositionScale)
	SHADER_PARAMETER(uint32, NumSplats)
	SHADER_PARAMETER(FVector3f, PositionBias)
	SHADER_PARAMETER(uint32, Stride)
	SHADER_PARAMETER(FVector3f, ScaleScale)
	SHADER_PARAMETER(uint32, BufferFormat)
	SHADER_PARAMETER(FVector3f, ScaleBias)
	SHADER_PARAMETER(float, PointScale)
	SHADER_PARAMETER(FVector2f, OpacityQuantization)
	SHADER_PARAMETER(float, OpacityScale)
	SHADER_PARAMETER(float, DepthOpacity)
	SHADER_PARAMETER(FVector4f, LightToClip)
	SHADER_PARAMETER(float, Dilation)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

class FVGSSplatLightShadowMapVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVGSSplatLightShadowMapVS);
	using FParameters = FVGSSplatLightShadowMapParameters;
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatLightShadowMapVS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VGS_LIGHT_SHADOW_MAP"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVGSSplatLightShadowMapVS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatLightShadow.usf", "MainVS", SF_Vertex);

class FVGSSplatLightShadowMapPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVGSSplatLightShadowMapPS);
	using FParameters = FVGSSplatLightShadowMapParameters;
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatLightShadowMapPS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VGS_LIGHT_SHADOW_MAP"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVGSSplatLightShadowMapPS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatLightShadow.usf", "MainPS", SF_Pixel);

class FVGSSplatLightShadowCompositePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVGSSplatLightShadowCompositePS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatLightShadowCompositePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorCopy)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferA)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferB)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferC)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, TransmittanceMap)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, DepthMap)
		SHADER_PARAMETER_SAMPLER(SamplerState, BilinearSampler)
		SHADER_PARAMETER(FVector3f, LightOrigin)
		SHADER_PARAMETER(FVector3f, LightX)
		SHADER_PARAMETER(FVector3f, LightY)
		SHADER_PARAMETER(FVector3f, LightZ)
		SHADER_PARAMETER(FVector3f, ToLight)
		SHADER_PARAMETER(FVector3f, LightColor)
		SHADER_PARAMETER(FVector4f, LightToUV)
		SHADER_PARAMETER(float, MapSize)
		SHADER_PARAMETER(float, Bias)
		SHADER_PARAMETER(float, PenumbraPerDepth)
		SHADER_PARAMETER(uint32, SelfShadow)
		SHADER_PARAMETER(uint32, HasGBuffer)
		SHADER_PARAMETER(float, FallbackStrength)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VGS_LIGHT_SHADOW_COMPOSITE"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVGSSplatLightShadowCompositePS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatLightShadow.usf", "MainPS", SF_Pixel);

const FLightSceneInfo* VGSSplat::FindMainLight(const FScene& Scene)
{
	const FLightSceneInfo* Brightest = nullptr;
	float BrightestLuminance = -1.0f;
	for (const FLightSceneInfo* Light : Scene.DirectionalLights)
	{
		if (Light == nullptr || Light->Proxy == nullptr || !Light->Proxy->CastsDynamicShadow())
		{
			continue;
		}
		if (Light->Proxy->IsUsedAsAtmosphereSunLight() && Light->Proxy->GetAtmosphereSunLightIndex() == 0)
		{
			return Light;
		}
		const float Luminance = Light->Proxy->GetColor().GetLuminance();
		if (Luminance > BrightestLuminance)
		{
			Brightest = Light;
			BrightestLuminance = Luminance;
		}
	}
	return Brightest;
}

FVector VGSSplat::GetMainLightDirection(const FLightSceneInfo& Light)
{
	return Light.Proxy->GetDirection().GetSafeNormal();
}

void VGSSplat::RenderLightShadow(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs, TConstArrayView<const FVGSSplatRenderProxy*> Proxies)
{
	if (!View.bIsViewInfo || !Inputs.SceneTextures || View.Family == nullptr || View.Family->Scene == nullptr
		|| View.GetFeatureLevel() < ERHIFeatureLevel::SM5)
	{
		return;
	}
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
	if (!ViewInfo.CachedViewUniformShaderParameters.IsValid())
	{
		return;
	}
	const FScene* Scene = View.Family->Scene->GetRenderScene();
	const FLightSceneInfo* MainLight = Scene ? VGSSplat::FindMainLight(*Scene) : nullptr;
	if (MainLight == nullptr)
	{
		return;
	}
	// Its colour over pi, past the atmosphere, as the view's DirectionalLightColor has it
	// (FViewInfo::SetupUniformBufferParameters), whichever light that one is.
	const FLinearColor LightColor = MainLight->Proxy->GetAtmosphereTransmittanceTowardSun() * MainLight->Proxy->GetColor() / PI;
	if (LightColor.GetMax() <= 0.0f)
	{
		return;
	}
	const FVector3f ToLight = FVector3f(-VGSSplat::GetMainLightDirection(*MainLight));

	TArray<const FVGSSplatRenderProxy*, TInlineAllocator<8>> Casters;
	FBox Bounds(ForceInit);
	for (const FVGSSplatRenderProxy* Proxy : Proxies)
	{
		if (Proxy && Proxy->bVisible && Proxy->bCastShadow && Proxy->GpuBuffer.IsValid() && Proxy->NumSplats > 0 && Proxy->LocalBounds.IsValid)
		{
			Casters.Add(Proxy);
			Bounds += FBox(FVector(Proxy->LocalBounds.Min), FVector(Proxy->LocalBounds.Max)).TransformBy(FMatrix(Proxy->LocalToWorld));
		}
	}
	if (Casters.IsEmpty())
	{
		return;
	}
	RDG_EVENT_SCOPE(GraphBuilder, "VGSSplat.LightShadow");

	// The light's frame: z the way it goes, from the casters' centre.
	const FVector Z = -FVector(ToLight).GetSafeNormal();
	FVector X, Y;
	Z.FindBestAxisVectors(X, Y);
	const FVector Origin = Bounds.GetCenter();
	const FMatrix WorldToLight = FTranslationMatrix(-Origin) * FMatrix(
		FPlane(X.X, Y.X, Z.X, 0.0),
		FPlane(X.Y, Y.Y, Z.Y, 0.0),
		FPlane(X.Z, Y.Z, Z.Z, 0.0),
		FPlane(0.0, 0.0, 0.0, 1.0));
	double Half = 0.0;
	for (int32 Corner = 0; Corner < 8; ++Corner)
	{
		const FVector P((Corner & 1) ? Bounds.Max.X : Bounds.Min.X, (Corner & 2) ? Bounds.Max.Y : Bounds.Min.Y, (Corner & 4) ? Bounds.Max.Z : Bounds.Min.Z);
		const FVector L = WorldToLight.TransformPosition(P);
		Half = FMath::Max(Half, FMath::Max(FMath::Abs(L.X), FMath::Abs(L.Y)));
	}
	Half = Half * 1.05 + 10.0; // the splats reach past their centres' bounds
	// Texels of a fixed size in the world, as long as the map stays within its largest side: the
	// map's cost is its fill, the splats' footprints in texels.
	const double WantedTexel = FMath::Max(double(CVarVGSSplatShadowTexelSize.GetValueOnRenderThread()), 0.05);
	const int32 MaxSize = FMath::Clamp(CVarVGSSplatShadowMaxMapSize.GetValueOnRenderThread(), 256, 8192);
	const int32 Size = FMath::Clamp(Align(FMath::CeilToInt(2.0 * Half / WantedTexel), 64), 256, MaxSize);
	const double Texel = 2.0 * Half / Size;

	FRDGTextureRef TransmittanceMap = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(FIntPoint(Size), PF_R16F, FClearValueBinding(FLinearColor::White), TexCreate_RenderTargetable | TexCreate_ShaderResource),
		TEXT("VGSSplat.LightShadow.Transmittance"));
	// Depths from the casters' centre, in half floats: a few hundred units, to a fraction of one.
	const FLinearColor Far(60000.0f, 60000.0f, 0.0f, 0.0f);
	FRDGTextureRef DepthMap = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(FIntPoint(Size), PF_G16R16F, FClearValueBinding(Far), TexCreate_RenderTargetable | TexCreate_ShaderResource),
		TEXT("VGSSplat.LightShadow.Depths"));
	AddClearRenderTargetPass(GraphBuilder, TransmittanceMap, FLinearColor::White);
	AddClearRenderTargetPass(GraphBuilder, DepthMap, Far);

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetShaderPlatform());
	TShaderMapRef<FVGSSplatLightShadowMapVS> MapVS(ShaderMap);
	TShaderMapRef<FVGSSplatLightShadowMapPS> MapPS(ShaderMap);
	const float DepthOpacity = FMath::Clamp(CVarVGSSplatShadowDepthOpacity.GetValueOnRenderThread(), 0.01f, 0.99f);
	for (const FVGSSplatRenderProxy* Proxy : Casters)
	{
		FVGSSplatLightShadowMapParameters* Params = GraphBuilder.AllocParameters<FVGSSplatLightShadowMapParameters>();
		Params->Splats = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(Proxy->GpuBuffer));
		Params->LocalToLight = FMatrix44f(FMatrix(Proxy->LocalToWorld) * WorldToLight);
		Params->PositionScale = Proxy->PositionScale;
		Params->NumSplats = Proxy->NumSplats;
		Params->PositionBias = Proxy->PositionBias;
		Params->Stride = Proxy->GpuDataStride;
		Params->ScaleScale = Proxy->ScaleScale;
		Params->BufferFormat = Proxy->BufferFormat;
		Params->ScaleBias = Proxy->ScaleBias;
		Params->PointScale = Proxy->PointScaleFactor;
		Params->OpacityQuantization = FVector2f(Proxy->ColorScale.W, Proxy->ColorBias.W);
		Params->OpacityScale = Proxy->OpacityScale;
		Params->DepthOpacity = DepthOpacity;
		Params->LightToClip = FVector4f(float(1.0 / Half), float(1.0 / Half), 0.0f, 0.0f);
		Params->Dilation = float(0.25 * Texel * Texel);
		Params->RenderTargets[0] = FRenderTargetBinding(TransmittanceMap, ERenderTargetLoadAction::ELoad);
		Params->RenderTargets[1] = FRenderTargetBinding(DepthMap, ERenderTargetLoadAction::ELoad);

		const uint32 NumSplats = Proxy->NumSplats;
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VGSSplat.LightShadow.Map %u splats %dx%d", NumSplats, Size, Size),
			Params,
			ERDGPassFlags::Raster,
			[Params, MapVS, MapPS, Size, NumSplats](FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer PSO;
				RHICmdList.ApplyCachedRenderTargets(PSO);
				// Transmittance multiplied; depths (d, -d) min blended: the nearest and farthest.
				PSO.BlendState = TStaticBlendState<
					CW_RED, BO_Add, BF_Zero, BF_SourceColor, BO_Add, BF_Zero, BF_One,
					CW_RG, BO_Min, BF_One, BF_One, BO_Min, BF_One, BF_One>::GetRHI();
				PSO.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				PSO.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
				PSO.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
				PSO.BoundShaderState.VertexShaderRHI = MapVS.GetVertexShader();
				PSO.BoundShaderState.PixelShaderRHI = MapPS.GetPixelShader();
				PSO.PrimitiveType = PT_TriangleList;
				SetGraphicsPipelineState(RHICmdList, PSO, 0);
				SetShaderParameters(RHICmdList, MapVS, MapVS.GetVertexShader(), *Params);
				SetShaderParameters(RHICmdList, MapPS, MapPS.GetPixelShader(), *Params);
				RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, float(Size), float(Size), 1.0f);
				RHICmdList.DrawPrimitive(0, NumSplats * 2, 1);
			});
	}

	// Composite onto the lit scene, from a copy of it.
	const FSceneTextureUniformParameters& SceneTextures = *Inputs.SceneTextures->GetContents();
	FRDGTextureRef SceneColor = SceneTextures.SceneColorTexture;
	FRDGTextureRef SceneDepth = SceneTextures.SceneDepthTexture;
	if (!SceneColor || !SceneDepth || SceneColor->Desc.NumSamples > 1)
	{
		return;
	}
	FRDGTextureRef SceneColorCopy = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(SceneColor->Desc.Extent, SceneColor->Desc.Format, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_RenderTargetable),
		TEXT("VGSSplat.LightShadow.SceneColor"));
	AddCopyTexturePass(GraphBuilder, SceneColor, SceneColorCopy);

	const bool bSubstrateMaterial = Substrate::IsSubstrateEnabled() && !Substrate::IsSubstrateBlendableGBufferEnabled(View.GetShaderPlatform());
	const bool bGBuffer = !bSubstrateMaterial && SceneTextures.GBufferATexture && SceneTextures.GBufferBTexture && SceneTextures.GBufferCTexture;
	FRDGTextureRef Black = GSystemTextures.GetBlackDummy(GraphBuilder);

	const float SourceAngle = MainLight->Proxy->GetLightSourceAngle(); // degrees
	const double PenumbraRadius = 0.5 * FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(double(SourceAngle), 0.0, 30.0)));

	FVGSSplatLightShadowCompositePS::FParameters* Params = GraphBuilder.AllocParameters<FVGSSplatLightShadowCompositePS::FParameters>();
	Params->View = View.ViewUniformBuffer;
	Params->SceneColorCopy = SceneColorCopy;
	Params->SceneDepth = SceneDepth;
	Params->GBufferA = bGBuffer ? SceneTextures.GBufferATexture : Black;
	Params->GBufferB = bGBuffer ? SceneTextures.GBufferBTexture : Black;
	Params->GBufferC = bGBuffer ? SceneTextures.GBufferCTexture : Black;
	Params->TransmittanceMap = TransmittanceMap;
	Params->DepthMap = DepthMap;
	Params->BilinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Params->LightOrigin = FVector3f(Origin + View.ViewMatrices.GetPreViewTranslation());
	Params->LightX = FVector3f(X);
	Params->LightY = FVector3f(Y);
	Params->LightZ = FVector3f(Z);
	Params->ToLight = ToLight;
	Params->LightColor = FVector3f(LightColor.R, LightColor.G, LightColor.B);
	Params->LightToUV = FVector4f(float(0.5 / Half), float(-0.5 / Half), 0.5f, 0.5f);
	Params->MapSize = float(Size);
	Params->Bias = float(FMath::Max(double(CVarVGSSplatShadowBias.GetValueOnRenderThread()), 2.0 * Texel));
	Params->PenumbraPerDepth = float(PenumbraRadius * FMath::Max(CVarVGSSplatShadowSoftness.GetValueOnRenderThread(), 0.0f) / (2.0 * Half));
	Params->SelfShadow = CVarVGSSplatShadowSelf.GetValueOnRenderThread() != 0 ? 1u : 0u;
	Params->HasGBuffer = bGBuffer ? 1u : 0u;
	Params->FallbackStrength = 0.6f;
	Params->RenderTargets[0] = FRenderTargetBinding(SceneColor, ERenderTargetLoadAction::ELoad);

	TShaderMapRef<FVGSSplatLightShadowCompositePS> CompositePS(ShaderMap);
	FPixelShaderUtils::AddFullscreenPass(GraphBuilder, ShaderMap, RDG_EVENT_NAME("VGSSplat.LightShadow.Composite"), CompositePS, Params, ViewInfo.ViewRect);
}
