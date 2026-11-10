using System.IO;
using UnrealBuildTool;

public class VGSRuntime : ModuleRules
{
	public VGSRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Splats are drawn by VGSSplatRenderer, this plugin's copy of Epic's Gaussian Splat
		// renderer (EPIC_RENDERER_COPY.md). Its proxy still names a type of Epic's
		// GaussianSplat runtime module, whose headers are all in Internal/, and
		// UnrealBuildTool shows Internal/ only to modules of the engine's scope (UEBuildModule.cs,
		// "only if the scope is the same as the SourceModule's scope or if the source module
		// is an engine module"), so this module asks to be treated as an engine module.
		bTreatAsEngineModule = true;

		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Projects",
			"RenderCore",
			"Renderer",
			"RHI",
			"GaussianSplat",
			"GaussianSplatShader",
			"VGSSplatRenderer",
			"VGSShaders"
		});

		// vgsbridge, the closed component that reads captures, is loaded at run time through
		// its C interface, as the Blender add-on loads it. What this plugin knows of the format
		// is how to read a packed chunk on the GPU (Shaders/Private/VGSPacked.usf); everything
		// that decompresses and authenticates a capture is in the bridge. Its header and DLL are put in the plugin by the VGS CMake build
		// (plugins/unreal/CMakeLists.txt); the plugin is not meant to be used from the
		// source tree without that step.
		PrivateIncludePaths.Add(Path.Combine(PluginDirectory, "Source", "ThirdParty", "vgsbridge", "include"));

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			RuntimeDependencies.Add("$(PluginDir)/Binaries/ThirdParty/vgsbridge/Win64/vgsbridge.dll");
			RuntimeDependencies.Add("$(PluginDir)/Binaries/ThirdParty/vgsbridge/Win64/LICENSE.md");
		}
	}
}
