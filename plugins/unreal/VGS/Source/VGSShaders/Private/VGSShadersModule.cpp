#include "VGSPackedShader.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

class FVGSShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VGS"));
		check(Plugin.IsValid());
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/VGS"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		// Read by C++ too (the renderer's record layout), as Epic maps its own Shared folder.
		AddShaderSourceSharedVirtualDirectory(TEXT("/Plugin/VGS/Shared/"));
	}
};

IMPLEMENT_MODULE(FVGSShadersModule, VGSShaders)

bool FVGSPackedEvaluateCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	// Where the Gaussian Splat renderer draws, which needs compute as this does.
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FVGSPackedEvaluateCS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
}

IMPLEMENT_GLOBAL_SHADER(FVGSPackedEvaluateCS, "/Plugin/VGS/Private/VGSPacked.usf", "MainCS", SF_Compute);
