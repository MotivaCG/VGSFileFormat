// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#include "VGSSplatViewExtension.h"
#include "RendererInterface.h"
#include "EngineModule.h"
#include "RenderGraphUtils.h"
// VGS CHANGE: no GaussianSplatRenderResource.h, VGSSplatTransform.h or GaussianSplatComponent.h:
// the copy draws only the sources registered with it, never Epic's components, and does not
// need Epic's GaussianSplat plugin (only Unreal 5.9 has it).
#include "VGSSplatRenderPipeline.h"

#include "SceneView.h"
#include "SceneInterface.h"
#include "PrimitiveSceneInfo.h"

#include "ScreenPass.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "VGSSplatInverseToneCurveLUT.h"
#include "VGSSplatRenderStats.h"
#include "VGSSplatRendererModule.h"
#include "VGSSplatSubsystem.h"
#include "Engine/World.h"
#include "Containers/SortedMap.h"
#include "SceneRendering.h" // VGS CHANGE: FViewInfo, for the lit pass
#include "SceneTextures.h" // VGS CHANGE
#include "Substrate/Substrate.h" // VGS CHANGE: its material buffer, for the lit pass
#include "VGSSplatLightShadow.h" // VGS CHANGE
#include "VGSSplatShadowVertexFactory.h" // VGS CHANGE: VGSSplat::GetShadowMethod
#if WITH_DEV_AUTOMATION_TESTS
#include "RHIGPUReadback.h"
#endif

#define INVALID_ID 0xFFFFFFFF

// VGS CHANGE: the switch Epic's GaussianSplat::IsEnabled reads (r.GaussianSplat), the copy's own.
static TAutoConsoleVariable<int32> CVarVGSSplatEnable(
	TEXT("r.VGSSplat"),
	1,
	TEXT("Draw splats (0 to hide them all)."),
	ECVF_RenderThreadSafe);

namespace VGSSplat
{
	static bool IsEnabled()
	{
		return CVarVGSSplatEnable.GetValueOnAnyThread() != 0;
	}
}

FVGSSplatViewExtension::FVGSSplatViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
{
}

FVGSSplatViewExtension::~FVGSSplatViewExtension()
{
 if (PostOpaqueHandle.IsValid() && FModuleManager::Get().IsModuleLoaded("Renderer"))
  GetRendererModule().RemovePostOpaqueRenderDelegate(PostOpaqueHandle);
}

void FVGSSplatViewExtension::BlendAfterOpaque(FPostOpaqueRenderParameters& Parameters)
{
 if (!Parameters.View || !Parameters.GraphBuilder || !Parameters.ColorTexture || !Parameters.DepthTexture) return;
 FMixFrame Frame;
 if (MixFrames.RemoveAndCopyValue(Parameters.View, Frame))
 {
  if (Frame.bExternalDepth) Parameters.GraphBuilder->UseInternalAccessMode(Frame.Depth);
  VGSSplat::BlendSelfIllumination(*Parameters.GraphBuilder, *Parameters.View, Parameters.ColorTexture, Parameters.DepthTexture, Frame.Color, Frame.Depth);
  if (Frame.bExternalDepth) Parameters.GraphBuilder->UseExternalAccessMode(Frame.Depth, ERHIAccess::SRVMask);
 }
}

void FVGSSplatViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
 if (!PostOpaqueHandle.IsValid())
  PostOpaqueHandle = GetRendererModule().RegisterPostOpaqueRenderDelegate(FPostOpaqueRenderDelegate::CreateSP(StaticCastSharedRef<FVGSSplatViewExtension>(AsShared()), &FVGSSplatViewExtension::BlendAfterOpaque));
	VGSSplat::FDeepShadowFrame Frame;
	if (UVGSSplatSubsystem* Subsystem = GetWorld()->GetSubsystem<UVGSSplatSubsystem>())
	{
		Frame = Subsystem->UpdateShadowLightFunction(InViewFamily);
	}
	ENQUEUE_RENDER_COMMAND(FUpdateVGSDeepShadowFrame)([WeakThis = AsWeak(), Frame](FRHICommandList&)
	{
		if (TSharedPtr<FVGSSplatViewExtension> This = StaticCastWeakPtr<FVGSSplatViewExtension>(WeakThis).Pin()) This->DeepShadowFrame = Frame;
	});
	ENQUEUE_RENDER_COMMAND(FUpdateVGSSplatStatsRegistration)([](FRHICommandList&)
	{
		FModuleManager::GetModuleChecked<FVGSSplatRendererModule>("VGSSplatRenderer").GetRenderStats_RenderThread().UpdateRegistration();
	});
	// VGS CHANGE: no registered components to update; sources set their proxies themselves.
}

void FVGSSplatViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	//if (PassId == EPostProcessingPass::VisualizeDepthOfField)
	if (PassId == EPostProcessingPass::BeforeDOF)
	{
		if (VGSSplat::IsEnabled() && View.Family && View.Family->Scene)
		{
			InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FVGSSplatViewExtension::PostProcessPass_RenderThread));
		}
	}
}

bool FVGSSplatViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	if (!FWorldSceneViewExtension::IsActiveThisFrame_Internal(Context))
	{
		return false;
	}

	return true;
}

namespace
{
	void GetSceneDepthAndVelocityTextures(
		const FPostProcessMaterialInputs& Inputs,
		FRDGTextureRef& OutSceneDepthTexture,
		FRDGTextureRef& OutSceneVelocityTexture)
	{
		OutSceneDepthTexture = nullptr;
		OutSceneVelocityTexture = nullptr;

		if (Inputs.SceneTextures.SceneTextures)
		{
			const FSceneTextureUniformParameters* Contents = Inputs.SceneTextures.SceneTextures->GetContents();
			OutSceneDepthTexture = Contents->SceneDepthTexture;
			OutSceneVelocityTexture = Contents->GBufferVelocityTexture;
		}
		else if (Inputs.SceneTextures.MobileSceneTextures)
		{
			const FMobileSceneTextureUniformParameters* Contents = Inputs.SceneTextures.MobileSceneTextures->GetContents();
			OutSceneDepthTexture = Contents->SceneDepthTexture;
			OutSceneVelocityTexture = Contents->SceneVelocityTexture;
		}
	}
}

FScreenPassTexture FVGSSplatViewExtension::PostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& InOutInputs)
{
	FScreenPassTextureSlice SceneColorSlice = InOutInputs.GetInput(EPostProcessMaterialInput::SceneColor);
	check(SceneColorSlice.IsValid());

	FScreenPassTexture SceneColor(SceneColorSlice);

	FScreenPassRenderTarget Output = InOutInputs.OverrideOutput;

	if (VGSSplat::IsHardwareRasterization(View.GetShaderPlatform()))
	{
		if (!Output.IsValid())
		{
			Output = FScreenPassRenderTarget(SceneColor.Texture, SceneColor.ViewRect, View.GetOverwriteLoadAction());
		}
		else
		{
			AddDrawTexturePass(GraphBuilder, View, SceneColorSlice, Output);
		}

		FRDGTextureRef SceneDepthTexture = nullptr;
		FRDGTextureRef SceneVelocityTexture = nullptr;
		GetSceneDepthAndVelocityTextures(InOutInputs, SceneDepthTexture, SceneVelocityTexture);
		Render(GraphBuilder, View, Output, SceneDepthTexture, SceneVelocityTexture);
	}
	else
	{
		FRDGTextureDesc ColorDesc = FRDGTextureDesc::Create2D(
			SceneColor.Texture->Desc.Extent,
			SceneColor.Texture->Desc.Format,
			//FClearValueBinding::Black,
			FClearValueBinding::Transparent,
			TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV);
		FRDGTextureRef ColorTexture = GraphBuilder.CreateTexture(ColorDesc, TEXT("VGSSplat.Color"));
		// Copy SceneColor into the intermediate so splats composite on top of the scene.
		AddCopyTexturePass(GraphBuilder, SceneColor.Texture, ColorTexture);

		FScreenPassRenderTarget ColorRT(ColorTexture, SceneColor.ViewRect, ERenderTargetLoadAction::ELoad);

		FRDGTextureRef SceneDepthTexture = nullptr;
		FRDGTextureRef SceneVelocityTexture = nullptr;
		GetSceneDepthAndVelocityTextures(InOutInputs, SceneDepthTexture, SceneVelocityTexture);
		Render(GraphBuilder, View, ColorRT, SceneDepthTexture, SceneVelocityTexture);

		// Write the composited result to the engine's expected output.
		if (Output.IsValid())
		{
			AddDrawTexturePass(GraphBuilder, View, FScreenPassTextureSlice::CreateFromScreenPassTexture(GraphBuilder, FScreenPassTexture(ColorTexture, SceneColor.ViewRect)), Output);
		}
		else
		{
			Output = ColorRT;
		}
	}

	return MoveTemp(Output);
}

