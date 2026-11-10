#include "VGSPlayer.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

class FVGSRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Beside the plugin, where the VGS build puts it and where a packaged game stages it
		// (VGSRuntime.Build.cs lists it as a runtime dependency).
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VGS"));
		if (!Plugin.IsValid())
		{
			UE_LOG(LogVGS, Error, TEXT("The VGS plugin cannot find itself; vgsbridge is not loaded"));
			return;
		}
		const FString Dll = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries/ThirdParty/vgsbridge/Win64/vgsbridge.dll"));
		VGS::LoadBridge(FPaths::ConvertRelativePathToFull(Dll));
	}

	virtual void ShutdownModule() override
	{
		VGS::UnloadBridge();
	}
};

IMPLEMENT_MODULE(FVGSRuntimeModule, VGSRuntime)

#if !UE_BUILD_SHIPPING
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "HAL/IConsoleManager.h"

// -ExecCmds runs on the first frame, before a capture has a frame on screen or Epic's
// renderer has loaded its inverse tone curve; screenshots for comparing looks need later.
namespace
{
	void RunAfter(const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			return;
		}
		const float Delay = FCString::Atof(*Args[0]);
		TArray<FString> Rest = Args;
		Rest.RemoveAt(0);
		const FString Command = FString::Join(Rest, TEXT(" "));
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Command](float)
		{
			if (GEngine && GEngine->GameViewport)
			{
				GEngine->GameViewport->ConsoleCommand(Command);
			}
			return false;
		}), Delay);
	}

	FAutoConsoleCommand GVGSDebugAfter(
		TEXT("vgs.Debug.After"),
		TEXT("vgs.Debug.After <seconds> <command...>: runs a console command later, through the game viewport, e.g. vgs.Debug.After 8 HighResShot 1920x1080"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunAfter));
}
#endif
