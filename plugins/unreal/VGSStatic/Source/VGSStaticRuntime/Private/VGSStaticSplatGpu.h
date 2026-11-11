#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "VGSSplatLayout.h"
#include "VGSStaticSplat.h"

class FRDGBuilder;
class FRHICommandListBase;

/**
 * An asset's splats on the GPU, in the renderer's buffers: made on the game thread (packing
 * the quantized layout there, if asked), uploaded on the render thread the first time a
 * component showing it is drawn, and shared by every one of them after that.
 */
class FVGSStaticSplatGpu
{
public:
	FVGSStaticSplatGpu(TSharedPtr<const FVGSStaticSplatData, ESPMode::ThreadSafe> InData, bool bQuantized, bool bNormalsFaceCamera);

	/** Render thread: uploads on the first call, then does nothing. */
	void Ensure_RenderThread(FRDGBuilder& GraphBuilder);
	void Ensure_RenderThread(FRHICommandListBase& RHICmdList);

	bool IsEmpty() const { return !Data.IsValid() || Data->NumSplats == 0; }

	// Valid after Ensure_RenderThread.
	TRefCountPtr<FRDGPooledBuffer> Splats;
	FShaderResourceViewRHIRef SplatsSRV; // of Splats, for the shadow pass's vertex factory
	TRefCountPtr<FRDGPooledBuffer> Normals; // null when the asset has none
	FShaderResourceViewRHIRef NormalsSRV; // of Normals, for the shadow pass's vertex factory
	TRefCountPtr<FRDGPooledBuffer> Clusters; // null when it has none

	// Valid from construction.
	uint32 NumSplats = 0;
	uint32 NumClusters = 0;
	uint32 Stride = 0;
	int32 SHDegree = 0;
	bool bQuantized = false;
	bool bNormalsFaceCamera = false; // the normals are directions only: the renderer turns them to the camera
	vgsunreal::Quantization Quantization = {};
	FBox3f Bounds = FBox3f(ForceInit);

private:
	TSharedPtr<const FVGSStaticSplatData, ESPMode::ThreadSafe> Data;
	TArray<uint8> QuantizedRecords;
	bool bUploaded = false;
};
