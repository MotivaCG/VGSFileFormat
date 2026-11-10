using UnrealBuildTool;

// The editor half: importing a Gaussian splat PLY as a VGSStatic asset (normals estimated
// when the file has none), placing it in a level, reimporting it.
public class VGSStaticEditor : ModuleRules
{
	public VGSStaticEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bTreatAsEngineModule = true; // VGSStaticRuntime's renderer headers, as there

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
			"Eigen", // the normals estimate (Private/Normals, the VGS editor's)
			"VGSStaticRuntime"
		});

		// The normals estimate, copied from the VGS editor when the plugin is assembled.
		PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "Private", "Normals"));
	}
}
