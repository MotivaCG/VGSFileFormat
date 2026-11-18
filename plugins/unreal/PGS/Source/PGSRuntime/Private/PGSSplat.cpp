#include "PGSSplat.h"

#include "PGSPly.h"
#include "PGSSplatGpu.h"

#include "Async/ParallelFor.h"
#include "EditorFramework/AssetImportData.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIResourceUtils.h"
#include "Serialization/CustomVersion.h"
#include "UObject/AssetRegistryTagsContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PGSSplat)

namespace
{
	struct FPGSSplatVersion
	{
		enum Type
		{
			First = 1,
			Latest = First,
		};
		static const FGuid Guid;
	};
	const FGuid FPGSSplatVersion::Guid(0x5C1A7E3B, 0x46D2489F, 0x9A1B03E4, 0x7F2C5D11);
	FCustomVersionRegistration GRegisterPGSSplatVersion(FPGSSplatVersion::Guid, FPGSSplatVersion::Latest, TEXT("PGSSplat"));

	constexpr int32 SplatsPerTask = 16384;
}

UPGSSplat::FOnChanged UPGSSplat::OnChanged;

void UPGSSplat::PostInitProperties()
{
#if WITH_EDITORONLY_DATA
	if (!HasAnyFlags(RF_ClassDefaultObject | RF_NeedLoad))
	{
		AssetImportData = NewObject<UAssetImportData>(this, TEXT("AssetImportData"));
	}
#endif
	Super::PostInitProperties();
}

void UPGSSplat::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	Ar.UsingCustomVersion(FPGSSplatVersion::Guid);

	// The splats themselves, in bulk; the UPROPERTYs above only describe them.
	if (Ar.IsLoading())
	{
		TSharedPtr<FPGSSplatData, ESPMode::ThreadSafe> Loaded = MakeShared<FPGSSplatData, ESPMode::ThreadSafe>();
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
		FPGSSplatData Empty;
		FPGSSplatData& Out = Data.IsValid() ? const_cast<FPGSSplatData&>(*Data) : Empty;
		Ar << Out.NumSplats << Out.SHCoefficients << Out.Stride << Out.Bounds;
		Out.Records.BulkSerialize(Ar);
		Out.Normals.BulkSerialize(Ar);
		Out.Clusters.BulkSerialize(Ar);
	}
}

void UPGSSplat::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
	Super::GetResourceSizeEx(CumulativeResourceSize);
	if (Data.IsValid())
	{
		CumulativeResourceSize.AddDedicatedSystemMemoryBytes(Data->Records.GetAllocatedSize() + Data->Normals.GetAllocatedSize() + Data->Clusters.GetAllocatedSize());
	}
}

void UPGSSplat::SetData(TSharedPtr<const FPGSSplatData, ESPMode::ThreadSafe> InData)
{
	Data = MoveTemp(InData);
	Gpu.Reset();
	OnChanged.Broadcast(this);
}

TSharedPtr<FPGSSplatGpu, ESPMode::ThreadSafe> UPGSSplat::GetGpu()
{
	if (!Gpu.IsValid() && Data.IsValid() && Data->NumSplats > 0)
	{
		Gpu = MakeShared<FPGSSplatGpu, ESPMode::ThreadSafe>(Data, GpuLayout == EPGSGpuLayout::Quantized, Normals == EPGSNormals::Axes);
	}
	return Gpu;
}

#if WITH_EDITOR
void UPGSSplat::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UPGSSplat, GpuLayout))
	{
		Gpu.Reset();
		OnChanged.Broadcast(this);
	}
}
#endif

#if WITH_EDITORONLY_DATA
void UPGSSplat::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
	if (AssetImportData)
	{
		Context.AddTag(FAssetRegistryTag(SourceFileTagName(), AssetImportData->GetSourceData().ToJson(), FAssetRegistryTag::TT_Hidden));
	}
	Context.AddTag(FAssetRegistryTag(TEXT("Splats"), FString::FromInt(NumSplats), FAssetRegistryTag::TT_Numerical));
	Context.AddTag(FAssetRegistryTag(TEXT("Normals"), StaticEnum<EPGSNormals>()->GetDisplayNameTextByValue(int64(Normals)).ToString(), FAssetRegistryTag::TT_Alphabetical));
	Super::GetAssetRegistryTags(Context);
}
#endif

// ---- the GPU copy ----------------------------------------------------------------------

FPGSSplatGpu::FPGSSplatGpu(TSharedPtr<const FPGSSplatData, ESPMode::ThreadSafe> InData, bool bInQuantized, bool bInNormalsFaceCamera)
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
	pgs::Splats Unpacked;
	pgs::fromFloatRecords(Data->Records.GetData(), NumSplats, Data->SHCoefficients, Unpacked);
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

void FPGSSplatGpu::Ensure_RenderThread(FRDGBuilder& GraphBuilder)
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

void FPGSSplatGpu::Ensure_RenderThread(FRHICommandListBase& RHICmdList)
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
	Splats = MakeBuffer(RHICmdList, FRDGBufferDesc::CreateByteAddressDesc(Bytes), Source, Bytes, TEXT("PGS.Splats"));
	SplatsSRV = RHICmdList.CreateShaderResourceView(Splats->GetRHI(), FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(Splats->GetRHI()));
	if (bQuantized)
	{
		QuantizedRecords.Empty(); // on the GPU now; the float records stay with the asset
	}

	if (Data->Normals.Num() > 0)
	{
		Normals = MakeBuffer(RHICmdList, FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), uint32(Data->Normals.Num())),
			Data->Normals.GetData(), uint32(Data->Normals.Num()) * sizeof(uint32), TEXT("PGS.Normals"));
		NormalsSRV = RHICmdList.CreateShaderResourceView(Normals->GetRHI(), FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(Normals->GetRHI()));
	}
	if (NumClusters > 0)
	{
		Clusters = MakeBuffer(RHICmdList, FRDGBufferDesc::CreateStructuredDesc(48, NumClusters),
			Data->Clusters.GetData(), uint32(Data->Clusters.Num()), TEXT("PGS.Clusters"));
	}
}
