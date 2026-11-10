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

			// VGS CHANGE: Epic's module is an engine module and sees the GaussianSplat runtime
			// module's Internal headers; a project plugin has to ask for the same scope.
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
					"GaussianSplat",
					"GaussianSplatShader",
					"VGSShaders" // VGS CHANGE: maps /Plugin/VGS, where these shaders are
				}
			);

			PrivateIncludePaths.Add(System.IO.Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private"));
		}
	}
}
