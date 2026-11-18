#include "PGSImport.h"

#define VGSBRIDGE_STATIC
#include "vgsbridge.h"
#undef VGSBRIDGE_STATIC

#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	uint32 Word(const uint8* Data) { uint32 Value; FMemory::Memcpy(&Value, Data, 4); return Value; }

	// PGS Editor writes one identity node and a triangle mesh. This reads its geometry for
	// shadows/collision; the embedded display material is not needed by the hidden proxy.
	bool ReadProxy(const uint8* Bytes, size_t Size, pgs::ProxyMesh& Out)
	{
		if (Size < 28 || Size > MAX_int32 || Word(Bytes) != 0x46546c67 || Word(Bytes + 4) != 2 || Word(Bytes + 8) != Size)
			return false;
		TSharedPtr<FJsonObject> Root;
		const uint8* Binary = nullptr;
		uint32 BinarySize = 0;
		for (size_t At = 12; At < Size;)
		{
			if (Size - At < 8) return false;
			const uint32 Length = Word(Bytes + At), Type = Word(Bytes + At + 4);
			At += 8;
			if (Length % 4 != 0 || Length > Size - At) return false;
			if (Type == 0x4e4f534a)
			{
				if (Root) return false;
				const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes + At), int32(Length));
				if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Text.Length(), Text.Get())), Root)) return false;
			}
			else if (Type == 0x004e4942)
			{
				if (Binary) return false;
				Binary = Bytes + At; BinarySize = Length;
			}
			At += Length;
		}
		if (!Root || !Binary) return false;
		auto Object = [](const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name) -> TSharedPtr<FJsonObject>
		{
			const TSharedPtr<FJsonObject>* Value = nullptr;
			return Parent && Parent->TryGetObjectField(Name, Value) ? *Value : nullptr;
		};
		auto Array = [](const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name) -> TArray<TSharedPtr<FJsonValue>>
		{
			const TArray<TSharedPtr<FJsonValue>>* Value = nullptr;
			return Parent && Parent->TryGetArrayField(Name, Value) ? *Value : TArray<TSharedPtr<FJsonValue>>();
		};
		auto Index = [](const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name, int64 Default = -1) -> int64
		{
			double Value;
			if (!Parent || !Parent->TryGetNumberField(Name, Value)) return Default;
			return FMath::IsFinite(Value) && Value >= 0 && Value <= MAX_int32 && Value == FMath::FloorToDouble(Value) ? int64(Value) : -1;
		};
		auto AtObject = [](const TArray<TSharedPtr<FJsonValue>>& Values, int64 At) -> TSharedPtr<FJsonObject>
		{
			return Values.IsValidIndex(int32(At)) && Values[int32(At)]->Type == EJson::Object ? Values[int32(At)]->AsObject() : nullptr;
		};
		const auto Nodes = Array(Root, TEXT("nodes")), Meshes = Array(Root, TEXT("meshes"));
		if (Nodes.Num() != 1 || Meshes.Num() != 1) return false;
		const auto Node = AtObject(Nodes, 0);
		if (!Node || Index(Node, TEXT("mesh")) != 0 || Node->HasField(TEXT("matrix")) || Node->HasField(TEXT("translation"))
			|| Node->HasField(TEXT("rotation")) || Node->HasField(TEXT("scale")) || Node->HasField(TEXT("children"))) return false;
		const auto Primitives = Array(AtObject(Meshes, 0), TEXT("primitives"));
		const auto Primitive = AtObject(Primitives, 0);
		if (Primitives.Num() != 1 || !Primitive || Index(Primitive, TEXT("mode"), 4) != 4 || Primitive->HasField(TEXT("extensions"))) return false;
		const auto Accessors = Array(Root, TEXT("accessors")), Views = Array(Root, TEXT("bufferViews"));
		struct FSpan { const uint8* Data = nullptr; int64 Count = 0, Stride = 0, Component = 0; };
		auto Span = [&](int64 Accessor, const TCHAR* Type, FSpan& OutSpan) -> bool
		{
			const auto A = AtObject(Accessors, Accessor);
			FString Shape;
			if (!A || !A->TryGetStringField(TEXT("type"), Shape) || Shape != Type || A->HasField(TEXT("sparse"))) return false;
			OutSpan.Component = Index(A, TEXT("componentType"));
			const int64 Width = Shape == TEXT("VEC3") ? 12 : OutSpan.Component == 5123 ? 2 : OutSpan.Component == 5125 ? 4 : 0;
			if (!Width || (Shape == TEXT("VEC3") && OutSpan.Component != 5126)) return false;
			const auto V = AtObject(Views, Index(A, TEXT("bufferView")));
			if (!V || Index(V, TEXT("buffer"), 0) != 0) return false;
			const int64 ViewOffset = Index(V, TEXT("byteOffset"), 0), ViewSize = Index(V, TEXT("byteLength"));
			const int64 Offset = Index(A, TEXT("byteOffset"), 0);
			OutSpan.Count = Index(A, TEXT("count")); OutSpan.Stride = Index(V, TEXT("byteStride"), Width);
			if (ViewOffset < 0 || ViewSize < 0 || ViewOffset + ViewSize > BinarySize || Offset < 0 || OutSpan.Count <= 0 || OutSpan.Stride < Width)
				return false;
			if (Offset + (OutSpan.Count - 1) * OutSpan.Stride + Width > ViewSize) return false;
			OutSpan.Data = Binary + ViewOffset + Offset;
			return true;
		};
		FSpan Positions, Indices;
		if (!Span(Index(Object(Primitive, TEXT("attributes")), TEXT("POSITION")), TEXT("VEC3"), Positions)
			|| !Span(Index(Primitive, TEXT("indices")), TEXT("SCALAR"), Indices)
			|| Positions.Count > MAX_int32 / 3 || Indices.Count > MAX_int32 || Indices.Count % 3 != 0) return false;
		Out.positions.resize(size_t(Positions.Count) * 3); Out.indices.resize(size_t(Indices.Count));
		for (int64 V = 0; V < Positions.Count; ++V)
		{
			float* P = &Out.positions[size_t(V) * 3]; FMemory::Memcpy(P, Positions.Data + V * Positions.Stride, 12);
			if (!FMath::IsFinite(P[0]) || !FMath::IsFinite(P[1]) || !FMath::IsFinite(P[2])) return false;
		}
		for (int64 I = 0; I < Indices.Count; ++I)
		{
			const uint8* P = Indices.Data + I * Indices.Stride;
			uint32 IndexValue = Indices.Component == 5123 ? uint32(P[0]) | (uint32(P[1]) << 8) : Word(P);
			if (IndexValue >= Positions.Count) return false;
			Out.indices[size_t(I)] = IndexValue;
		}
		return true;
	}
}

