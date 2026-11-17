// VGS: optional development readback of the real scene velocity before TSR.
#include "VGSSplatViewExtension.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "GlobalShader.h"
#include "PostProcess/PostProcessInputs.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "SceneRendering.h"
#include "SceneTextures.h"

static TAutoConsoleVariable<int32> CVarVGSSplatMotionReadback(TEXT("r.VGSSplat.Debug.MotionReadback"), 0,
	TEXT("Development: change to a new positive integer to log one GPU readback of capture-pixel motion before TSR. Requires the own shadow map's receiver mask."), ECVF_RenderThreadSafe);

class FVGSSplatMotionDebugCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVGSSplatMotionDebugCS);
	SHADER_USE_PARAMETER_STRUCT(FVGSSplatMotionDebugCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, VelocityTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, ReceiverMask)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, MotionStats)
		SHADER_PARAMETER(FIntPoint, ViewMin)
		SHADER_PARAMETER(FIntPoint, ViewSize)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FVGSSplatMotionDebugCS, "/Plugin/VGS/Private/SplatRenderer/VGSSplatMotionDebug.usf", "MainCS", SF_Compute);

void FVGSSplatViewExtension::PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs)
{
	if (MotionReadback && MotionReadback->IsReady())
	{
		const uint32* Data = static_cast<const uint32*>(MotionReadback->Lock(4 * sizeof(uint32)));
		UE_LOG(LogTemp, Display, TEXT("%s motion readback: request=%d covered=%u moving=%u max=%.3fpx clear=%u"),
			VGSSPLAT_PLUGIN_NAME, LastMotionReadbackRequest, Data[0], Data[1], double(Data[2]) / 1000.0, Data[3]);
		MotionReadback->Unlock(); MotionReadback.Reset();
	}
	const int32 Request = CVarVGSSplatMotionReadback.GetValueOnRenderThread();
	if (Request == 0) LastMotionReadbackRequest = 0;
	if (Request <= 0 || Request == LastMotionReadbackRequest || MotionReadback || !DeepShadowFrame.IsValid() || !View.bIsViewInfo) return;
	const FSceneTextures& Textures = static_cast<const FViewInfo&>(View).GetSceneTextures();
	if (!Textures.Velocity || !HasBeenProduced(Textures.Velocity)) return;
	FRDGTextureRef Mask = VGSSplat::RegisterReceiverMask(GraphBuilder, DeepShadowFrame);
	FRDGBufferRef Stats = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 4), TEXT("VGSSplat.MotionStats"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Stats), 0u);
	auto* P = GraphBuilder.AllocParameters<FVGSSplatMotionDebugCS::FParameters>();
	P->VelocityTexture = Textures.Velocity; P->ReceiverMask = Mask; P->MotionStats = GraphBuilder.CreateUAV(Stats);
	P->ViewMin = static_cast<const FViewInfo&>(View).ViewRect.Min; P->ViewSize = static_cast<const FViewInfo&>(View).ViewRect.Size();
	TShaderMapRef<FVGSSplatMotionDebugCS> CS(GetGlobalShaderMap(View.GetShaderPlatform()));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VGSSplat.MotionReadback"), CS, P, FComputeShaderUtils::GetGroupCount(P->ViewSize, FIntPoint(8)));
	MotionReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VGSSplat.MotionReadback"));
	AddEnqueueCopyPass(GraphBuilder, MotionReadback.Get(), Stats, 4 * sizeof(uint32));
	GraphBuilder.UseExternalAccessMode(Mask, ERHIAccess::SRVMask);
	LastMotionReadbackRequest = Request;
}
#endif
