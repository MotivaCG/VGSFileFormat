#pragma once

#include "CoreMinimal.h"

#include "vgsbridge.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVGS, Log, All);

/*
 * vgsbridge.dll, loaded at run time.
 *
 * The plugin calls the bridge through function pointers rather than linking it, as the
 * Blender add-on does through ctypes: everything that reads the format stays in the
 * separately supplied library, under the VGS Decoder licence beside it, and this module
 * only asks for instants and copies what it is handed. The header is included for its
 * types; its functions are never called directly.
 */
namespace VGS
{
	struct FBridgeApi
	{
		decltype(&vgsb_api_version) ApiVersion = nullptr;
		decltype(&vgsb_last_error) LastError = nullptr;
		decltype(&vgsb_open) Open = nullptr;
		decltype(&vgsb_close) Close = nullptr;
		decltype(&vgsb_get_info) GetInfo = nullptr;
		decltype(&vgsb_schedule) Schedule = nullptr;
		decltype(&vgsb_acquire) Acquire = nullptr;
		decltype(&vgsb_release) Release = nullptr;
		decltype(&vgsb_set_include_sh) SetIncludeSH = nullptr;
		decltype(&vgsb_set_density) SetDensity = nullptr;
	};

	/** Loads the DLL and resolves every function; false, with the reason logged, on failure. */
	bool LoadBridge(const FString& DllPath);
	void UnloadBridge();

	/** The loaded bridge, or null; GetBridgeError says why. */
	const FBridgeApi* GetBridge();
	const FString& GetBridgeError();

	/** The bridge's strings are UTF-8. */
	FString FromUtf8(const char* Text);

	/**
	 * One open capture: a vgsb_player and its decoding lanes. Game thread only - the bridge
	 * allows one thread at a time per player, and the component is the only user.
	 */
	class FPlayer
	{
	public:
		/** Null, with OutError set, if the bridge is missing or the capture does not open. */
		static TSharedPtr<FPlayer> Open(const FString& Path, bool bIncludeSH, int32 Slots, int32 Threads, FString& OutError);
		~FPlayer();

		FPlayer(const FPlayer&) = delete;
		FPlayer& operator=(const FPlayer&) = delete;

		const vgsb_info& GetInfo() const { return Info; }
		const FString& GetPath() const { return Path; }
		int32 GetThreads() const { return Threads; }

		void Schedule(TConstArrayView<double> Seconds);

		/** VGSB_OK, VGSB_TIMEOUT or VGSB_ERROR (LastError says why). Release after VGSB_OK. */
		int32 Acquire(double Seconds, double TimeoutMs, vgsb_frame& OutFrame);
		void Release();

		/** No-ops when nothing changes: both discard what the lanes decoded ahead. */
		void SetIncludeSH(bool bInclude);
		void SetDensity(float Density);

		FString LastError() const;

	private:
		FPlayer() = default;

		vgsb_player* Player = nullptr;
		vgsb_info Info = {};
		FString Path;
		bool bIncludeSH = true;
		float Density = 1.0f;
		int32 Threads = 0;
	};
}
