#include "VGSStaticSplatSource.h"

#include "VGSStaticSplatGpu.h"

#include "RenderGraphBuilder.h"
#include "SceneView.h"

void FVGSStaticSplatSource::SetLook_RenderThread(const FVGSStaticLook& InLook)
{
	check(IsInRenderingThread());
	Look = InLook;
}

void FVGSStaticSplatSource::SetGpu_RenderThread(TSharedPtr<FVGSStaticSplatGpu, ESPMode::ThreadSafe> InGpu)
{
	check(IsInRenderingThread());
	Gpu = MoveTemp(InGpu);
}

void FVGSStaticSplatSource::Collect_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, TArray<const FVGSStaticSplatRenderProxy*>& OutProxies)
{
	if (!Look.bVisible || !Gpu.IsValid() || Gpu->IsEmpty())
	{
		return;
	}
	Gpu->Ensure_RenderThread(GraphBuilder); // the first component drawn uploads; the rest share it
	if (!Gpu->Splats.IsValid())
	{
		return;
	}

	Proxy.NumSplats = Gpu->NumSplats;
	Proxy.NumLOD0Splats = 0; // one level of detail
	// Harmonics off is a lower degree over the same records: the stride is what steps them.
	Proxy.SHDegree = Look.bSphericalHarmonics ? Gpu->SHDegree : 0;
	Proxy.SHCoordinateSpace = EVGSStaticSplatCoordinateSpace::LH_FRU; // the file's space is the local space
	Proxy.GpuBuffer = Gpu->Splats;
	Proxy.GpuDataStride = Gpu->Stride;
	Proxy.BufferFormat = Gpu->bQuantized ? 1u : 0u;
	if (Gpu->bQuantized)
	{
		const vgsunreal::Quantization& Q = Gpu->Quantization;
		Proxy.PositionScale = FVector3f(Q.positionScale[0], Q.positionScale[1], Q.positionScale[2]);
		Proxy.PositionBias = FVector3f(Q.positionBias[0], Q.positionBias[1], Q.positionBias[2]);
		Proxy.ScaleScale = FVector3f(Q.scaleScale[0], Q.scaleScale[1], Q.scaleScale[2]);
		Proxy.ScaleBias = FVector3f(Q.scaleBias[0], Q.scaleBias[1], Q.scaleBias[2]);
		Proxy.ColorScale = FVector4f(Q.colorScale[0], Q.colorScale[1], Q.colorScale[2], Q.colorScale[3]);
		Proxy.ColorBias = FVector4f(Q.colorBias[0], Q.colorBias[1], Q.colorBias[2], Q.colorBias[3]);
		Proxy.SHScale = FVector3f(Q.shScale[0], Q.shScale[1], Q.shScale[2]);
		Proxy.SHBias = FVector3f(Q.shBias[0], Q.shBias[1], Q.shBias[2]);
	}
	Proxy.ClusterBuffer = Gpu->Clusters;
	Proxy.NumClusters = Gpu->Clusters.IsValid() ? Gpu->NumClusters : 0;
	Proxy.NormalBuffer = Gpu->Normals;
	Proxy.bNormalsFaceCamera = Gpu->bNormalsFaceCamera;

	Proxy.LocalToWorld = Look.LocalToWorld;
	Proxy.WorldToLocal = Look.WorldToLocal;
	Proxy.LocalBounds = Gpu->Bounds;
	Proxy.PointScaleFactor = Look.PointScaleFactor;
	Proxy.OpacityScale = Look.OpacityScale;
	Proxy.InverseToneCurveStrength = Look.InverseToneCurveStrength;
	Proxy.bInvertExpandGamutLUT = Look.bInvertExpandGamut != 0;
	Proxy.Intensity = Look.Intensity;
	Proxy.bBlendInSRGB = Look.bBlendInSRGB != 0;
	Proxy.Grade = Look.Grade;
	Proxy.Tint = Look.Tint;
	Proxy.bLit = Look.bLit != 0;
	Proxy.SelfIllumination = Look.SelfIllumination;
	Proxy.Roughness = Look.Roughness;
	Proxy.Specular = Look.Specular;
	Proxy.NormalSmoothing = Look.NormalSmoothing;
	Proxy.bCastShadow = Look.bCastShadow != 0;
	Proxy.bVisible = true;
	OutProxies.Add(&Proxy);
}
