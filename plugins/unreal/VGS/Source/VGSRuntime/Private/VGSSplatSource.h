#pragma once

#include "CoreMinimal.h"
#include "GaussianSplatRenderPipeline.h" // FGaussianSplatRenderProxy (GaussianSplatSceneViewExtension, Internal)
#include "RenderGraphResources.h"
#include "VGSSplatLayout.h"

class FRDGBuilder;
class FSceneView;

/** One decoded instant, already in the renderer's layout. Built on the game thread, read on the render thread. */
struct FVGSPackedFrame
{
	TArray<uint8> Bytes;
	uint32 NumSplats = 0;
	uint32 Stride = 0;
	int32 SHDegree = 0;
	bool bQuantized = false;
	vgsunreal::Quantization Quantization = {};
	double Seconds = 0.0;
};

using FVGSPackedFrameRef = TSharedPtr<const FVGSPackedFrame, ESPMode::ThreadSafe>;

namespace VGS { class FPackedChunk; }
using FVGSPackedChunkRef = TSharedPtr<const VGS::FPackedChunk, ESPMode::ThreadSafe>;

/**
 * One instant of a packed capture, for VGSPacked.usf to evaluate on the GPU: its chunk,
 * uploaded once, and what changes per frame - where the instant falls in the chunk, the
 * basis at it, and the component's intensity and density.
 */
struct FVGSGpuFrame
{
	FVGSPackedChunkRef Chunk;
	uint32 Frame = 0;
	float Alpha = 0.0f;
	float Normalized = 0.0f;
	TArray<FVector4f> Basis;
	float Gain = 1.0f;
	float DcOffset = 0.0f;
	bool bThinned = false;
	uint32 DensityThreshold = MAX_uint32;
	float DensityExponent = 1.0f;
	double Seconds = 0.0;
};

using FVGSGpuFrameRef = TSharedPtr<const FVGSGpuFrame, ESPMode::ThreadSafe>;

/** What the component says about itself every tick: where it is and how to draw. */
struct FVGSRenderState
{
	FMatrix44f LocalToWorld = FMatrix44f::Identity; // capture axes and metres to world
	FMatrix44f WorldToLocal = FMatrix44f::Identity;
	FBox3f LocalBounds = FBox3f(EForceInit::ForceInit); // in the capture's space
	bool bVisible = true;
	float PointScaleFactor = 1.0f;
	float OpacityScale = 1.0f;
	float InverseToneCurveStrength = 0.0f;
	bool bInvertExpandGamut = true;
};

/**
 * A capture as the Gaussian Splat renderer sees it: a source of splats with no
 * UGaussianSplatComponent behind it, which is what GaussianSplat::RegisterExternalProxySource
 * is for (Niagara's splat renderer is the other user, NiagaraRendererSplats.cpp).
 *
 * Render thread only, apart from construction. The renderer calls Collect once per view,
 * inside its post-process pass; the first call of a frame uploads a new instant if the game
 * thread sent one, and every call appends the one proxy this object owns. The binding is
 * made with CreateSP and expires with this object, so the component releases its
 * reference on the render thread and nothing has to unregister.
 */
class FVGSSplatSource : public TSharedFromThis<FVGSSplatSource, ESPMode::ThreadSafe>
{
public:
	void SetState_RenderThread(const FVGSRenderState& State);
	void SetFrame_RenderThread(FVGSPackedFrameRef Frame);
	void SetGpuFrame_RenderThread(FVGSGpuFrameRef Frame);

	void Collect_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, TArray<const FGaussianSplatRenderProxy*>& OutProxies);

private:
	void Upload_RenderThread(FRDGBuilder& GraphBuilder, const FVGSPackedFrameRef& Frame);
	void Evaluate_RenderThread(FRDGBuilder& GraphBuilder, const FVGSGpuFrameRef& Frame);
	void UploadChunk_RenderThread(FRDGBuilder& GraphBuilder, const FVGSPackedChunkRef& Chunk);
	void SetLayout(uint32 NumSplats, int32 SHDegree, uint32 Stride, bool bQuantized, const vgsunreal::Quantization* Quantization);

	FGaussianSplatRenderProxy Proxy;
	FVGSRenderState State;

	/** Sent by the game thread and not uploaded yet: one or the other. */
	FVGSPackedFrameRef PendingFrame;
	FVGSGpuFrameRef PendingGpuFrame;

	/** The packed chunk on the GPU, as VGSPacked.usf reads it; replaced when the serial changes. */
	struct FChunkBuffers
	{
		uint64 Serial = 0;
		TRefCountPtr<FRDGPooledBuffer> SplatA, SplatB, SplatC, ShStatic, ShTemporal, PositionTerms,
			RotationTerms, PositionSamples, RotationSamples, Books, Luts;
	};
	FChunkBuffers ChunkBuffers;

	/** The instant on the GPU, kept for the frames of the game where the capture does not move on. */
	TRefCountPtr<FRDGPooledBuffer> SplatBuffer;
	uint32 UploadedSplats = 0;
};
