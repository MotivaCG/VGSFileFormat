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
		// The renderer's record layout comes from Shaders/Shared/VGSSplatShared.h, the copy of
		// Epic's GaussianSplatShared.h (EPIC_RENDERER_COPY.md), under /Plugin/VGS/Shared/.
		PrivateDependencyModuleNames.AddRange(new[] { "Projects" });
	}
}
