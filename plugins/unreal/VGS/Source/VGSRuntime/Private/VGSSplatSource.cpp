#include "VGSSplatSource.h"
#include "VGSPackedShader.h"
#include "VGSPlayer.h"
#include "VGSStats.h"

#include "GlobalShader.h"
#include "PrimitiveSceneProxy.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"

DECLARE_CYCLE_STAT(TEXT("Queue upload (render thread)"), STAT_VGS_Upload, STATGROUP_VGS);
DECLARE_MEMORY_STAT(TEXT("Uploaded per frame"), STAT_VGS_UploadBytes, STATGROUP_VGS);
DECLARE_CYCLE_STAT(TEXT("Evaluate packed (render thread)"), STAT_VGS_Evaluate, STATGROUP_VGS);
DECLARE_DWORD_COUNTER_STAT(TEXT("Packed chunks uploaded"), STAT_VGS_ChunkUploads, STATGROUP_VGS);

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
	PendingGpuFrame.Reset(); // the latest of either kind wins
}

void FVGSSplatSource::SetGpuFrame_RenderThread(FVGSGpuFrameRef Frame)
{
	check(IsInRenderingThread());
	PendingGpuFrame = MoveTemp(Frame);
	PendingFrame.Reset();
}

void FVGSSplatSource::SetLayout(uint32 NumSplats, int32 SHDegree, uint32 Stride, bool bQuantized, const vgsunreal::Quantization* Quantization)
{
	Proxy.NumSplats = NumSplats;
	Proxy.NumLOD0Splats = 0; // no levels of detail: every splat is level 0
	Proxy.SHDegree = SHDegree;
	Proxy.GpuDataStride = Stride;
	Proxy.BufferFormat = bQuantized ? 1u : 0u;
	if (bQuantized && Quantization)
	{
		const vgsunreal::Quantization& Q = *Quantization;
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

void FVGSSplatSource::UploadChunk_RenderThread(FRDGBuilder& GraphBuilder, const FVGSPackedChunkRef& ChunkRef)
{
	const vgsb_packed_chunk& C = ChunkRef->Get();
	INC_DWORD_STAT(STAT_VGS_ChunkUploads);

	// Straight from the bridge's arrays, which the chunk reference keeps alive until RDG has
	// copied them. Sizes change from chunk to chunk, and once a chunk the pool's churn is
	// nothing.
	const auto Upload = [&GraphBuilder, &ChunkRef](const TCHAR* Name, uint32 Stride, const void* Data, uint64 Words, uint32 WordsPerElement)
	{
		const uint32 Elements = uint32(FMath::Max<uint64>(Words / WordsPerElement, 1));
		FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(Stride, Elements), Name);
		if (Data && Words >= WordsPerElement)
		{
			GraphBuilder.QueueBufferUpload(Buffer, Data, uint64(Elements) * Stride, [Keep = ChunkRef](const void*) {});
		}
		else
		{
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Buffer), 0u);
		}
		return GraphBuilder.ConvertToExternalBuffer(Buffer);
	};
	const uint64 Splats = C.splats;
	ChunkBuffers.SplatA = Upload(TEXT("VGS.SplatA"), 16, C.splat_a, Splats * 4, 4);
	ChunkBuffers.SplatB = Upload(TEXT("VGS.SplatB"), 16, C.splat_b, Splats * 4, 4);
	ChunkBuffers.SplatC = Upload(TEXT("VGS.SplatC"), 16, C.splat_c, Splats * 4, 4);
	ChunkBuffers.ShStatic = Upload(TEXT("VGS.ShStatic"), 4, C.sh_static, C.sh_words, 1);
	ChunkBuffers.ShTemporal = Upload(TEXT("VGS.ShTemporal"), 4, C.sh_temporal, C.sh_words, 1);
	ChunkBuffers.PositionTerms = Upload(TEXT("VGS.PositionTerms"), 8, C.position_terms, C.position_term_words, 2);
	ChunkBuffers.RotationTerms = Upload(TEXT("VGS.RotationTerms"), 4, C.rotation_terms, C.rotation_term_words, 1);
	ChunkBuffers.PositionSamples = Upload(TEXT("VGS.PositionSamples"), 8, C.position_samples, C.position_sample_words, 2);
	ChunkBuffers.RotationSamples = Upload(TEXT("VGS.RotationSamples"), 4, C.rotation_samples, C.rotation_sample_words, 1);
	ChunkBuffers.Books = Upload(TEXT("VGS.Books"), 16, C.books, C.book_floats, 4);
	ChunkBuffers.Luts = Upload(TEXT("VGS.Luts"), 8, C.luts, C.lut_floats, 2);
	ChunkBuffers.bHasNormals = C.normals != nullptr && C.normal_words >= Splats;
	ChunkBuffers.Normals = Upload(TEXT("VGS.Normals"), 4, ChunkBuffers.bHasNormals ? C.normals : nullptr, ChunkBuffers.bHasNormals ? C.normal_words : 0, 1);
	ChunkBuffers.Serial = C.serial;
}

