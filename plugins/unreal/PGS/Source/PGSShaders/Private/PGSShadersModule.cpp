#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

class FPGSShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PGS"));
		check(Plugin.IsValid());
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/PGS"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		// Read by C++ too (the renderer's record layout), as Epic maps its own Shared folder.
		AddShaderSourceSharedVirtualDirectory(TEXT("/Plugin/PGS/Shared/"));
	}
};

IMPLEMENT_MODULE(FPGSShadersModule, PGSShaders)
