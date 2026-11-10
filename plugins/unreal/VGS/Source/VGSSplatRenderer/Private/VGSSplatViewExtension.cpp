// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#include "VGSSplatViewExtension.h"
#include "GaussianSplatRenderResource.h"
#include "VGSSplatRenderPipeline.h"
#include "VGSSplatTransform.h"

#include "SceneView.h"
#include "SceneInterface.h"
#include "PrimitiveSceneInfo.h"

#include "ScreenPass.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "GaussianSplatComponent.h"
#include "VGSSplatInverseToneCurveLUT.h"
#include "VGSSplatRenderStats.h"
#include "VGSSplatRendererModule.h"
#include "VGSSplatSubsystem.h"
#include "Engine/World.h"
#include "Containers/SortedMap.h"

#define INVALID_ID 0xFFFFFFFF

static TAutoConsoleVariable<int32> CVarVGSSplatNumSplats(
	TEXT("r.VGSSplat.NumSplats"),
	-1,
	TEXT("The maximum number of splats to support (default = -1 is unbounded)"),
	ECVF_RenderThreadSafe);

namespace
{
	static uint32 GetUniqueID(const UGaussianSplatComponent* Component)
	{
		return IsValid(Component->Asset) ? Component->GetUniqueID() : INVALID_ID;
	}
}

FVGSSplatViewExtension::FVGSSplatViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
{
}

FVGSSplatViewExtension::~FVGSSplatViewExtension()
{
	RenderProxy_RenderThread.Reset();
}

FMatrix44f VGSSplat::GetComponentLocalToWorld(const UGaussianSplatComponent& Component)
{
	return FMatrix44f(Component.GetComponentTransform().ToMatrixWithScale());
}

FVGSSplatRenderProxy* CreateRenderProxy(const UGaussianSplatComponent* Component)
{
	FVGSSplatRenderProxy* Proxy = new FVGSSplatRenderProxy();

	Proxy->SplatID = Component->GetUniqueID();
	UGaussianSplat* VGSSplat = Component->Asset;
	TSharedPtr<FGaussianSplatRenderResource> Resource = VGSSplat->GetResource();

	Proxy->SHDegree = Resource->GetSHDegree();
	Proxy->SHCoordinateSpace = VGSSplat->GetSHCoordinateSpace();

	// Extract compression values
	const FSharedStruct& RuntimeData = Resource->GetRuntimeData();
	const FGaussianSplatFormatQuantized* QuantizedFormat = RuntimeData.GetPtr<FGaussianSplatFormatQuantized>();
	Proxy->BufferFormat = QuantizedFormat != nullptr ? 1u : 0u;
	if (QuantizedFormat)
	{
		Proxy->PositionScale = QuantizedFormat->PositionScaleBias[0];
		Proxy->PositionBias  = QuantizedFormat->PositionScaleBias[1];
		Proxy->ScaleScale    = QuantizedFormat->ScaleScaleBias[0];
		Proxy->ScaleBias     = QuantizedFormat->ScaleScaleBias[1];
		Proxy->ColorScale    = QuantizedFormat->ColorScaleBias[0];
		Proxy->ColorBias     = QuantizedFormat->ColorScaleBias[1];
		Proxy->SHScale       = QuantizedFormat->SHScaleBias[0];
		Proxy->SHBias        = QuantizedFormat->SHScaleBias[1];
	}

	Proxy->LocalToWorld = VGSSplat::GetComponentLocalToWorld(*Component);
	Proxy->WorldToLocal = Proxy->LocalToWorld.Inverse();

	Proxy->PointScaleFactor = Component->PointScaleFactor;
	Proxy->OpacityScale     = Component->OpacityScale;

	Proxy->InverseToneCurveStrength = Component->bInvertToneCurve ? Component->InverseToneCurveStrength : 0.0f;
	Proxy->bInvertExpandGamutLUT = Component->bInvertExpandGamut;

	return Proxy;
}

void FVGSSplatViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
	ENQUEUE_RENDER_COMMAND(FUpdateVGSSplatStatsRegistration)([](FRHICommandList&)
	{
		FModuleManager::GetModuleChecked<FVGSSplatRendererModule>("VGSSplatRenderer").GetRenderStats_RenderThread().UpdateRegistration();
	});

	// Update render-thread transform data
	for (const UGaussianSplatComponent* Component : RegisteredComponents)
	{
		const uint32 UniqueID = GetUniqueID(Component);
		if (UniqueID != INVALID_ID)
		{
			FMatrix44f LocalToWorld = VGSSplat::GetComponentLocalToWorld(*Component);
			FMatrix44f WorldToLocal = FMatrix44f(Component->GetComponentTransform().ToMatrixWithScale().Inverse());
			bool bVisible = Component->ShouldRender();
			const float PointScaleFactor = Component->PointScaleFactor;
			const float OpacityScale = Component->OpacityScale;
			float InverseToneCurveStrength = Component->bInvertToneCurve ? Component->InverseToneCurveStrength : 0.0f;
			bool bInvertExpandGamutLUT = Component->bInvertExpandGamut;

			ENQUEUE_RENDER_COMMAND(FCreateVGSSplatRenderProxy)(
				[
					WeakThis = AsWeak(),
					UniqueID,
					LocalToWorld,
					WorldToLocal,
					bVisible,
					PointScaleFactor,
					OpacityScale,
					InverseToneCurveStrength,
					bInvertExpandGamutLUT
				](FRHICommandList& RHICmdList)
				{
					TSharedPtr<FVGSSplatViewExtension> This = StaticCastWeakPtr<FVGSSplatViewExtension>(WeakThis).Pin();
					if (This.IsValid())
					{
						if (TUniquePtr<FVGSSplatRenderProxy>* Proxy = This->RenderProxy_RenderThread.Find(UniqueID))
						{
							(*Proxy)->LocalToWorld = LocalToWorld;
							(*Proxy)->WorldToLocal = WorldToLocal;
							(*Proxy)->bVisible = bVisible;
							(*Proxy)->PointScaleFactor = PointScaleFactor;
							(*Proxy)->OpacityScale = OpacityScale;
							(*Proxy)->InverseToneCurveStrength = InverseToneCurveStrength;
							(*Proxy)->bInvertExpandGamutLUT = bInvertExpandGamutLUT;
						}
					}
				}
			);
		}
	}
}

void FVGSSplatViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	//if (PassId == EPostProcessingPass::VisualizeDepthOfField)
	if (PassId == EPostProcessingPass::BeforeDOF)
	{
		if (GaussianSplat::IsEnabled() && View.Family && View.Family->Scene)
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

void FVGSSplatViewExtension::Render(FRDGBuilder& GraphBuilder, const FSceneView& View, const FScreenPassRenderTarget& Output, FRDGTexture* SceneDepthTexture, FRDGTexture* SceneVelocityTexture)
{
	FVGSSplatRendererModule& Module = FModuleManager::GetModuleChecked<FVGSSplatRendererModule>("VGSSplatRenderer");

	// Registered components first, then whatever external sources build into this frame's graph.
	TArray<const FVGSSplatRenderProxy*> Proxies;
	Proxies.Reserve(RenderProxy_RenderThread.Num());
	for (auto Itr = RenderProxy_RenderThread.CreateIterator(); Itr; ++Itr)
	{
		Proxies.Add(Itr.Value().Get());
	}
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
	VGSSplat::Render(GraphBuilder, View, RenderItems, RenderStats, Output, SceneDepthTexture, SceneVelocityTexture);
}

void FVGSSplatViewExtension::RegisterComponent(const UGaussianSplatComponent* Component)
{
	if (!Component->HasValidSplatData() || RegisteredComponents.Contains(Component))
	{
		return;
	}
	RegisteredComponents.Add(Component);

	// Create render proxy on render thread
	const uint32 UniqueID = GetUniqueID(Component);
	if (UniqueID != INVALID_ID)
	{
		TUniquePtr<FVGSSplatRenderProxy> RenderProxy(CreateRenderProxy(Component));
		TSharedPtr<FGaussianSplatRenderResource> Resource = Component->Asset->GetResource();
		const int32 NumSplatsCVar = CVarVGSSplatNumSplats.GetValueOnGameThread();

		ENQUEUE_RENDER_COMMAND(FAddVGSSplatRenderProxy)(
			[
				WeakThis = AsWeak(),
				UniqueID,
				RenderProxy = MoveTemp(RenderProxy),
				Resource,
				NumSplatsCVar
			](FRHICommandList& RHICmdList) mutable
			{
				RenderProxy->NumSplats = uint32(Resource->GetNumSplats());
				RenderProxy->NumLOD0Splats = uint32(Resource->GetNumLOD0Splats());
				RenderProxy->LocalBounds = Resource->GetBounds();

				bool bDebugDisableClustering = false;
				if (NumSplatsCVar > 0 && uint32(NumSplatsCVar) < RenderProxy->NumSplats)
				{
					RenderProxy->NumSplats = uint32(NumSplatsCVar);
					bDebugDisableClustering = true;
				}

				RenderProxy->GpuBuffer = Resource->GetSplatBuffer();
				RenderProxy->GpuDataStride = Resource->GetSplatStride();
				if (!bDebugDisableClustering)
				{
					RenderProxy->ClusterBuffer = Resource->GetClusterBuffer();
					RenderProxy->NumClusters = Resource->GetNumClusters();
				}

				TSharedPtr<FVGSSplatViewExtension> This = StaticCastWeakPtr<FVGSSplatViewExtension>(WeakThis).Pin();
				if (This.IsValid())
				{
					This->RenderProxy_RenderThread.Add(UniqueID, MoveTemp(RenderProxy));
				}
			}
		);
	}
}

void FVGSSplatViewExtension::UnregisterComponent(const UGaussianSplatComponent* Component)
{
	RegisteredComponents.Remove(Component);

	// Remove render proxy on render thread
	const uint32 UniqueID = Component->GetUniqueID(); // bypass asset check
	ENQUEUE_RENDER_COMMAND(FRemoveVGSSplatRenderProxy)(
		[
			WeakThis = AsWeak(),
			UniqueID
		](FRHICommandList& RHICmdList)
		{
			TSharedPtr<FVGSSplatViewExtension> This = StaticCastWeakPtr<FVGSSplatViewExtension>(WeakThis).Pin();
			if (This.IsValid())
			{
				This->RenderProxy_RenderThread.Remove(UniqueID); // auto-deletes via TUniquePtr
			}
		}
	);
}

void FVGSSplatViewExtension::AddExternalProxySource(FOnCollectVGSSplatProxies::FDelegate&& InSource)
{
	ENQUEUE_RENDER_COMMAND(FAddVGSSplatExternalProxySource)(
		[WeakThis = AsWeak(), Source = MoveTemp(InSource)](FRHICommandList&) mutable
		{
			if (TSharedPtr<FVGSSplatViewExtension> This = StaticCastWeakPtr<FVGSSplatViewExtension>(WeakThis).Pin())
			{
				This->ExternalProxySources_RenderThread.Add(MoveTemp(Source));
			}
		}
	);
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
