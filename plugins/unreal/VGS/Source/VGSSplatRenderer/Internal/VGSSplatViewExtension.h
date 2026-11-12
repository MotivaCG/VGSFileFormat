// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "Containers/SortedMap.h"
#include "Containers/SortedSet.h"

class FVGSSplatRenderProxy;
class FVGSSplatViewExtension;
struct FScreenPassRenderTarget;
namespace VGSSplat { struct FLitTargets; } // VGS CHANGE

#define UE_API VGSSPLATRENDERER_API

/**
 * Source of splats with no UGaussianSplatComponent behind them, e.g. a Niagara emitter converting its particles.
 * Broadcast on the render thread once per rendered view from inside the splat post process pass, after the frame's
 * simulation work has been submitted, so a source can add passes to the graph before appending the proxies to draw.
 *
 * Appended proxies are read until the splat pass for the view has been set up, on the same call stack, so sources
 * should own them persistently. Bind with CreateSP: the binding expires with the shared object and that is the only
 * deregistration, so everything the bound function reads must be destroyed on the render thread or be kept alive
 * by that object.
 */
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnCollectVGSSplatProxies, FRDGBuilder&, const FSceneView&, TArray<const FVGSSplatRenderProxy*>&);

/**
 * VGS CHANGE: broadcast on the render thread at the start of every view family's rendering
 * (PreRenderViewFamily_RenderThread), before shadow depths are drawn, so that a source whose
 * splats change can write them where its shadow casters read them. Bound and expiring as
 * FOnCollectVGSSplatProxies.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnPrepareVGSSplatSources, FRDGBuilder&);

namespace VGSSplat
{
	/** Binds a source to the splat renderer of World. Callable from the game thread or a game thread task; no-op for worlds without one. */
	UE_API void RegisterExternalProxySource(UWorld* World, FOnCollectVGSSplatProxies::FDelegate&& Source);

	/** VGS CHANGE: as above, with what the source does before each view family is drawn. */
	UE_API void RegisterExternalProxySource(UWorld* World, FOnCollectVGSSplatProxies::FDelegate&& Source, FOnPrepareVGSSplatSources::FDelegate&& Prepare);

	/** The splat renderer of World, or null if it has none. Not exported; callers in other modules use the entry points above. */
	FVGSSplatViewExtension* FindViewExtension(UWorld* World);
}

class FVGSSplatViewExtension : public FWorldSceneViewExtension
{
public:
	FVGSSplatViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld);
	~FVGSSplatViewExtension();

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;
	// VGS CHANGE: lit proxies go into the GBuffer here, after the base pass and before lighting.
	virtual void PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView, const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;
	// VGS CHANGE: sources prepare their splats here, before shadow depths.
	virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
	// VGS CHANGE: the splats' own shadow of the directional light, onto the lit scene (VGSSplatLightShadow.h).
	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs) override;

	// VGS CHANGE: no RegisterComponent / UnregisterComponent; only external sources are drawn.
	void AddExternalProxySource(FOnCollectVGSSplatProxies::FDelegate&& InSource, FOnPrepareVGSSplatSources::FDelegate&& InPrepare = FOnPrepareVGSSplatSources::FDelegate());

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

	FScreenPassTexture PostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& InOutInputs);
	// VGS CHANGE: with LitTargets, only the proxies drawn lit, into the GBuffer; without, the rest.
	void Render(FRDGBuilder& GraphBuilder, const FSceneView& View, const FScreenPassRenderTarget& Output, FRDGTexture* SceneDepthTexture, FRDGTexture* SceneVelocityTexture, const VGSSplat::FLitTargets* LitTargets = nullptr);

private:
	FOnCollectVGSSplatProxies ExternalProxySources_RenderThread;
	FOnPrepareVGSSplatSources PrepareSources_RenderThread; // VGS CHANGE
};

#undef UE_API
