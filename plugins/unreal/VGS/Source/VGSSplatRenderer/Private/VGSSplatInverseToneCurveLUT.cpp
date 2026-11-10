// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#include "VGSSplatInverseToneCurveLUT.h"
#include "VGSSplatInverseToneCurveLUTCache.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "RHICommandList.h"

DEFINE_LOG_CATEGORY(LogVGSSplat);

namespace VGSSplat
{
	namespace
	{
		// VGS CHANGE: files under the plugin's Resources/InverseToneCurve, one per variant, the
		// data of Epic's assets /GaussianSplat/LUTs/UE_InverseFilmicTC and UE_InverseFilmicTC_Full.
		const TSortedMap<EInverseToneCurveLUTVariant, const TCHAR*> GLUTFiles =
		{
			{ EInverseToneCurveLUTVariant::Filmic,            TEXT("UE_InverseFilmicTC.vgslut") },
			{ EInverseToneCurveLUTVariant::FilmicExpandGamut, TEXT("UE_InverseFilmicTC_Full.vgslut") },
		};

		// "VLUT", edge, channels (3, half floats), unpacked bytes, then a zlib stream of the
		// texels, x fastest (pack_inverse_tone_curve_luts.py). Returns RGBA16F texels.
		bool ReadLUT(const FString& Path, uint32& OutEdge, TArray<uint16>& OutRGBA)
		{
			TArray<uint8> File;
			if (!FFileHelper::LoadFileToArray(File, *Path, FILEREAD_Silent) || File.Num() < 16 || FMemory::Memcmp(File.GetData(), "VLUT", 4) != 0)
			{
				return false;
			}
			uint32 Header[3];
			FMemory::Memcpy(Header, File.GetData() + 4, sizeof(Header));
			const uint32 Edge = Header[0], Channels = Header[1], Unpacked = Header[2];
			const uint64 Texels = uint64(Edge) * Edge * Edge;
			if (Edge < 2 || Edge > 256 || Channels != 3 || Unpacked != Texels * 3 * sizeof(uint16))
			{
				return false;
			}
			TArray<uint16> RGB;
			RGB.SetNumUninitialized(int32(Texels * 3));
			if (!FCompression::UncompressMemory(NAME_Zlib, RGB.GetData(), int32(Unpacked), File.GetData() + 16, File.Num() - 16))
			{
				return false;
			}
			constexpr uint16 HalfOne = 0x3C00;
			OutRGBA.SetNumUninitialized(int32(Texels * 4));
			for (uint64 Texel = 0; Texel < Texels; ++Texel)
			{
				OutRGBA[Texel * 4 + 0] = RGB[Texel * 3 + 0];
				OutRGBA[Texel * 4 + 1] = RGB[Texel * 3 + 1];
				OutRGBA[Texel * 4 + 2] = RGB[Texel * 3 + 2];
				OutRGBA[Texel * 4 + 3] = HalfOne;
			}
			OutEdge = Edge;
			return true;
		}
	}

	FInverseToneCurveLUTCache::FInverseToneCurveLUTCache() = default;

	FInverseToneCurveLUTCache::~FInverseToneCurveLUTCache()
	{
		// The textures are render resources: let the render thread drop the last references.
		TSortedMap<EInverseToneCurveLUTVariant, FSlot> ToRelease = MoveTemp(Slots);
		ENQUEUE_RENDER_COMMAND(ReleaseVGSSplatInverseToneCurveLUTs)([ToRelease = MoveTemp(ToRelease)](FRHICommandListImmediate&) mutable
		{
			ToRelease.Empty();
		});
	}

	FRHITexture* FInverseToneCurveLUTCache::GetRHI_RenderThread(bool bInvertExpandGamut)
	{
		checkSlow(IsInRenderingThread());
		const EInverseToneCurveLUTVariant Variant = static_cast<EInverseToneCurveLUTVariant>(bInvertExpandGamut);
		FSlot& Slot = Slots.FindOrAdd(Variant);
		if (Slot.Texture.IsValid() || Slot.bTried)
		{
			return Slot.Texture.GetReference();
		}
		Slot.bTried = true;

		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(VGSSPLAT_PLUGIN_NAME);
		const FString Path = Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("InverseToneCurve"), GLUTFiles[Variant]) : FString();
		uint32 Edge = 0;
		TArray<uint16> Texels;
		if (!ReadLUT(Path, Edge, Texels))
		{
			UE_LOG(LogVGSSplat, Warning, TEXT("Inverse tone curve LUT not found or unreadable at %s; splats are composited without it."), *Path);
			return nullptr;
		}

		FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create3D(TEXT("VGSSplat.InverseToneCurveLUT"), Edge, Edge, Edge, PF_FloatRGBA)
			.SetFlags(ETextureCreateFlags::ShaderResource)
			.SetInitialState(ERHIAccess::SRVMask);
		Slot.Texture = RHICmdList.CreateTexture(Desc);
		const uint32 RowPitch = Edge * 4 * sizeof(uint16);
		RHICmdList.UpdateTexture3D(Slot.Texture, 0, FUpdateTextureRegion3D(0, 0, 0, 0, 0, 0, Edge, Edge, Edge), RowPitch, RowPitch * Edge,
			reinterpret_cast<const uint8*>(Texels.GetData()));
		return Slot.Texture.GetReference();
	}
}
