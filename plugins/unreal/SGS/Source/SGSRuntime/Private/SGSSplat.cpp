#include "SGSSplat.h"

#include "SGSPly.h"
#include "SGSSplatGpu.h"

#include "Async/ParallelFor.h"
#include "EditorFramework/AssetImportData.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIResourceUtils.h"
#include "Serialization/CustomVersion.h"
#include "UObject/AssetRegistryTagsContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SGSSplat)

namespace
{
	struct FSGSSplatVersion
	{
		enum Type
		{
			First = 1,
			Latest = First,
		};
		static const FGuid Guid;
	};
	const FGuid FSGSSplatVersion::Guid(0x5C1A7E3B, 0x46D2489F, 0x9A1B03E4, 0x7F2C5D11);
	FCustomVersionRegistration GRegisterSGSSplatVersion(FSGSSplatVersion::Guid, FSGSSplatVersion::Latest, TEXT("SGSSplat"));

	constexpr int32 SplatsPerTask = 16384;
}

USGSSplat::FOnChanged USGSSplat::OnChanged;

void USGSSplat::PostInitProperties()
{
#if WITH_EDITORONLY_DATA
	if (!HasAnyFlags(RF_ClassDefaultObject | RF_NeedLoad))
	{
		AssetImportData = NewObject<UAssetImportData>(this, TEXT("AssetImportData"));
	}
#endif
	Super::PostInitProperties();
}

void USGSSplat::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	Ar.UsingCustomVersion(FSGSSplatVersion::Guid);

	// The splats themselves, in bulk; the UPROPERTYs above only describe them.
	if (Ar.IsLoading())
	{
		TSharedPtr<FSGSSplatData, ESPMode::ThreadSafe> Loaded = MakeShared<FSGSSplatData, ESPMode::ThreadSafe>();
		Ar << Loaded->NumSplats << Loaded->SHCoefficients << Loaded->Stride << Loaded->Bounds;
		Loaded->Records.BulkSerialize(Ar);
		Loaded->Normals.BulkSerialize(Ar);
		Loaded->Clusters.BulkSerialize(Ar);
		const bool bValid = Loaded->NumSplats > 0 && uint64(Loaded->Records.Num()) == uint64(Loaded->NumSplats) * Loaded->Stride
			&& (Loaded->Normals.Num() == 0 || uint32(Loaded->Normals.Num()) == Loaded->NumSplats) && Loaded->Clusters.Num() % 48 == 0;
		Data = bValid ? Loaded : nullptr;
		Gpu.Reset();
	}
	else
	{
		FSGSSplatData Empty;
		FSGSSplatData& Out = Data.IsValid() ? const_cast<FSGSSplatData&>(*Data) : Empty;
		Ar << Out.NumSplats << Out.SHCoefficients << Out.Stride << Out.Bounds;
		Out.Records.BulkSerialize(Ar);
		Out.Normals.BulkSerialize(Ar);
		Out.Clusters.BulkSerialize(Ar);
	}
}

void USGSSplat::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
	Super::GetResourceSizeEx(CumulativeResourceSize);
	if (Data.IsValid())
	{
		CumulativeResourceSize.AddDedicatedSystemMemoryBytes(Data->Records.GetAllocatedSize() + Data->Normals.GetAllocatedSize() + Data->Clusters.GetAllocatedSize());
	}
}

void USGSSplat::SetData(TSharedPtr<const FSGSSplatData, ESPMode::ThreadSafe> InData)
{
	Data = MoveTemp(InData);
	Gpu.Reset();
	OnChanged.Broadcast(this);
}

TSharedPtr<FSGSSplatGpu, ESPMode::ThreadSafe> USGSSplat::GetGpu()
{
	if (!Gpu.IsValid() && Data.IsValid() && Data->NumSplats > 0)
	{
		Gpu = MakeShared<FSGSSplatGpu, ESPMode::ThreadSafe>(Data, GpuLayout == ESGSGpuLayout::Quantized, Normals == ESGSNormals::Axes);
	}
	return Gpu;
}

#if WITH_EDITOR
void USGSSplat::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(USGSSplat, GpuLayout))
	{
		Gpu.Reset();
		OnChanged.Broadcast(this);
	}
}
#endif

#if WITH_EDITORONLY_DATA
void USGSSplat::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
	if (AssetImportData)
	{
		Context.AddTag(FAssetRegistryTag(SourceFileTagName(), AssetImportData->GetSourceData().ToJson(), FAssetRegistryTag::TT_Hidden));
	}
	Context.AddTag(FAssetRegistryTag(TEXT("Splats"), FString::FromInt(NumSplats), FAssetRegistryTag::TT_Numerical));
	Context.AddTag(FAssetRegistryTag(TEXT("Normals"), StaticEnum<ESGSNormals>()->GetDisplayNameTextByValue(int64(Normals)).ToString(), FAssetRegistryTag::TT_Alphabetical));
	Super::GetAssetRegistryTags(Context);
}
#endif

// ---- the GPU copy ----------------------------------------------------------------------

