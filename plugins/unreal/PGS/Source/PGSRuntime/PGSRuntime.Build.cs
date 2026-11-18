using UnrealBuildTool;

public class PGSRuntime : ModuleRules
{
	public PGSRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Splats are drawn by PGSSplatRenderer, the VGS plugin's copy of Epic's Gaussian
		// Splat renderer renamed for this plugin (plugins/unreal/README.md), whose headers are in
		// Internal/; UnrealBuildTool shows Internal/ only to modules of the engine's scope.
		bTreatAsEngineModule = true;

		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"RenderCore",
			"Renderer",
			"RHI",
			"PGSSplatRenderer",
			"PGSShaders"
		});
	}
}
