// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#include "VGSSplatViewExtension.h"
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
}

void FVGSSplatViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
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
		const bool bDrawnLit = RenderProxy->bLit && VGSSplat::SupportsLit(View);
		if (bDrawnLit != (LitTargets != nullptr))
		{
			continue;
		}

		FRHITexture* ProxyLUTRHI = nullptr;
		if (RenderProxy->InverseToneCurveStrength > 0.0f)
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
		VGSSplat::RenderLit(GraphBuilder, View, RenderItems, RenderStats, *LitTargets); // VGS CHANGE
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
	if (VGSSplat::IsEnabled() && InViewFamily.Scene)
	{
		PrepareSources_RenderThread.Broadcast(GraphBuilder);
	}
}

// VGS CHANGE: with r.VGSSplat.Shadow.Method 0 or 1, the casting proxies' own shadow map of the
// directional light, taken off the lit scene before post-processing.
void FVGSSplatViewExtension::PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs)
{
	if (!VGSSplat::IsEnabled() || !VGSSplat::UsesSplatShadowMap() || View.Family == nullptr || View.Family->Scene == nullptr)
	{
		return;
	}
	TArray<const FVGSSplatRenderProxy*> Proxies;
	ExternalProxySources_RenderThread.Broadcast(GraphBuilder, View, Proxies);
	VGSSplat::RenderLightShadow(GraphBuilder, View, Inputs, Proxies);
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