FSGSSplatGpu::FSGSSplatGpu(TSharedPtr<const FSGSSplatData, ESPMode::ThreadSafe> InData, bool bInQuantized, bool bInNormalsFaceCamera)
	: Data(MoveTemp(InData))
{
	bNormalsFaceCamera = bInNormalsFaceCamera;
	check(Data.IsValid());
	NumSplats = Data->NumSplats;
	NumClusters = uint32(Data->Clusters.Num() / 48);
	SHDegree = FMath::Max(vgsunreal::shDegree(Data->SHCoefficients), 0);
	Stride = Data->Stride;
	Bounds = Data->Bounds;
	bQuantized = bInQuantized;
	if (!bQuantized)
	{
		return;
	}

	// Epic's compressed layout, packed once from the float records, in parallel.
	sgs::Splats Unpacked;
	sgs::fromFloatRecords(Data->Records.GetData(), NumSplats, Data->SHCoefficients, Unpacked);
	const vgsunreal::FrameView View = Unpacked.view();
	const int32 Tasks = FMath::DivideAndRoundUp(int32(NumSplats), SplatsPerTask);
	TArray<vgsunreal::Ranges> Ranges;
	Ranges.Init(vgsunreal::Ranges::empty(), Tasks);
	ParallelFor(Tasks, [&](int32 Task)
	{
		const size_t Begin = size_t(Task) * SplatsPerTask;
		Ranges[Task] = vgsunreal::measure(View, Begin, FMath::Min<size_t>(Begin + SplatsPerTask, NumSplats));
	});
	vgsunreal::Ranges All = vgsunreal::Ranges::empty();
	for (const vgsunreal::Ranges& R : Ranges)
	{
		All.merge(R);
	}
	Quantization = vgsunreal::quantization(All);
	Stride = vgsunreal::quantizedStride(Data->SHCoefficients);
	QuantizedRecords.SetNumUninitialized(int64(NumSplats) * Stride);
	ParallelFor(Tasks, [&](int32 Task)
	{
		const size_t Begin = size_t(Task) * SplatsPerTask;
		vgsunreal::packQuantized(View, Quantization, Begin, FMath::Min<size_t>(Begin + SplatsPerTask, NumSplats), QuantizedRecords.GetData());
	});
}

void FSGSSplatGpu::Ensure_RenderThread(FRDGBuilder& GraphBuilder)
{
	Ensure_RenderThread(GraphBuilder.RHICmdList);
}

namespace
{
	/**
	 * A buffer made with its data, outside a render graph: the shadow pass's vertex factory
	 * may need the splats before the splat renderer's graph runs (a character off screen
	 * whose shadow is on it). Wrapped as a pooled buffer for the renderer.
	 */
	TRefCountPtr<FRDGPooledBuffer> MakeBuffer(FRHICommandListBase& RHICmdList, const FRDGBufferDesc& Desc, const void* Data, uint32 Bytes, const TCHAR* Name)
	{
		FBufferRHIRef Buffer = UE::RHIResourceUtils::CreateBufferFromArray(RHICmdList, Name, Desc.Usage, Desc.BytesPerElement, Data, Bytes);
		return new FRDGPooledBuffer(RHICmdList, Buffer, Desc, Desc.NumElements, Name);
	}
}

void FSGSSplatGpu::Ensure_RenderThread(FRHICommandListBase& RHICmdList)
{
	check(IsInRenderingThread());
	if (bUploaded || IsEmpty())
	{
		return;
	}
	bUploaded = true;

	// The records: a byte address buffer, as the renderer reads them, padded to 16 bytes.
	const TArray<uint8>& Records = bQuantized ? QuantizedRecords : Data->Records;
	const uint32 Bytes = uint32(Align(uint64(Records.Num()), uint64(16)));
	TArray<uint8> Padded;
	const uint8* Source = Records.GetData();
	if (Bytes != uint32(Records.Num()))
	{
		Padded.SetNumZeroed(Bytes);
		FMemory::Memcpy(Padded.GetData(), Records.GetData(), Records.Num());
		Source = Padded.GetData();
	}
	Splats = MakeBuffer(RHICmdList, FRDGBufferDesc::CreateByteAddressDesc(Bytes), Source, Bytes, TEXT("SGS.Splats"));
	SplatsSRV = RHICmdList.CreateShaderResourceView(Splats->GetRHI(), FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(Splats->GetRHI()));
	if (bQuantized)
	{
		QuantizedRecords.Empty(); // on the GPU now; the float records stay with the asset
	}

	if (Data->Normals.Num() > 0)
	{
		Normals = MakeBuffer(RHICmdList, FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), uint32(Data->Normals.Num())),
			Data->Normals.GetData(), uint32(Data->Normals.Num()) * sizeof(uint32), TEXT("SGS.Normals"));
		NormalsSRV = RHICmdList.CreateShaderResourceView(Normals->GetRHI(), FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(Normals->GetRHI()));
	}
	if (NumClusters > 0)
	{
		Clusters = MakeBuffer(RHICmdList, FRDGBufferDesc::CreateStructuredDesc(48, NumClusters),
			Data->Clusters.GetData(), uint32(Data->Clusters.Num()), TEXT("SGS.Clusters"));
	}
}
