#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "SGSSplatShadowVertexFactory.h" // SGSSplatRenderer

class FSGSSplatGpu;
class USGSSplatComponent;

/** What the shadow casters need of a component, taken on the game thread. */
struct FSGSShadowSettings
{
	FMatrix44f FileToComponent = FMatrix44f::Identity;
	float PointScale = 1.0f;
	float OpacityScale = 1.0f;
	float Inset = 0.0f; // component units
};

/**
 * A static splat component in Unreal's scene: only so that its splats cast shadows. It is in
 * no view's main or depth pass (the splat renderer draws the splats, through the component's
 * FSGSSplatSource); in shadow depth passes it draws them as small opaque octahedra
 * with FSGSSplatShadowVertexFactory, from the asset's GPU copy, which it uploads if
 * no view has yet.
 */
class FSGSSplatSceneProxy final : public FPrimitiveSceneProxy
{
public:
	FSGSSplatSceneProxy(const USGSSplatComponent* Component, TSharedPtr<FSGSSplatGpu, ESPMode::ThreadSafe> InGpu, const FSGSShadowSettings& InSettings);
	virtual ~FSGSSplatSceneProxy() override;

	virtual SIZE_T GetTypeHash() const override;
	virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }
	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;

private:
	TSharedPtr<FSGSSplatGpu, ESPMode::ThreadSafe> Gpu;
	FSGSShadowSettings Settings;
	FSGSSplatShadowVertexFactory VertexFactory;
	bool bFileAxesMirrored = false; // the file's axes are left handed in the component's
};
