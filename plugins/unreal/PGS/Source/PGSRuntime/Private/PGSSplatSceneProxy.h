#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "PGSSplatShadowVertexFactory.h" // PGSSplatRenderer

class FPGSSplatGpu;
class UPGSSplatComponent;

/** What the shadow casters need of a component, taken on the game thread. */
struct FPGSShadowSettings
{
	FMatrix44f FileToComponent = FMatrix44f::Identity;
	float PointScale = 1.0f;
	float OpacityScale = 1.0f;
	float Inset = 0.0f; // component units
};

/**
 * A static splat component in Unreal's scene: only so that its splats cast shadows. It is in
 * no view's main or depth pass (the splat renderer draws the splats, through the component's
 * FPGSSplatSource); in shadow depth passes it draws them as small opaque octahedra
 * with FPGSSplatShadowVertexFactory, from the asset's GPU copy, which it uploads if
 * no view has yet.
 */
class FPGSSplatSceneProxy final : public FPrimitiveSceneProxy
{
public:
	FPGSSplatSceneProxy(const UPGSSplatComponent* Component, TSharedPtr<FPGSSplatGpu, ESPMode::ThreadSafe> InGpu, const FPGSShadowSettings& InSettings);
	virtual ~FPGSSplatSceneProxy() override;

	virtual SIZE_T GetTypeHash() const override;
	virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }
	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;

private:
	TSharedPtr<FPGSSplatGpu, ESPMode::ThreadSafe> Gpu;
	FPGSShadowSettings Settings;
	FPGSSplatShadowVertexFactory VertexFactory;
	bool bFileAxesMirrored = false; // the file's axes are left handed in the component's
};
