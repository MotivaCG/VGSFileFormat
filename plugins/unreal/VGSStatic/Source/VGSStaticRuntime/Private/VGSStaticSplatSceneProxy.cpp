#include "VGSStaticSplatSceneProxy.h"

#include "VGSStaticSplatComponent.h"
#include "VGSStaticSplatGpu.h"

#include "SceneInterface.h"
#include "SceneManagement.h"
#include "SceneView.h"

FVGSStaticSplatSceneProxy::FVGSStaticSplatSceneProxy(const UVGSStaticSplatComponent* Component, TSharedPtr<FVGSStaticSplatGpu, ESPMode::ThreadSafe> InGpu, const FVGSStaticShadowSettings& InSettings)
	: FPrimitiveSceneProxy(Component)
	, Gpu(MoveTemp(InGpu))
	, Settings(InSettings)
	, VertexFactory(GetScene().GetFeatureLevel())
{
	bFileAxesMirrored = Settings.FileToComponent.Determinant() < 0.0f;
}

FVGSStaticSplatSceneProxy::~FVGSStaticSplatSceneProxy()
{
	VertexFactory.ReleaseResource();
}

SIZE_T FVGSStaticSplatSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FVGSStaticSplatSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	VertexFactory.InitResource(RHICmdList);
	if (!Gpu.IsValid() || Gpu->IsEmpty())
	{
		return;
	}
	Gpu->Ensure_RenderThread(RHICmdList);
	if (!Gpu->SplatsSRV.IsValid())
	{
		return;
	}

	FVGSStaticSplatShadowParameters Parameters;
	FMemory::Memzero(Parameters);
	Parameters.Splats = Gpu->SplatsSRV;
	Parameters.FileToComponent = Settings.FileToComponent;
	Parameters.NumSplats = Gpu->NumSplats;
	Parameters.Stride = Gpu->Stride;
	Parameters.BufferFormat = Gpu->bQuantized ? 1u : 0u;
	if (Gpu->bQuantized)
	{
		const vgsunreal::Quantization& Q = Gpu->Quantization;
		Parameters.PositionScale = FVector3f(Q.positionScale[0], Q.positionScale[1], Q.positionScale[2]);
		Parameters.PositionBias = FVector3f(Q.positionBias[0], Q.positionBias[1], Q.positionBias[2]);
		Parameters.ScaleScale = FVector3f(Q.scaleScale[0], Q.scaleScale[1], Q.scaleScale[2]);
		Parameters.ScaleBias = FVector3f(Q.scaleBias[0], Q.scaleBias[1], Q.scaleBias[2]);
		Parameters.OpacityQuantization = FVector2f(Q.colorScale[3], Q.colorBias[3]);
	}
	Parameters.PointScale = Settings.PointScale;
	Parameters.OpacityScale = Settings.OpacityScale;
	Parameters.OpacityCutoff = FMath::Clamp(Settings.OpacityCutoff, 0.01f, 0.99f);
	Parameters.Normals = Gpu->NormalsSRV;
	Parameters.HasNormals = Gpu->NormalsSRV.IsValid() ? (Gpu->bNormalsFaceCamera ? 2u : 1u) : 0u;
	Parameters.Center = Gpu->Bounds.IsValid ? FVector3f(Gpu->Bounds.GetCenter()) : FVector3f::ZeroVector;
	Parameters.Inset = FMath::Max(Settings.Inset, 0.0f);
	VertexFactory.SetParameters(RHICmdList, Parameters);
}

FPrimitiveViewRelevance FVGSStaticSplatSceneProxy::GetViewRelevance(const FSceneView* View) const
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

void FVGSStaticSplatSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	if (!VertexFactory.HasParameters())
	{
		return;
	}
	const bool bReverseCulling = bFileAxesMirrored != IsLocalToWorldDeterminantNegative();
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if (VisibilityMap & (1u << ViewIndex))
		{
			VertexFactory.AddShadowMeshes(*this, ViewIndex, Collector, bReverseCulling);
		}
	}
}
