#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"

class FVGSSplatSource;
class UVGSCaptureComponent;

/**
 * A capture in Unreal's scene: only so that its splats cast shadows. It is in no view's main
 * or depth pass (the splat renderer draws the splats, through the component's
 * FVGSSplatSource); in shadow depth passes it draws the source's current instant as small
 * opaque octahedra, with the source's FVGSSplatShadowVertexFactory, which the source points at
 * every new instant before the frame's shadows are drawn (FVGSSplatSource::Prepare_RenderThread).
 */
class FVGSCaptureSceneProxy final : public FPrimitiveSceneProxy
{
public:
	FVGSCaptureSceneProxy(const UVGSCaptureComponent* Component, TSharedPtr<FVGSSplatSource, ESPMode::ThreadSafe> InSource, bool bInCaptureAxesMirrored);

	virtual SIZE_T GetTypeHash() const override;
	virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;

private:
	TSharedPtr<FVGSSplatSource, ESPMode::ThreadSafe> Source;
	bool bCaptureAxesMirrored = false; // the capture's axes are left handed in the component's
};