// VGS CHANGE: lit proxies, into the GBuffer before Unreal lights it.
void FVGSSplatViewExtension::PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView, const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	if (!VGSSplat::IsEnabled() || InView.Family == nullptr || InView.Family->Scene == nullptr || !VGSSplat::SupportsLit(InView))
	{
		return;
	}
	// The scene textures uniform buffer has no GBuffer yet at this point; the textures are
	// the view family's.
	const FSceneTextures& Textures = static_cast<const FViewInfo&>(InView).GetSceneTextures();
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(InView);
	VGSSplat::FLitTargets Targets;
	Targets.SceneColor = Textures.Color.Target;
	Targets.SceneDepth = Textures.Depth.Target;
	Targets.SceneVelocity = Textures.Velocity;
	Targets.CaptureReceiverMask = VGSSplat::RegisterReceiverMask(GraphBuilder, DeepShadowFrame);
	Targets.ViewRect = ViewInfo.ViewRect;
	if (Substrate::IsSubstrateEnabled() && !Substrate::IsSubstrateBlendableGBufferEnabled(InView.GetShaderPlatform()))
	{
		// Substrate's material buffer, as the engine's debug probes write it
		// (DebugProbeRendering.cpp).
		const FSubstrateSceneData* Substrate = ViewInfo.SubstrateViewData.SceneData;
		if (Substrate == nullptr || !Substrate->MaterialTextureArray || !Substrate->MaterialTextureArrayUAVWithoutRTs || !Substrate->TopLayerTexture)
		{
			return;
		}
		Targets.bSubstrateMaterial = true;
		Targets.SubstrateMaterial = Substrate->MaterialTextureArray;
		Targets.SubstrateMaterialUAV = Substrate->MaterialTextureArrayUAVWithoutRTs;
		Targets.SubstrateTopLayer = Substrate->TopLayerTexture;
		Targets.SubstrateMaxBytesPerPixel = Substrate->EffectiveMaxBytesPerPixel;
		Targets.bSubstrateRoughDiffuse = Substrate->bRoughDiffuse;
	}
	else
	{
		Targets.GBufferA = Textures.GBufferA;
		Targets.GBufferB = Textures.GBufferB;
		Targets.GBufferC = Textures.GBufferC;
		if (!Targets.GBufferA || !Targets.GBufferB || !Targets.GBufferC)
		{
			return;
		}
	}
	if (!Targets.SceneColor || !Targets.SceneDepth)
	{
		return;
	}
	Render(GraphBuilder, InView, FScreenPassRenderTarget(Targets.SceneColor, Targets.ViewRect, ERenderTargetLoadAction::ELoad), Targets.SceneDepth, nullptr, &Targets);
	if (Targets.CaptureReceiverMask) GraphBuilder.UseExternalAccessMode(Targets.CaptureReceiverMask, ERHIAccess::SRVMask);
}

