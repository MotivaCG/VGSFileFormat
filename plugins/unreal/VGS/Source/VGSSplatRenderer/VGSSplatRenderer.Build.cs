// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class VGSSplatRenderer : ModuleRules
	{
		public VGSSplatRenderer(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

			// VGS CHANGE: Epic's module is an engine module, and sees the renderer's Internal
			// headers; a project plugin has to ask for the same scope.
			bTreatAsEngineModule = true;

			PublicDependencyModuleNames.AddRange(new[] { "Core" });

			PrivateDependencyModuleNames.AddRange(
				new[]
				{
					"CoreUObject",
					"Engine",
					"Renderer",
					"RenderCore",
					"RHI",
					"Projects",
					// VGS CHANGE: no GaussianSplat or GaussianSplatShader: what the renderer used of
					// them is copied (VGSSplatCommon.h, VGSSplatCluster.h, Shaders/Shared), so the
					// plugin does not need Epic's GaussianSplat plugin, which only Unreal 5.9 has.
					"VGSShaders" // VGS CHANGE: maps /Plugin/VGS, where these shaders are
				}
			);

			// VGS CHANGE: the shared shader headers, read by C++ too (as GaussianSplatShader adds
			// Epic's), and the plugin's name, for its Resources (the inverse tone curve LUTs).
			PublicIncludePaths.Add(System.IO.Path.Combine(PluginDirectory, "Shaders", "Shared"));
			PrivateDefinitions.Add("VGSSPLAT_PLUGIN_NAME=TEXT(\"" + System.IO.Path.GetFileNameWithoutExtension(System.IO.Directory.GetFiles(PluginDirectory, "*.uplugin")[0]) + "\")");
			RuntimeDependencies.Add("$(PluginDir)/Resources/InverseToneCurve/...");

			PrivateIncludePaths.Add(System.IO.Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private"));
		}
	}
}
