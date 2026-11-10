#include "VGSStaticSplat.h"

#include "VGSStaticPly.h"
#include "VGSStaticSplatGpu.h"

#include "Async/ParallelFor.h"
#include "EditorFramework/AssetImportData.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "Serialization/CustomVersion.h"
#include "UObject/AssetRegistryTagsContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VGSStaticSplat)

namespace
{
	struct FVGSStaticSplatVersion
	{
		enum Type
		{
			First = 1,
			Latest = First,
		};
		static const FGuid Guid;
	};
	const FGuid FVGSStaticSplatVersion::Guid(0x5C1A7E3B, 0x46D2489F, 0x9A1B03E4, 0x7F2C5D11);
	FCustomVersionRegistration GRegisterVGSStaticSplatVersion(FVGSStaticSplatVersion::Guid, FVGSStaticSplatVersion::Latest, TEXT("VGSStaticSplat"));

	constexpr int32 SplatsPerTask = 16384;
}

UVGSStaticSplat::FOnChanged UVGSStaticSplat::OnChanged;

void UVGSStaticSplat::PostInitProperties()
{
#if WITH_EDITORONLY_DATA
	if (!HasAnyFlags(RF_ClassDefaultObject | RF_NeedLoad))
	{
		AssetImportData = NewObject<UAssetImportData>(this, TEXT("AssetImportData"));
	}
#endif
	Super::PostInitProperties();
}

void UVGSStaticSplat::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	Ar.UsingCustomVersion(FVGSStaticSplatVersion::Guid);

	// The splats themselves, in bulk; the UPROPERTYs above only describe them.
	if (Ar.IsLoading())
	{
		TSharedPtr<FVGSStaticSplatData, ESPMode::ThreadSafe> Loaded = MakeShared<FVGSStaticSplatData, ESPMode::ThreadSafe>();
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
		FVGSStaticSplatData Empty;
		FVGSStaticSplatData& Out = Data.IsValid() ? const_cast<FVGSStaticSplatData&>(*Data) : Empty;
		Ar << Out.NumSplats << Out.SHCoefficients << Out.Stride << Out.Bounds;
		Out.Records.BulkSerialize(Ar);
		Out.Normals.BulkSerialize(Ar);
		Out.Clusters.BulkSerialize(Ar);
	}
}

void UVGSStaticSplat::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
	Super::GetResourceSizeEx(CumulativeResourceSize);
	if (Data.IsValid())
	{
		CumulativeResourceSize.AddDedicatedSystemMemoryBytes(Data->Records.GetAllocatedSize() + Data->Normals.GetAllocatedSize() + Data->Clusters.GetAllocatedSize());
	}
}

void UVGSStaticSplat::SetData(TSharedPtr<const FVGSStaticSplatData, ESPMode::ThreadSafe> InData)
{
	Data = MoveTemp(InData);
	Gpu.Reset();
	OnChanged.Broadcast(this);
}

TSharedPtr<FVGSStaticSplatGpu, ESPMode::ThreadSafe> UVGSStaticSplat::GetGpu()
{
	if (!Gpu.IsValid() && Data.IsValid() && Data->NumSplats > 0)
	{
		Gpu = MakeShared<FVGSStaticSplatGpu, ESPMode::ThreadSafe>(Data, GpuLayout == EVGSStaticGpuLayout::Quantized, Normals == EVGSStaticNormals::Axes);
	}
	return Gpu;
}

#if WITH_EDITOR
void UVGSStaticSplat::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UVGSStaticSplat, GpuLayout))
	{
		Gpu.Reset();
		OnChanged.Broadcast(this);
	}
}
#endif

#if WITH_EDITORONLY_DATA
void UVGSStaticSplat::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
	if (AssetImportData)
	{
		Context.AddTag(FAssetRegistryTag(SourceFileTagName(), AssetImportData->GetSourceData().ToJson(), FAssetRegistryTag::TT_Hidden));
	}
	Context.AddTag(FAssetRegistryTag(TEXT("Splats"), FString::FromInt(NumSplats), FAssetRegistryTag::TT_Numerical));
	Context.AddTag(FAssetRegistryTag(TEXT("Normals"), StaticEnum<EVGSStaticNormals>()->GetDisplayNameTextByValue(int64(Normals)).ToString(), FAssetRegistryTag::TT_Alphabetical));
	Super::GetAssetRegistryTags(Context);
}
#endif

// ---- the GPU copy ----------------------------------------------------------------------

FVGSStaticSplatGpu::FVGSStaticSplatGpu(TSharedPtr<const FVGSStaticSplatData, ESPMode::ThreadSafe> InData, bool bInQuantized, bool bInNormalsFaceCamera)
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
	vgsstatic::Splats Unpacked;
	vgsstatic::fromFloatRecords(Data->Records.GetData(), NumSplats, Data->SHCoefficients, Unpacked);
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

void FVGSStaticSplatGpu::Ensure_RenderThread(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());
	if (bUploaded || IsEmpty())
	{
		return;
	}
	bUploaded = true;

	// The records: a byte address buffer, as the renderer reads them. RDG copies the data, so
	// nothing here has to outlive the graph.
	const TArray<uint8>& Records = bQuantized ? QuantizedRecords : Data->Records;
	const uint32 Bytes = uint32(Align(uint64(Records.Num()), uint64(16)));
	FRDGBufferRef SplatBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(Bytes), TEXT("VGSStatic.Splats"));
	GraphBuilder.QueueBufferUpload(SplatBuffer, Records.GetData(), Records.Num());
	Splats = GraphBuilder.ConvertToExternalBuffer(SplatBuffer);
	if (bQuantized)
	{
		QuantizedRecords.Empty(); // on the GPU now; the float records stay with the asset
	}

	if (Data->Normals.Num() > 0)
	{
		FRDGBufferRef NormalBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("VGSStatic.Normals"), sizeof(uint32), uint32(Data->Normals.Num()),
			Data->Normals.GetData(), uint32(Data->Normals.Num()) * sizeof(uint32));
		Normals = GraphBuilder.ConvertToExternalBuffer(NormalBuffer);
	}
	if (NumClusters > 0)
	{
		FRDGBufferRef ClusterBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("VGSStatic.Clusters"), 48, NumClusters,
			Data->Clusters.GetData(), uint32(Data->Clusters.Num()));
		Clusters = GraphBuilder.ConvertToExternalBuffer(ClusterBuffer);
	}
}