void FVGSSplatViewExtension::Render(FRDGBuilder& GraphBuilder, const FSceneView& View, const FScreenPassRenderTarget& Output, FRDGTexture* SceneDepthTexture, FRDGTexture* SceneVelocityTexture, const VGSSplat::FLitTargets* LitTargets)
{
	FVGSSplatRendererModule& Module = FModuleManager::GetModuleChecked<FVGSSplatRendererModule>("VGSSplatRenderer");

	// VGS CHANGE: only external sources; the copy has no registered components.
	TArray<const FVGSSplatRenderProxy*> Proxies;
	ExternalProxySources_RenderThread.Broadcast(GraphBuilder, View, Proxies);

	TSortedMap<VGSSplat::EInverseToneCurveLUTVariant, FRHITexture*> LUTRHI;
	TArray<FVGSSplatRenderItem> RenderItems;
	RenderItems.Reserve(Proxies.Num());
	for (const FVGSSplatRenderProxy* RenderProxy : Proxies)
	{
		if (!RenderProxy->bVisible || !RenderProxy->GpuBuffer.IsValid())
		{
			continue;
		}
		// VGS CHANGE: a lit proxy is drawn in one pass or the other, never both; where lit
		// drawing is not supported it stays in the post-process pass, self-illuminated.
		// Endpoint 1 takes the existing unlit path: no normals, lit raster or GBuffer lighting.
  const bool bDrawnLit = RenderProxy->bLit && RenderProxy->SelfIllumination < 1.0f && VGSSplat::SupportsLit(View);
		if (bDrawnLit != (LitTargets != nullptr))
		{
			continue;
		}

		// The inverse tone curve goes on whatever shows the capture's own colour: a capture drawn
		// self-illuminated (Self-illumination, or Shaded at an amount of 1 or unsupported) and the
		// self-illuminated part of a Shaded one, so that both ends of the amount match.
		FRHITexture* ProxyLUTRHI = nullptr;
		if ((!bDrawnLit || RenderProxy->SelfIllumination > 0.0f) && RenderProxy->InverseToneCurveStrength > 0.0f)
		{
			const VGSSplat::EInverseToneCurveLUTVariant Variant = static_cast<VGSSplat::EInverseToneCurveLUTVariant>(RenderProxy->bInvertExpandGamutLUT);
			FRHITexture*& CachedLUTRHI = LUTRHI.FindOrAdd(Variant);
			if (CachedLUTRHI == nullptr)
			{
				CachedLUTRHI = Module.GetInverseToneCurveLUTRHI_RenderThread(RenderProxy->bInvertExpandGamutLUT);
			}
			ProxyLUTRHI = CachedLUTRHI;
		}

		RenderItems.Add({ RenderProxy, ProxyLUTRHI });
	}

	// Sort by bounding box centroid
	const FVector ViewOrigin = View.ViewMatrices.GetViewOrigin();
	const FVector ViewDirection = View.GetViewDirection();
	auto GetSortDistance = [&ViewOrigin, &ViewDirection](const FVGSSplatRenderProxy& Proxy)
	{
		// Proxies without bounds are drawn last
		if (!Proxy.LocalBounds.IsValid)
		{
			return TNumericLimits<double>::Lowest();
		}

		const FVector Center = FVector(FVector3f(Proxy.LocalToWorld.TransformPosition(Proxy.LocalBounds.GetCenter())));
		const FVector Distance = Center - ViewOrigin;
		const double SortDistance = FVector::DotProduct(Distance, ViewDirection);

		// A NaN distance is treated similarly to a proxy without bounds
		return FMath::IsFinite(SortDistance) ? SortDistance : TNumericLimits<double>::Lowest();
	};
	RenderItems.StableSort([&GetSortDistance](const FVGSSplatRenderItem& A, const FVGSSplatRenderItem& B)
	{
		return GetSortDistance(*A.Proxy) > GetSortDistance(*B.Proxy);
	});

	VGSSplat::FRenderStats& RenderStats = Module.GetRenderStats_RenderThread();
	if (LitTargets != nullptr)
	{
		if (RenderItems.IsEmpty()) return;
  VGSSplat::FLitTargets Targets = *LitTargets;
  if (!Targets.CaptureReceiverMask)
  {
   Targets.CaptureReceiverMask = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Targets.SceneColor->Desc.Extent,
    PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_RenderTargetable), TEXT("VGSSplat.ReceiverDepth"));
   AddClearRenderTargetPass(GraphBuilder, Targets.CaptureReceiverMask, FLinearColor::Black);
  }
  const bool bNeedsMix = RenderItems.ContainsByPredicate([](const FVGSSplatRenderItem& Item) { return Item.Proxy->SelfIllumination > 0.0f; });
  if (bNeedsMix)
  {
   Targets.MixReference = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Targets.SceneColor->Desc.Extent,
    PF_FloatRGBA, FClearValueBinding::Transparent, TexCreate_ShaderResource | TexCreate_RenderTargetable), TEXT("VGSSplat.MixReference"));
   AddClearRenderTargetPass(GraphBuilder, Targets.MixReference, FLinearColor::Transparent);
  }
  VGSSplat::RenderLit(GraphBuilder, View, RenderItems, RenderStats, Targets);
  if (bNeedsMix)
  {
   // The existing receiver mask stores the winning device depth. Reuse it without a depth copy.
   MixFrames.Add(&View, {Targets.MixReference, Targets.CaptureReceiverMask, LitTargets->CaptureReceiverMask != nullptr});
  }
		return;
	}
	VGSSplat::Render(GraphBuilder, View, RenderItems, RenderStats, Output, SceneDepthTexture, SceneVelocityTexture);
}

