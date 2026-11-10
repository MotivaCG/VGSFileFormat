// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "Containers/SortedMap.h"
#include "Containers/SortedSet.h"

class UGaussianSplatComponent;
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

namespace VGSSplat
{
	/** Binds a source to the splat renderer of World. Callable from the game thread or a game thread task; no-op for worlds without one. */
	UE_API void RegisterExternalProxySource(UWorld* World, FOnCollectVGSSplatProxies::FDelegate&& Source);

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

	void RegisterComponent(const UGaussianSplatComponent* InComponent);
	void UnregisterComponent(const UGaussianSplatComponent* InComponent);
	void AddExternalProxySource(FOnCollectVGSSplatProxies::FDelegate&& InSource);

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

	FScreenPassTexture PostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& InOutInputs);
	// VGS CHANGE: with LitTargets, only the proxies drawn lit, into the GBuffer; without, the rest.
	void Render(FRDGBuilder& GraphBuilder, const FSceneView& View, const FScreenPassRenderTarget& Output, FRDGTexture* SceneDepthTexture, FRDGTexture* SceneVelocityTexture, const VGSSplat::FLitTargets* LitTargets = nullptr);

private:
	TArray<const UGaussianSplatComponent*> RegisteredComponents;
	TSortedSet<uint32> Components_GameThread;
	TSortedMap<uint32, TUniquePtr<FVGSSplatRenderProxy>> RenderProxy_RenderThread;
	FOnCollectVGSSplatProxies ExternalProxySources_RenderThread;
};

#undef UE_API
