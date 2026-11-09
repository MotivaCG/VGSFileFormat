using System.IO;
using UnrealBuildTool;

public class VGSRuntime : ModuleRules
{
	public VGSRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Epic's Gaussian Splat plugin keeps every header in Internal/, and UnrealBuildTool
		// shows Internal/ only to modules of the same scope as the plugin: the engine's
		// (UEBuildModule.cs, "only if the scope is the same as the SourceModule's scope or
		// if the source module is an engine module"). This module includes two of them -
		// GaussianSplatViewExtension.h and GaussianSplatRenderPipeline.h - so it asks
		// to be treated as an engine module, which also lets it live in a project's Plugins
		// folder. Installed under Engine/Plugins it would not need to.
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
			"GaussianSplatSceneViewExtension"
		});

		// vgsbridge, the closed component that reads captures, is loaded at run time through
		// its C interface, as the Blender add-on loads it: this module holds nothing of the
		// format. Its header and DLL are put in the plugin by the VGS CMake build
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
