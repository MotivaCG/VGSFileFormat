using UnrealBuildTool;

// Maps the plugin's Shaders folder to /Plugin/SGS for the splat renderer copy
// (SGSSplatRenderer), whose global shaders have to be known before the engine compiles
// its global shader map: so this module loads at PostConfigInit and holds nothing else.
public class SGSShaders : ModuleRules
{
	public SGSShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "RenderCore", "RHI" });
		PrivateDependencyModuleNames.AddRange(new[] { "Projects" });
	}
}
