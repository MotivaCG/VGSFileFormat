#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

class FVGSStaticShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VGSStatic"));
		check(Plugin.IsValid());
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/VGSStatic"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		// Read by C++ too (the renderer's record layout), as Epic maps its own Shared folder.
		AddShaderSourceSharedVirtualDirectory(TEXT("/Plugin/VGSStatic/Shared/"));
	}
};

IMPLEMENT_MODULE(FVGSStaticShadersModule, VGSStaticShaders)