bool ReadPGSImport(const FString& Filename, FPGSImportData& Out, FString& Error)
{
	Out = {};
	const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("PGS"));
	const FString Dll = Plugin ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries/ThirdParty/vgsbridge/Win64/vgsbridge.dll")) : FString();
	void* Library = Dll.IsEmpty() ? nullptr : FPlatformProcess::GetDllHandle(*FPaths::ConvertRelativePathToFull(Dll));
	if (!Library) { Error = TEXT("Cannot load the PGS PGS decoder. Install the complete assembled plugin."); return false; }
	const auto Open = reinterpret_cast<decltype(&vgsb_open_still)>(FPlatformProcess::GetDllExport(Library, TEXT("vgsb_open_still")));
	const auto Close = reinterpret_cast<decltype(&vgsb_close_still)>(FPlatformProcess::GetDllExport(Library, TEXT("vgsb_close_still")));
	const auto LastError = reinterpret_cast<decltype(&vgsb_last_error)>(FPlatformProcess::GetDllExport(Library, TEXT("vgsb_last_error")));
	const auto Version = reinterpret_cast<decltype(&vgsb_api_version)>(FPlatformProcess::GetDllExport(Library, TEXT("vgsb_api_version")));
	bool bSuccess = false;
	if (!Open || !Close || !LastError || !Version || Version() != VGSB_API_VERSION)
		Error = TEXT("The PGS decoder is incompatible. Install the complete current PGS plugin.");
	else
	{
		vgsb_frame Frame{}; const uint8* ProxyBytes = nullptr; size_t ProxySize = 0;
		vgsb_still* Still = Open(TCHAR_TO_UTF8(*FPaths::ConvertRelativePathToFull(Filename)), &Frame, &ProxyBytes, &ProxySize);
		if (!Still) Error = UTF8_TO_TCHAR(LastError());
		else
		{
			const int32 Coefficients = Frame.sh_coefficients;
			if (!Frame.count || (Coefficients != 0 && Coefficients != 3 && Coefficients != 8 && Coefficients != 15)
				|| Frame.count > uint64(MAX_int32 / (56 + 12 * FMath::Max(Coefficients, 0))))
				Error = TEXT("The PGS splat count or harmonic layout cannot be represented by this asset.");
			else
			{
				auto& S = Out.Splats; S.count = size_t(Frame.count); S.shCoefficients = Coefficients;
				S.positions.assign(Frame.positions, Frame.positions + S.count * 3);
				S.rotations.assign(Frame.rotations, Frame.rotations + S.count * 4);
				S.scales.assign(Frame.scales, Frame.scales + S.count * 3);
				S.radiance.assign(Frame.radiance, Frame.radiance + S.count * 4);
				if (Coefficients) S.sh.assign(Frame.sh, Frame.sh + S.count * 3 * Coefficients);
				if (Frame.normals)
				{
					S.normals.assign(Frame.normals, Frame.normals + S.count * 3);
					S.splatsWithNormals = S.count; S.comments.emplace_back("normals PGS capture");
				}
				Out.bHasProxy = ProxySize > 0;
				bSuccess = !Out.bHasProxy || ReadProxy(ProxyBytes, ProxySize, Out.Proxy);
				if (!bSuccess) Error = TEXT("The embedded PGS proxy is not supported: expected the PGS Editor's single identity-node GLB triangle mesh.");
			}
			Close(Still);
		}
	}
	FPlatformProcess::FreeDllHandle(Library);
	if (!bSuccess) Out = {};
	return bSuccess;
}