void FVGSSplatSource::Evaluate_RenderThread(FRDGBuilder& GraphBuilder, const FVGSGpuFrameRef& Frame)
{
	SCOPE_CYCLE_COUNTER(STAT_VGS_Evaluate);
	const vgsb_packed_chunk& C = Frame->Chunk->Get();
	const uint32 NumSplats = uint32(C.splats);
	if (NumSplats == 0)
	{
		SplatBuffer = nullptr;
		UploadedSplats = 0;
		return;
	}
	if (ChunkBuffers.Serial != C.serial)
	{
		UploadChunk_RenderThread(GraphBuilder, Frame->Chunk);
	}

	const int32 Degree = FMath::Max(vgsunreal::shDegree(C.sh_coefficients), 0);
	const int32 Coefficients = Degree > 0 ? C.sh_coefficients : 0;
	const uint32 Stride = vgsunreal::floatStride(Coefficients);

	// The records, written where they are drawn from: reused from frame to frame while
	// chunks fit, and sized in whole mebibytes as the uploads are.
	const uint64 Bytes = uint64(NumSplats) * Stride;
	if (!SplatBuffer.IsValid() || SplatBuffer->GetSize() < Bytes)
	{
		const uint32 Capacity = uint32(Align(Bytes, uint64(BufferGranularity)));
		SplatBuffer = GraphBuilder.ConvertToExternalBuffer(
			GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(Capacity), TEXT("VGS.Splats")));
	}
	FRDGBufferRef Output = GraphBuilder.RegisterExternalBuffer(SplatBuffer);
	GraphBuilder.UseInternalAccessMode(Output); // left readable for the shadows by an earlier Prepare of this graph
	FRDGBufferRef Basis = CreateStructuredBuffer(GraphBuilder, TEXT("VGS.Basis"), sizeof(FVector4f), uint32(FMath::Max(Frame->Basis.Num(), 1)),
		Frame->Basis.GetData(), uint32(Frame->Basis.Num()) * sizeof(FVector4f));

	FVGSPackedEvaluateCS::FParameters* P = GraphBuilder.AllocParameters<FVGSPackedEvaluateCS::FParameters>();
	const auto Srv = [&GraphBuilder](const TRefCountPtr<FRDGPooledBuffer>& Buffer)
	{
		return GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(Buffer));
	};
	P->SplatA = Srv(ChunkBuffers.SplatA);
	P->SplatB = Srv(ChunkBuffers.SplatB);
	P->SplatC = Srv(ChunkBuffers.SplatC);
	P->ShStatic = Srv(ChunkBuffers.ShStatic);
	P->ShTemporal = Srv(ChunkBuffers.ShTemporal);
	P->PositionTerms = Srv(ChunkBuffers.PositionTerms);
	P->RotationTerms = Srv(ChunkBuffers.RotationTerms);
	P->PositionSamples = Srv(ChunkBuffers.PositionSamples);
	P->RotationSamples = Srv(ChunkBuffers.RotationSamples);
	P->Books = Srv(ChunkBuffers.Books);
	P->Luts = Srv(ChunkBuffers.Luts);
	P->Basis = GraphBuilder.CreateSRV(Basis);
	P->Splats = NumSplats;
	P->Samples = uint32(C.samples);
	P->Frame = Frame->Frame;
	P->GroupCount = uint32(C.group_count);
	P->ShPlanes = uint32(C.sh_planes);
	P->ShCoefficients = uint32(Coefficients);
	P->EntriesStatic = uint32(C.entries_static);
	P->EntriesTemporal = uint32(C.entries_temporal);
	P->Sh0Stride = uint32(C.sh0_stride);
	P->OpacityStride = uint32(C.opacity_stride);
	P->BooksTemporal = uint32(C.sh_planes) * 3u * uint32(C.entries_static);
	P->BasisOffset = FIntVector4(C.basis_offset[0], C.basis_offset[1], C.basis_offset[2], C.basis_offset[3]);
	P->Alpha = Frame->Alpha;
	P->Normalized = Frame->Normalized;
	for (int32 G = 0; G < 8; ++G)
	{
		P->GroupA[G] = FVector4f(C.group_a[G * 4], C.group_a[G * 4 + 1], C.group_a[G * 4 + 2], C.group_a[G * 4 + 3]);
		P->GroupB[G] = FVector4f(C.group_b[G * 4], C.group_b[G * 4 + 1], C.group_b[G * 4 + 2], C.group_b[G * 4 + 3]);
	}
	P->Gain = Frame->Gain;
	P->DcOffset = Frame->DcOffset;
	P->Thinned = Frame->bThinned ? 1u : 0u;
	P->DensityThreshold = Frame->DensityThreshold;
	P->DensityExponent = Frame->DensityExponent;
	P->SplatStride = Stride;
	P->RWSplatBuffer = GraphBuilder.CreateUAV(Output);

	// Normals, when the capture has them: one word per record, beside the records.
	P->Normals = Srv(ChunkBuffers.Normals);
	P->HasNormals = ChunkBuffers.bHasNormals ? 1u : 0u;
	if (ChunkBuffers.bHasNormals)
	{
		const uint64 NormalBytes = uint64(NumSplats) * sizeof(uint32);
		if (!NormalBuffer.IsValid() || NormalBuffer->GetSize() < NormalBytes)
		{
			const uint32 Capacity = uint32(Align(NormalBytes, uint64(BufferGranularity)));
			NormalBuffer = GraphBuilder.ConvertToExternalBuffer(
				GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Capacity / sizeof(uint32)), TEXT("VGS.SplatNormals")));
		}
		FRDGBufferRef Normals = GraphBuilder.RegisterExternalBuffer(NormalBuffer);
		GraphBuilder.UseInternalAccessMode(Normals);
		P->RWNormalBuffer = GraphBuilder.CreateUAV(Normals);
	}
	else
	{
		NormalBuffer = nullptr;
		P->RWNormalBuffer = GraphBuilder.CreateUAV(GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("VGS.SplatNormals.Dummy")));
	}

	TShaderMapRef<FVGSPackedEvaluateCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VGS.EvaluatePacked %u", NumSplats), Shader, P,
		FComputeShaderUtils::GetGroupCountWrapped(NumSplats, FVGSPackedEvaluateCS::ThreadGroupSize));

	UploadedSplats = NumSplats;
	SetLayout(NumSplats, Degree, Stride, false, nullptr);
}

