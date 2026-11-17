// VGS: depth-resolved splat shadows, consumed by a Light Function before lighting.
#include "VGSSplatLightShadow.h"
#include "VGSSplatRenderPipeline.h"
#include "CommonRenderResources.h"
#include "GlobalShader.h"
#include "LightSceneInfo.h"
#include "LightSceneProxy.h"
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"
#include "ScenePrivate.h"
#include "TextureResource.h"
#include "DataDrivenShaderPlatformInfo.h"

static TAutoConsoleVariable<float> CVarVGSSplatShadowTexelSize(
	TEXT("r.VGSSplat.Shadow.TexelSize"),
	0.5f,
	TEXT("With r.VGSSplat.Shadow.Method 0 or 1: the size (cm) of a texel of the splats' own shadow map, which covers all the casting splats\n")
	TEXT("(up to r.VGSSplat.Shadow.MaxMapSize texels a side; coarser beyond)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatShadowMaxMapSize(
	TEXT("r.VGSSplat.Shadow.MaxMapSize"),
	1024,
	TEXT("With r.VGSSplat.Shadow.Method 0 or 1: spatial resolution limit (64..1024) for the 64-layer shadow grid."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVGSSplatShadowSelf(
	TEXT("r.VGSSplat.Shadow.Self"),
	0,
	TEXT("With r.VGSSplat.Shadow.Method 0 or 1: 1 enables shadows on Shaded captures, including self-shadow; 0 excludes this renderer's visible capture pixels."),
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

BEGIN_SHADER_PARAMETER_STRUCT(FVGSDeepShadowMapParameters, )
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OpticalDepth)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, NearestDepth)
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
	SHADER_PARAMETER(FVector3f, GridExtent)
	SHADER_PARAMETER(float, GridResolution)
	SHADER_PARAMETER(float, Dilation)
	SHADER_PARAMETER(float, OpticalDepthScale)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

class FVGSSplatDeepShadowVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVGSSplatDeepShadowVS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatDeepShadowVS, FGlobalShader);
	using FParameters = FVGSDeepShadowMapParameters;
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& E)
	{ FGlobalShader::ModifyCompilationEnvironment(P, E); E.SetDefine(TEXT("VGS_DEEP_SHADOW_MAP"), 1); }
};
class FVGSSplatDeepShadowPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVGSSplatDeepShadowPS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatDeepShadowPS, FGlobalShader);
	using FParameters = FVGSDeepShadowMapParameters;
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& E)
	{ FGlobalShader::ModifyCompilationEnvironment(P, E); E.SetDefine(TEXT("VGS_DEEP_SHADOW_MAP"), 1); }
};
IMPLEMENT_GLOBAL_SHADER(FVGSSplatDeepShadowVS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatLightShadow.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FVGSSplatDeepShadowPS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatLightShadow.usf", "MainPS", SF_Pixel);

class FVGSSplatDeepShadowPrefixCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVGSSplatDeepShadowPrefixCS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatDeepShadowPrefixCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, OpticalDepth)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NearestDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, ShadowAtlas)
		SHADER_PARAMETER(uint32, Resolution)
		SHADER_PARAMETER(float, InverseOpticalDepthScale)
	END_SHADER_PARAMETER_STRUCT()
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& E)
	{ FGlobalShader::ModifyCompilationEnvironment(P, E); E.SetDefine(TEXT("VGS_DEEP_SHADOW_PREFIX"), 1); }
};
IMPLEMENT_GLOBAL_SHADER(FVGSSplatDeepShadowPrefixCS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatLightShadow.usf", "PrefixCS", SF_Compute);

FRDGTextureRef VGSSplat::RegisterReceiverMask(FRDGBuilder& GraphBuilder, const FDeepShadowFrame& Frame)
{
	if (!Frame.IsValid()) return nullptr;
	FRDGTextureRef Texture = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Frame.Receivers->GetRenderTargetTexture(), TEXT("VGSSplat.CaptureReceiverMask")));
	GraphBuilder.UseInternalAccessMode(Texture);
	return Texture;
}

