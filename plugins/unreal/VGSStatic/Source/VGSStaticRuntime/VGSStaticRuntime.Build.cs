using UnrealBuildTool;

public class VGSStaticRuntime : ModuleRules
{
	public VGSStaticRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Splats are drawn by VGSStaticSplatRenderer, the VGS plugin's copy of Epic's Gaussian
		// Splat renderer renamed for this plugin (plugins/unreal/README.md), whose headers are in
		// Internal/; UnrealBuildTool shows Internal/ only to modules of the engine's scope.
		bTreatAsEngineModule = true;

		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"RenderCore",
			"Renderer",
			"RHI",
			"VGSStaticSplatRenderer",
			"VGSStaticShaders"
		});
	}
}
