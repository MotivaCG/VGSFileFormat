using UnrealBuildTool;

// The plugin's shaders: VGSPacked.usf, which evaluates packed chunks on the GPU. Global
// shaders have to be known before the engine compiles its global shader map, so this module
// loads at PostConfigInit, as Epic's GaussianSplatShader does, and holds nothing else.
public class VGSShaders : ModuleRules
{
	public VGSShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[] { "Core", "RenderCore", "RHI" });
		// GaussianSplatShader maps /Plugin/GaussianSplat, whose GaussianSplatShared.h gives
		// VGSPacked.usf the renderer's record layout.
		PrivateDependencyModuleNames.AddRange(new[] { "Projects", "GaussianSplatShader" });
	}
}