void VGSSplat::RenderDeepShadow(FRDGBuilder& GraphBuilder, const FDeepShadowFrame& Frame, TConstArrayView<const FVGSSplatRenderProxy*> Proxies)
{
	if (!Frame.IsValid()) return;
	RDG_EVENT_SCOPE(GraphBuilder, "VGSSplat.DeepShadow");
	FRDGTextureRef Receivers = RegisterReceiverMask(GraphBuilder, Frame);
	AddClearRenderTargetPass(GraphBuilder, Receivers, FLinearColor::Black);
	GraphBuilder.UseExternalAccessMode(Receivers, ERHIAccess::SRVMask);
	FRDGTextureRef Atlas = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Frame.Atlas->GetRenderTargetTexture(), TEXT("VGSSplat.DeepShadowAtlas")));
	GraphBuilder.UseInternalAccessMode(Atlas);
	FRDGBufferRef OpticalDepth = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Frame.Resolution * Frame.Resolution * 64), TEXT("VGSSplat.OpticalDepthBins"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OpticalDepth), 0u);
	FRDGBufferRef NearestDepth = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Frame.Resolution * Frame.Resolution), TEXT("VGSSplat.NearestLightDepth"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(NearestDepth), 0x7f800000u);
	FRDGTextureRef Dummy = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(Frame.Resolution), PF_R8,
		FClearValueBinding::Black, TexCreate_RenderTargetable), TEXT("VGSSplat.ShadowRasterAttachment"));
	const FMatrix WorldToLight = FTranslationMatrix(-Frame.Origin) * FMatrix(
		FPlane(Frame.X.X, Frame.Y.X, Frame.Z.X, 0), FPlane(Frame.X.Y, Frame.Y.Y, Frame.Z.Y, 0),
		FPlane(Frame.X.Z, Frame.Y.Z, Frame.Z.Z, 0), FPlane(0, 0, 0, 1));
	TShaderMapRef<FVGSSplatDeepShadowVS> VS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FVGSSplatDeepShadowPS> PS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	uint64 TotalSplats = 0;
	for (const FVGSSplatRenderProxy* Proxy : Proxies)
		if (Proxy && Proxy->bVisible && Proxy->bCastShadow && Proxy->GpuBuffer.IsValid()) TotalSplats += Proxy->NumSplats;
	// Each splat contributes at most -log(1-0.995) < 6 once per pixel. This scale
	// bounds the complete integer sum, avoiding overflow without a pixel-shader spin loop.
	const float OpticalDepthScale = float(FMath::Min<uint64>(65536, FMath::Max<uint64>(1, (MAX_uint32 / FMath::Max<uint64>(1, TotalSplats) - 1) / 6)));
	for (const FVGSSplatRenderProxy* Proxy : Proxies)
	{
		if (!Proxy || !Proxy->bVisible || !Proxy->bCastShadow || !Proxy->GpuBuffer.IsValid() || Proxy->NumSplats == 0) continue;
		auto* P = GraphBuilder.AllocParameters<FVGSDeepShadowMapParameters>();
		P->OpticalDepth = GraphBuilder.CreateUAV(OpticalDepth);
		P->NearestDepth = GraphBuilder.CreateUAV(NearestDepth);
		P->Splats = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(Proxy->GpuBuffer));
		P->LocalToLight = FMatrix44f(FMatrix(Proxy->LocalToWorld) * WorldToLight);
		P->PositionScale = Proxy->PositionScale; P->PositionBias = Proxy->PositionBias;
		P->ScaleScale = Proxy->ScaleScale; P->ScaleBias = Proxy->ScaleBias;
		P->NumSplats = Proxy->NumSplats; P->Stride = Proxy->GpuDataStride; P->BufferFormat = Proxy->BufferFormat;
		P->PointScale = Proxy->PointScaleFactor; P->OpacityScale = Proxy->OpacityScale;
		P->OpacityQuantization = FVector2f(Proxy->ColorScale.W, Proxy->ColorBias.W);
		P->GridExtent = FVector3f(Frame.Extent); P->GridResolution = float(Frame.Resolution);
		const double Texel = 2.0 * FMath::Max(Frame.Extent.X, Frame.Extent.Y) / Frame.Resolution;
		P->Dilation = float(0.25 * Texel * Texel);
		P->OpticalDepthScale = OpticalDepthScale;
		P->RenderTargets[0] = FRenderTargetBinding(Dummy, ERenderTargetLoadAction::ENoAction);
		GraphBuilder.AddPass(RDG_EVENT_NAME("VGSSplat.DeepShadow.Bins %u", Proxy->NumSplats), P, ERDGPassFlags::Raster,
			[P, VS, PS, Resolution = Frame.Resolution](FRHICommandList& RHICmdList)
		{
			FGraphicsPipelineStateInitializer State;
			RHICmdList.ApplyCachedRenderTargets(State);
			State.BlendState = TStaticBlendState<CW_NONE>::GetRHI();
			State.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
			State.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
			State.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
			State.BoundShaderState.VertexShaderRHI = VS.GetVertexShader(); State.BoundShaderState.PixelShaderRHI = PS.GetPixelShader();
			State.PrimitiveType = PT_TriangleList;
			SetGraphicsPipelineState(RHICmdList, State, 0);
			SetShaderParameters(RHICmdList, VS, VS.GetVertexShader(), *P);
			SetShaderParameters(RHICmdList, PS, PS.GetPixelShader(), *P);
			RHICmdList.SetViewport(0, 0, 0, float(Resolution), float(Resolution), 1);
			RHICmdList.DrawPrimitive(0, P->NumSplats * 2, 1);
		});
	}
	auto* P = GraphBuilder.AllocParameters<FVGSSplatDeepShadowPrefixCS::FParameters>();
	P->OpticalDepth = GraphBuilder.CreateSRV(OpticalDepth); P->ShadowAtlas = GraphBuilder.CreateUAV(Atlas); P->Resolution = Frame.Resolution;
	P->InverseOpticalDepthScale = 1.0f / OpticalDepthScale;
	P->NearestDepth = GraphBuilder.CreateSRV(NearestDepth);
	TShaderMapRef<FVGSSplatDeepShadowPrefixCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VGSSplat.DeepShadow.Prefix"), ERDGPassFlags::Compute | ERDGPassFlags::NeverCull, CS, P,
		FComputeShaderUtils::GetGroupCount(FIntPoint(Frame.Resolution), FIntPoint(8)));
	GraphBuilder.UseExternalAccessMode(Atlas, ERHIAccess::SRVMask);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RHIGPUReadback.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVGSSplatDeepShadowDepthTest, VGSSPLAT_PLUGIN_NAME TEXT(".Lighting.DeepShadowDepth"), EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVGSSplatDeepShadowDepthTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UTextureRenderTarget2D> Atlas(NewObject<UTextureRenderTarget2D>());
	TStrongObjectPtr<UTextureRenderTarget2D> Receivers(NewObject<UTextureRenderTarget2D>());
	Atlas->bCanCreateUAV = true;
	Atlas->InitCustomFormat(64, 72, PF_R16F, true); Atlas->UpdateResourceImmediate(true);
	Receivers->InitCustomFormat(8, 8, PF_R32_FLOAT, true); Receivers->UpdateResourceImmediate(true);
	VGSSplat::FDeepShadowFrame Frame;
	Frame.Atlas = Atlas->GameThread_GetRenderTargetResource(); Frame.Receivers = Receivers->GameThread_GetRenderTargetResource();
	Frame.Resolution = 8; Frame.Extent = FVector(1); Frame.Origin = FVector::ZeroVector;
	Frame.X = FVector(1, 0, 0); Frame.Y = FVector(0, 1, 0); Frame.Z = FVector(0, 0, 1);
	TArray<float> Values;
	ENQUEUE_RENDER_COMMAND(FVGSDepthShadowRegression)([Frame, &Values](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		TArray<float> Records;
		for (int32 I = 0; I < 21; ++I)
		{
			// 20 faint front splats, a receiver in the middle, and a strong back splat.
			const float Record[14] = {0, 0, I < 20 ? -0.75f : 0.75f, 0.2f, 0.2f, 0.0001f, 0, 0, 0, 1, 0, 0, 0, I < 20 ? 0.05f : 0.2f};
			Records.Append(Record, 14);
		}
		FVGSSplatRenderProxy Proxy;
		Proxy.NumSplats = 21; Proxy.GpuDataStride = 56; Proxy.bCastShadow = true;
		FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(Records.Num() * sizeof(float)), TEXT("VGS.Test.Splats"));
		GraphBuilder.QueueBufferUpload(Buffer, Records.GetData(), Records.Num() * sizeof(float));
		Proxy.GpuBuffer = GraphBuilder.ConvertToExternalBuffer(Buffer);
		const FVGSSplatRenderProxy* Proxies[] = {&Proxy};
		VGSSplat::RenderDeepShadow(GraphBuilder, Frame, Proxies);
		FRDGTextureRef Texture = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Frame.Atlas->GetRenderTargetTexture(), TEXT("VGS.Test.Atlas")));
		GraphBuilder.UseInternalAccessMode(Texture);
		FRHIGPUTextureReadback Readback(TEXT("VGS.Test.DeepShadow"));
		AddEnqueueCopyPass(GraphBuilder, &Readback, Texture);
		GraphBuilder.Execute();
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		if (!Readback.IsReady()) return;
		int32 Pitch = 0;
		const FFloat16* Pixels = static_cast<const FFloat16*>(Readback.Lock(Pitch));
		for (int32 Layer : {0, 31, 63}) Values.Add(Pixels[((Layer / 8) * 8 + 4) * Pitch + (Layer % 8) * 8 + 4].GetFloat());
		Values.Add(Pixels[68 * Pitch + 4].GetFloat());
		Readback.Unlock();
	});
	FlushRenderingCommands();
	if (!TestEqual(TEXT("GPU samples available"), Values.Num(), 4)) return false;
	const double Falloff = FMath::Exp(-0.5 * (0.125 * 0.125 * 2) / (0.2 * 0.2 + 0.015625));
	const float Front = float(FMath::Pow(1.0 - 0.05 * Falloff, 20));
	TestTrue(TEXT("Nothing before the first caster"), FMath::IsNearlyEqual(Values[0], 1.0f, 0.001f));
	TestTrue(TEXT("Faint front layers shadow a receiver before the strong back layer"), FMath::IsNearlyEqual(Values[1], Front, 0.002f));
	TestTrue(TEXT("Back layer only affects receivers behind it"), FMath::IsNearlyEqual(Values[2], Front * float(1.0 - 0.2 * Falloff), 0.002f));
	TestTrue(TEXT("Nearest depth preserves the front surface"), FMath::IsNearlyEqual(Values[3], 0.125f, 0.001f));
	AddInfo(FString::Printf(TEXT("Depth transmittance before/middle/after: %.6f %.6f %.6f (middle reference %.6f)"), Values[0], Values[1], Values[2], Front));
	return true;
}
#endif
