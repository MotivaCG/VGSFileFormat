#include "VGSPlayer.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

#include <type_traits>

DEFINE_LOG_CATEGORY(LogVGS);

namespace VGS
{
	namespace
	{
		void* GDllHandle = nullptr;
		FBridgeApi GApi;
		bool GLoaded = false;
		FString GError = TEXT("vgsbridge has not been loaded");
	}

	FString FromUtf8(const char* Text)
	{
		return Text ? FString(StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(Text)).Get()) : FString();
	}

	bool LoadBridge(const FString& DllPath)
	{
		if (GLoaded)
		{
			return true;
		}

		if (!FPaths::FileExists(DllPath))
		{
			GError = FString::Printf(TEXT("vgsbridge.dll is missing: %s"), *DllPath);
			UE_LOG(LogVGS, Error, TEXT("%s"), *GError);
			return false;
		}

		GDllHandle = FPlatformProcess::GetDllHandle(*DllPath);
		if (!GDllHandle)
		{
			GError = FString::Printf(TEXT("vgsbridge.dll did not load: %s"), *DllPath);
			UE_LOG(LogVGS, Error, TEXT("%s"), *GError);
			return false;
		}

		bool bComplete = true;
		auto Resolve = [&bComplete](auto& Function, const TCHAR* Name)
		{
			Function = reinterpret_cast<std::remove_reference_t<decltype(Function)>>(FPlatformProcess::GetDllExport(GDllHandle, Name));
			if (!Function)
			{
				UE_LOG(LogVGS, Error, TEXT("vgsbridge.dll has no %s"), Name);
				bComplete = false;
			}
		};
		FBridgeApi Api;
		Resolve(Api.ApiVersion, TEXT("vgsb_api_version"));
		Resolve(Api.LastError, TEXT("vgsb_last_error"));
		Resolve(Api.Open, TEXT("vgsb_open"));
		Resolve(Api.Close, TEXT("vgsb_close"));
		Resolve(Api.GetInfo, TEXT("vgsb_get_info"));
		Resolve(Api.Schedule, TEXT("vgsb_schedule"));
		Resolve(Api.Acquire, TEXT("vgsb_acquire"));
		Resolve(Api.Release, TEXT("vgsb_release"));
		Resolve(Api.SetIncludeSH, TEXT("vgsb_set_include_sh"));
		Resolve(Api.SetDensity, TEXT("vgsb_set_density"));

		// A bridge built against other structs would be read wrongly, so refuse it outright.
		if (bComplete && Api.ApiVersion() != VGSB_API_VERSION)
		{
			GError = FString::Printf(TEXT("vgsbridge.dll speaks API %d, this plugin %d: install the plugin and the DLL from the same package"),
				Api.ApiVersion(), VGSB_API_VERSION);
			bComplete = false;
		}
		else if (!bComplete)
		{
			GError = TEXT("vgsbridge.dll is incomplete");
		}

		if (!bComplete)
		{
			UE_LOG(LogVGS, Error, TEXT("%s"), *GError);
			FPlatformProcess::FreeDllHandle(GDllHandle);
			GDllHandle = nullptr;
			return false;
		}

		GApi = Api;
		GLoaded = true;
		GError.Reset();
		UE_LOG(LogVGS, Log, TEXT("Loaded %s"), *DllPath);
		return true;
	}

	void UnloadBridge()
	{
		if (GDllHandle)
		{
			FPlatformProcess::FreeDllHandle(GDllHandle);
			GDllHandle = nullptr;
		}
		GApi = FBridgeApi();
		GLoaded = false;
		GError = TEXT("vgsbridge has been unloaded");
	}

	const FBridgeApi* GetBridge()
	{
		return GLoaded ? &GApi : nullptr;
	}

	const FString& GetBridgeError()
	{
		return GError;
	}

	TSharedPtr<FPlayer> FPlayer::Open(const FString& Path, bool bIncludeSH, int32 Slots, FString& OutError)
	{
		const FBridgeApi* Api = GetBridge();
		if (!Api)
		{
			OutError = GetBridgeError();
			return nullptr;
		}

		// The bridge maps the file itself and takes UTF-8, so accented paths work.
		const auto Utf8Path = StringCast<UTF8CHAR>(*Path);
		vgsb_player* Player = Api->Open(reinterpret_cast<const char*>(Utf8Path.Get()), bIncludeSH ? 1 : 0, FMath::Clamp(Slots, 2, 64), 0);
		if (!Player)
		{
			OutError = FromUtf8(Api->LastError());
			return nullptr;
		}

		TSharedPtr<FPlayer> Result(new FPlayer());
		Result->Player = Player;
		Result->Path = Path;
		Result->bIncludeSH = bIncludeSH;
		Api->GetInfo(Player, &Result->Info);
		return Result;
	}

	FPlayer::~FPlayer()
	{
		if (Player)
		{
			if (const FBridgeApi* Api = GetBridge())
			{
				Api->Close(Player);
			}
			Player = nullptr;
		}
	}

	void FPlayer::Schedule(TConstArrayView<double> Seconds)
	{
		if (const FBridgeApi* Api = GetBridge())
		{
			Api->Schedule(Player, Seconds.GetData(), Seconds.Num());
		}
	}

	int32 FPlayer::Acquire(double Seconds, double TimeoutMs, vgsb_frame& OutFrame)
	{
		const FBridgeApi* Api = GetBridge();
		return Api ? Api->Acquire(Player, Seconds, TimeoutMs, &OutFrame) : VGSB_ERROR;
	}

	void FPlayer::Release()
	{
		if (const FBridgeApi* Api = GetBridge())
		{
			Api->Release(Player);
		}
	}

	void FPlayer::SetIncludeSH(bool bInclude)
	{
		const FBridgeApi* Api = GetBridge();
		if (Api && bInclude != bIncludeSH)
		{
			Api->SetIncludeSH(Player, bInclude ? 1 : 0);
			bIncludeSH = bInclude;
		}
	}

	void FPlayer::SetDensity(float InDensity)
	{
		const FBridgeApi* Api = GetBridge();
		const float Clamped = FMath::Clamp(InDensity, 0.01f, 1.0f);
		if (Api && Clamped != Density)
		{
			Api->SetDensity(Player, Clamped);
			Density = Clamped;
		}
	}

	FString FPlayer::LastError() const
	{
		const FBridgeApi* Api = GetBridge();
		return Api ? FromUtf8(Api->LastError()) : GetBridgeError();
	}
}
