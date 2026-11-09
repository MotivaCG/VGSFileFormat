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

	void Collect_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, TArray<const FGaussianSplatRenderProxy*>& OutProxies);

private:
	void Upload_RenderThread(FRDGBuilder& GraphBuilder, const FVGSPackedFrameRef& Frame);

	FGaussianSplatRenderProxy Proxy;
	FVGSRenderState State;

	/** Sent by the game thread and not uploaded yet. */
	FVGSPackedFrameRef PendingFrame;

	/** The instant on the GPU, kept for the frames of the game where the capture does not move on. */
	TRefCountPtr<FRDGPooledBuffer> SplatBuffer;
	uint32 UploadedSplats = 0;
};