void FVGSSplatSource::Upload_RenderThread(FRDGBuilder& GraphBuilder, const FVGSPackedFrameRef& Frame)
{
	SCOPE_CYCLE_COUNTER(STAT_VGS_Upload);
	SET_MEMORY_STAT(STAT_VGS_UploadBytes, Frame->Bytes.Num());
	if (Frame->NumSplats == 0 || Frame->Bytes.Num() == 0)
	{
		SplatBuffer = nullptr;
		NormalBuffer = nullptr;
		UploadedSplats = 0;
		ChunkBuffers = FChunkBuffers();
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
	// A CPU frame after packed ones: the chunk on the GPU is of no more use. Evaluated on
	// the CPU, a capture is lit by its splats' shortest axes; its normals are not uploaded.
	ChunkBuffers = FChunkBuffers();
	NormalBuffer = nullptr;
	SetLayout(Frame->NumSplats, Frame->SHDegree, Frame->Stride, Frame->bQuantized, &Frame->Quantization);
}

FVGSSplatSource::~FVGSSplatSource()
{
	if (ShadowVertexFactory.IsValid())
	{
		ShadowVertexFactory->ReleaseResource();
	}
}

bool FVGSSplatSource::Advance_RenderThread(FRDGBuilder& GraphBuilder)
{
	// Once per frame of the game, not per view family or view: the first uploads, the rest
	// reuse it.
	bool bAdvanced = false;
	if (PendingFrame.IsValid())
	{
		FVGSPackedFrameRef Frame = MoveTemp(PendingFrame);
		PendingFrame.Reset();
		Upload_RenderThread(GraphBuilder, Frame);
		bAdvanced = true;
	}
	if (PendingGpuFrame.IsValid())
	{
		FVGSGpuFrameRef Frame = MoveTemp(PendingGpuFrame);
		PendingGpuFrame.Reset();
		Evaluate_RenderThread(GraphBuilder, Frame);
		bAdvanced = true;
	}
	return bAdvanced;
}

void FVGSSplatSource::Prepare_RenderThread(FRDGBuilder& GraphBuilder)
{
	Advance_RenderThread(GraphBuilder);
	UpdateShadows_RenderThread(GraphBuilder);
}

void FVGSSplatSource::UpdateShadows_RenderThread(FRDGBuilder& GraphBuilder)
{
	if (!State.bVisible || !SplatBuffer.IsValid() || UploadedSplats == 0)
	{
		if (ShadowVertexFactory.IsValid())
		{
			ShadowVertexFactory->ClearSplats();
		}
		return;
	}

	// Shadow depth passes read the splats outside RDG, through the shadow vertex factory's
	// uniform buffer: RDG keeps them readable for the rest of the graph (the splat renderer
	// only reads them too) and leaves them so at its end, for the frames of the game where
	// the capture does not move on. The next upload or evaluation takes them back.
	GraphBuilder.UseExternalAccessMode(GraphBuilder.RegisterExternalBuffer(SplatBuffer), ERHIAccess::SRVMask);
	if (NormalBuffer.IsValid())
	{
		GraphBuilder.UseExternalAccessMode(GraphBuilder.RegisterExternalBuffer(NormalBuffer), ERHIAccess::SRVMask);
	}

	FRHICommandListBase& RHICmdList = GraphBuilder.RHICmdList;
	if (!ShadowVertexFactory.IsValid())
	{
		ShadowVertexFactory = MakeUnique<FVGSSplatShadowVertexFactory>(GMaxRHIFeatureLevel);
		ShadowVertexFactory->InitResource(RHICmdList);
	}

	FVGSSplatShadowParameters Parameters;
	FMemory::Memzero(Parameters);
	Parameters.Splats = SplatBuffer->GetSRV(RHICmdList, FRHIBufferSRVCreateInfo());
	Parameters.FileToComponent = State.CaptureToComponent;
	Parameters.NumSplats = UploadedSplats;
	Parameters.Stride = Proxy.GpuDataStride;
	Parameters.BufferFormat = Proxy.BufferFormat;
	Parameters.PositionScale = Proxy.PositionScale;
	Parameters.PositionBias = Proxy.PositionBias;
	Parameters.ScaleScale = Proxy.ScaleScale;
	Parameters.ScaleBias = Proxy.ScaleBias;
	Parameters.OpacityQuantization = FVector2f(Proxy.ColorScale.W, Proxy.ColorBias.W);
	Parameters.PointScale = State.PointScaleFactor;
	Parameters.OpacityScale = State.OpacityScale;
	if (NormalBuffer.IsValid())
	{
		// The editor's normals, written with their outward side.
		Parameters.Normals = NormalBuffer->GetSRV(RHICmdList, FRHIBufferSRVCreateInfo());
		Parameters.HasNormals = 1u;
	}
	Parameters.Center = State.LocalBounds.IsValid ? State.LocalBounds.GetCenter() : FVector3f::ZeroVector;
	Parameters.Inset = FMath::Max(State.ShadowInset, 0.0f);
	ShadowVertexFactory->SetParameters(RHICmdList, Parameters);
}

void FVGSSplatSource::AddShadowMeshes_RenderThread(const FPrimitiveSceneProxy& SceneProxy, const FSceneView& View, int32 ViewIndex, FMeshElementCollector& Collector, bool bReverseCulling) const
{
	if (ShadowVertexFactory.IsValid())
	{
		ShadowVertexFactory->AddShadowMeshes(SceneProxy, View, ViewIndex, Collector, bReverseCulling);
	}
}

void FVGSSplatSource::Collect_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, TArray<const FVGSSplatRenderProxy*>& OutProxies)
{
	// Normally done in Prepare, before the view family; a view the renderer drew without
	// preparing still shows the latest instant, and the shadows of the next frame cast it.
	if (Advance_RenderThread(GraphBuilder))
	{
		UpdateShadows_RenderThread(GraphBuilder);
	}

	if (!State.bVisible || !SplatBuffer.IsValid() || UploadedSplats == 0)
	{
		return;
	}

	// The capture keeps its own axes; the turn into Unreal's is in LocalToWorld, so the
	// harmonics are evaluated in the space they were fitted in (VGSSplatLayout.h).
	Proxy.SHCoordinateSpace = EVGSSplatCoordinateSpace::LH_FRU;
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
	Proxy.Intensity = State.Intensity;
	Proxy.bBlendInSRGB = State.bBlendInSRGB;
	Proxy.Grade = State.Grade;
	Proxy.Tint = State.Tint;
	Proxy.bLit = State.bLit;
	Proxy.SelfIllumination = State.SelfIllumination;
	Proxy.Roughness = State.Roughness;
	Proxy.Specular = State.Specular;
	Proxy.NormalSmoothing = State.NormalSmoothing;
	Proxy.NormalBuffer = NormalBuffer;
	Proxy.bCastShadow = State.bCastShadow;
	Proxy.bVisible = true;
	OutProxies.Add(&Proxy);
}
