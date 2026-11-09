#include "VGSBridge.h"

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
