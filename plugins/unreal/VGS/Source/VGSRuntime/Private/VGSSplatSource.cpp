#include "VGSSplatSource.h"
#include "VGSStats.h"

#include "RenderGraphBuilder.h"
#include "SceneView.h"

DECLARE_CYCLE_STAT(TEXT("Queue upload (render thread)"), STAT_VGS_Upload, STATGROUP_VGS);
DECLARE_MEMORY_STAT(TEXT("Uploaded per frame"), STAT_VGS_UploadBytes, STATGROUP_VGS);

namespace
{
	/**
	 * Buffers are sized in whole mebibytes: the number of live splats changes from one
	 * instant to the next, and RDG's pool only hands back a buffer of the same size, so
	 * exact sizes would allocate a new one on almost every frame. An upload may be smaller
	 * than its buffer.
	 */
	constexpr uint32 BufferGranularity = 1u << 20;
}

void FVGSSplatSource::SetState_RenderThread(const FVGSRenderState& InState)
{
	check(IsInRenderingThread());
	State = InState;
}

void FVGSSplatSource::SetFrame_RenderThread(FVGSPackedFrameRef Frame)
{
	check(IsInRenderingThread());
	PendingFrame = MoveTemp(Frame);
}

void FVGSSplatSource::Upload_RenderThread(FRDGBuilder& GraphBuilder, const FVGSPackedFrameRef& Frame)
{
	SCOPE_CYCLE_COUNTER(STAT_VGS_Upload);
	SET_MEMORY_STAT(STAT_VGS_UploadBytes, Frame->Bytes.Num());
	if (Frame->NumSplats == 0 || Frame->Bytes.Num() == 0)
	{
		SplatBuffer = nullptr;
		UploadedSplats = 0;
		return;
	}

	const uint32 Bytes = uint32(Frame->Bytes.Num());
	const uint32 Capacity = Align(Bytes, BufferGranularity);
	FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(Capacity), TEXT("VGS.Splats"));

	// The frame stays alive until RDG has copied it: the callback holds a reference.
	GraphBuilder.QueueBufferUpload(Buffer, Frame->Bytes.GetData(), Bytes, [Keep = Frame](const void*) {});

	// Kept beyond this graph, for the views and frames that show the same instant. The
	// renderer registers it again in the same graph, which RDG resolves to this buffer.
	SplatBuffer = GraphBuilder.ConvertToExternalBuffer(Buffer);
	UploadedSplats = Frame->NumSplats;

	Proxy.NumSplats = Frame->NumSplats;
	Proxy.NumLOD0Splats = 0; // no levels of detail: every splat is level 0
	Proxy.SHDegree = Frame->SHDegree;
	Proxy.GpuDataStride = Frame->Stride;
	Proxy.BufferFormat = Frame->bQuantized ? 1u : 0u;

	const vgsunreal::Quantization& Q = Frame->Quantization;
	if (Frame->bQuantized)
	{
		Proxy.PositionScale = FVector3f(Q.positionScale[0], Q.positionScale[1], Q.positionScale[2]);
		Proxy.PositionBias = FVector3f(Q.positionBias[0], Q.positionBias[1], Q.positionBias[2]);
		Proxy.ScaleScale = FVector3f(Q.scaleScale[0], Q.scaleScale[1], Q.scaleScale[2]);
		Proxy.ScaleBias = FVector3f(Q.scaleBias[0], Q.scaleBias[1], Q.scaleBias[2]);
		Proxy.ColorScale = FVector4f(Q.colorScale[0], Q.colorScale[1], Q.colorScale[2], Q.colorScale[3]);
		Proxy.ColorBias = FVector4f(Q.colorBias[0], Q.colorBias[1], Q.colorBias[2], Q.colorBias[3]);
		Proxy.SHScale = FVector3f(Q.shScale[0], Q.shScale[1], Q.shScale[2]);
		Proxy.SHBias = FVector3f(Q.shBias[0], Q.shBias[1], Q.shBias[2]);
	}
	else
	{
		Proxy.PositionScale = Proxy.ScaleScale = Proxy.SHScale = FVector3f::One();
		Proxy.PositionBias = Proxy.ScaleBias = Proxy.SHBias = FVector3f::Zero();
		Proxy.ColorScale = FVector4f::One();
		Proxy.ColorBias = FVector4f::Zero();
	}
}

void FVGSSplatSource::Collect_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, TArray<const FGaussianSplatRenderProxy*>& OutProxies)
{
	// Once per frame of the game, not per view: the first view uploads, the rest reuse it.
	if (PendingFrame.IsValid())
	{
		FVGSPackedFrameRef Frame = MoveTemp(PendingFrame);
		PendingFrame.Reset();
		Upload_RenderThread(GraphBuilder, Frame);
	}

	if (!State.bVisible || !SplatBuffer.IsValid() || UploadedSplats == 0)
	{
		return;
	}

	// The capture keeps its own axes; the turn into Unreal's is in LocalToWorld, so the
	// harmonics are evaluated in the space they were fitted in (VGSSplatLayout.h).
	Proxy.SHCoordinateSpace = EGaussianSplatCoordinateSpace::LH_FRU;
	Proxy.GpuBuffer = SplatBuffer;
	Proxy.ClusterBuffer = nullptr;
	Proxy.NumClusters = 0;
	Proxy.LocalToWorld = State.LocalToWorld;
	Proxy.WorldToLocal = State.WorldToLocal;
	Proxy.LocalBounds = State.LocalBounds;
	Proxy.PointScaleFactor = State.PointScaleFactor;
	Proxy.OpacityScale = State.OpacityScale;
	Proxy.InverseToneCurveStrength = State.InverseToneCurveStrength;
	Proxy.bInvertExpandGamutLUT = State.bInvertExpandGamut;
	Proxy.bVisible = true;
	OutProxies.Add(&Proxy);
}
