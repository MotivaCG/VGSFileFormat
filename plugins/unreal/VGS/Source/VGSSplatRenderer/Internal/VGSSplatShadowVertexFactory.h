// VGS: not Epic's. Splats in Unreal's shadow depth passes, so that they cast shadows.
#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "ShaderParameterMacros.h"
#include "VertexFactory.h"

class FMeshElementCollector;
class FPrimitiveSceneProxy;
class FMaterialRenderProxy;
class FSceneView;

BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FVGSSplatShadowParameters, VGSSPLATRENDERER_API)
	SHADER_PARAMETER_SRV(ByteAddressBuffer, Splats)
	SHADER_PARAMETER_SRV(StructuredBuffer<uint>, Normals) // the renderer's normal words; a dummy when HasNormals is 0
	SHADER_PARAMETER(FMatrix44f, FileToComponent) // the asset's axes and units to the component's (row vectors)
	SHADER_PARAMETER(FVector3f, PositionScale) // the quantized layout's, as the renderer's proxy has them
	SHADER_PARAMETER(uint32, NumSplats)
	SHADER_PARAMETER(FVector3f, PositionBias)
	SHADER_PARAMETER(uint32, Stride)
	SHADER_PARAMETER(FVector3f, ScaleScale)
	SHADER_PARAMETER(uint32, BufferFormat) // 0 float records, 1 quantized
	SHADER_PARAMETER(FVector3f, ScaleBias)
	SHADER_PARAMETER(float, PointScale)
	SHADER_PARAMETER(FVector2f, OpacityQuantization) // scale and bias of the quantized opacity
	SHADER_PARAMETER(float, OpacityScale)
	SHADER_PARAMETER(FVector3f, Center) // of the asset, in its own space: what "inwards" means without a normal's side
	SHADER_PARAMETER(uint32, HasNormals) // 0 none (shortest axes), 1 with their side, 2 directions only
	SHADER_PARAMETER(float, Inset) // component units the casters are moved inwards, along their normal
END_GLOBAL_SHADER_PARAMETER_STRUCT()

namespace VGSSplat
{
	/**
	 * How splats cast shadows (r.VGSSplat.Shadow.Method, which the project's settings set,
	 * UVGSSplatSettings), in the order the settings list them:
	 * 0 the splat renderer's own shadow map of the main directional light, composited onto the
	 *   lit scene (VGSSplatLightShadow.h), the default;
	 * 1 that for the main directional light, and Unreal's shadow maps for every other light;
	 * 2 Unreal's shadow maps for every light, as octahedra at a random cutoff each (this vertex
	 *   factory);
	 * 3 none.
	 */
	enum class EShadowMethod : int32 { SplatShadowMap = 0, Mixed = 1, UnrealShadowMaps = 2, None = 3 };
	VGSSPLATRENDERER_API EShadowMethod GetShadowMethod();

	/** True if the splat renderer's own shadow map is drawn (methods 0 and 1). */
	inline bool UsesSplatShadowMap()
	{
		const EShadowMethod Method = GetShadowMethod();
		return Method == EShadowMethod::SplatShadowMap || Method == EShadowMethod::Mixed;
	}
}

/**
 * Draws splats into shadow depth maps as small opaque octahedra, each at a random opacity
 * cutoff (Shaders/Private/SplatRenderer/VGSSplatShadowVertexFactory.ush), straight from the
 * renderer's splat buffer: no memory of their own beyond one shared index buffer of 512 KB.
 * Supports GPU Scene, so virtual shadow maps take them as they take meshes.
 *
 * Its shaders are compiled for Unreal's default material only, which draws them.
 */
class VGSSPLATRENDERER_API FVGSSplatShadowVertexFactory : public FVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FVGSSplatShadowVertexFactory);

public:
	explicit FVGSSplatShadowVertexFactory(ERHIFeatureLevel::Type InFeatureLevel);

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;

	static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);

	/**
	 * Render thread: what the next draws read. Call again when any of it changes, even every
	 * frame (the parameters are updated in place, in order with recorded commands), before the
	 * frame's shadow depth passes gather their meshes.
	 */
	void SetParameters(FRHICommandListBase& RHICmdList, const FVGSSplatShadowParameters& Parameters);

	/** Render thread: casts nothing until SetParameters is called again. */
	void ClearSplats();

	/**
	 * Adds this proxy's splats, NumSplats of them, to a view's shadow casters: a batch of
	 * draws (one per SplatsPerDraw splats) drawing nothing in the view itself, as long as the
	 * proxy's view relevance keeps it out of the main and depth passes. Nothing unless the
	 * method draws into Unreal's shadow maps; with Mixed, nothing for the main directional
	 * light's shadow views, which the splats' own shadow map covers.
	 */
	void AddShadowMeshes(const FPrimitiveSceneProxy& Proxy, const FSceneView& View, int32 ViewIndex, FMeshElementCollector& Collector, bool bReverseCulling) const;

	/** Splats per draw: as many as 16-bit indices reach, six vertices each. */
	static constexpr uint32 SplatsPerDraw = 10920;

	bool HasParameters() const { return UniformBuffer.IsValid() && NumSplats > 0; }

private:
	TUniformBufferRef<FVGSSplatShadowParameters> UniformBuffer;
	uint32 NumSplats = 0;

	friend class FVGSSplatShadowVertexFactoryShaderParameters;
};