// VGS CHANGE: no RegisterComponent / UnregisterComponent (Epic's components are Epic's to draw).

void FVGSSplatViewExtension::AddExternalProxySource(FOnCollectVGSSplatProxies::FDelegate&& InSource, FOnPrepareVGSSplatSources::FDelegate&& InPrepare)
{
	ENQUEUE_RENDER_COMMAND(FAddVGSSplatExternalProxySource)(
		[WeakThis = AsWeak(), Source = MoveTemp(InSource), Prepare = MoveTemp(InPrepare)](FRHICommandList&) mutable
		{
			if (TSharedPtr<FVGSSplatViewExtension> This = StaticCastWeakPtr<FVGSSplatViewExtension>(WeakThis).Pin())
			{
				This->ExternalProxySources_RenderThread.Add(MoveTemp(Source));
				if (Prepare.IsBound()) // VGS CHANGE
				{
					This->PrepareSources_RenderThread.Add(MoveTemp(Prepare));
				}
			}
		}
	);
}

// VGS CHANGE: before anything of the family is drawn, shadow depths included.
void FVGSSplatViewExtension::PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	MixFrames.Reset();
	if (VGSSplat::IsEnabled() && InViewFamily.Scene)
	{
		PrepareSources_RenderThread.Broadcast(GraphBuilder);
		if (DeepShadowFrame.IsValid() && !InViewFamily.Views.IsEmpty())
		{
			TArray<const FVGSSplatRenderProxy*> Proxies;
			ExternalProxySources_RenderThread.Broadcast(GraphBuilder, *InViewFamily.Views[0], Proxies);
			VGSSplat::RenderDeepShadow(GraphBuilder, DeepShadowFrame, Proxies);
		}
	}
}

FVGSSplatViewExtension* VGSSplat::FindViewExtension(UWorld* World)
{
	UVGSSplatSubsystem* Subsystem = UWorld::GetSubsystem<UVGSSplatSubsystem>(World);
	return (IsValid(Subsystem) && Subsystem->SceneViewExtension.IsValid()) ? Subsystem->SceneViewExtension.Get() : nullptr;
}

void VGSSplat::RegisterExternalProxySource(UWorld* World, FOnCollectVGSSplatProxies::FDelegate&& Source)
{
	if (FVGSSplatViewExtension* Extension = FindViewExtension(World))
	{
		Extension->AddExternalProxySource(MoveTemp(Source));
	}
}

// VGS CHANGE
void VGSSplat::RegisterExternalProxySource(UWorld* World, FOnCollectVGSSplatProxies::FDelegate&& Source, FOnPrepareVGSSplatSources::FDelegate&& Prepare)
{
	if (FVGSSplatViewExtension* Extension = FindViewExtension(World))
	{
		Extension->AddExternalProxySource(MoveTemp(Source), MoveTemp(Prepare));
	}
}
