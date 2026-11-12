#include "VGSCaptureSceneProxy.h"

#include "VGSCaptureComponent.h"
#include "VGSSplatSource.h"

#include "SceneInterface.h"
#include "SceneManagement.h"
#include "SceneView.h"

FVGSCaptureSceneProxy::FVGSCaptureSceneProxy(const UVGSCaptureComponent* Component, TSharedPtr<FVGSSplatSource, ESPMode::ThreadSafe> InSource, bool bInCaptureAxesMirrored)
	: FPrimitiveSceneProxy(Component)
	, Source(MoveTemp(InSource))
	, bCaptureAxesMirrored(bInCaptureAxesMirrored)
{
}

SIZE_T FVGSCaptureSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

FPrimitiveViewRelevance FVGSCaptureSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Relevance;
	Relevance.bDrawRelevance = IsShown(View);
	Relevance.bShadowRelevance = IsShadowCast(View);
	Relevance.bDynamicRelevance = true;
	Relevance.bStaticRelevance = false;
	// Shadows only: the splat renderer draws the splats in the view.
	Relevance.bRenderInMainPass = false;
	Relevance.bRenderInDepthPass = false;
	Relevance.bOpaque = true;
	return Relevance;
}

void FVGSCaptureSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	if (!Source.IsValid())
	{
		return;
	}
	const bool bReverseCulling = bCaptureAxesMirrored != IsLocalToWorldDeterminantNegative();
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if (VisibilityMap & (1u << ViewIndex))
		{
			Source->AddShadowMeshes_RenderThread(*this, *Views[ViewIndex], ViewIndex, Collector, bReverseCulling);
		}
	}
}
