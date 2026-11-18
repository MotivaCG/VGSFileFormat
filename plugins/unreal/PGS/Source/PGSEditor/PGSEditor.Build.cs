using UnrealBuildTool;

// The editor half: importing a Gaussian splat PLY as a PGS asset (normals estimated
// when the file has none), placing it in a level, reimporting it.
public class PGSEditor : ModuleRules
{
	public PGSEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bTreatAsEngineModule = true; // PGSRuntime's renderer headers, as there

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"AssetDefinition",
			"Slate",
			"SlateCore",
			"InputCore",
			"PropertyEditor",
			"ToolMenus",
			"Projects",
			"Json",
			"MeshDescription", // the shadow proxy mesh
			"StaticMeshDescription",
			"PhysicsCore",
			"Eigen", // the normals estimate (Private/Normals, the VGS editor's)
			"RenderCore",
			"RHI",
			"ImageCore",
			"PGSRuntime"
		});

		// The normals estimate, copied from the VGS editor when the plugin is assembled.
		PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "Private", "Normals"));
		PrivateIncludePaths.Add(System.IO.Path.Combine(PluginDirectory, "Source", "ThirdParty", "vgsbridge", "include"));
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			RuntimeDependencies.Add("$(PluginDir)/Binaries/ThirdParty/vgsbridge/Win64/vgsbridge.dll");
			RuntimeDependencies.Add("$(PluginDir)/Binaries/ThirdParty/vgsbridge/Win64/LICENSE.md");
		}
	}
}
