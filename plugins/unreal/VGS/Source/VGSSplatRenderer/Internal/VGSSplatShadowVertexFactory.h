// VGS: not Epic's. Splats in Unreal's shadow depth passes, so that they cast shadows.
#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "ShaderParameterMacros.h"
#include "VertexFactory.h"

class FMeshElementCollector;
class FPrimitiveSceneProxy;
class FMaterialRenderProxy;

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
	SHADER_PARAMETER(float, OpacityCutoff)
	SHADER_PARAMETER(FVector3f, Center) // of the asset, in its own space: what "inwards" means without a normal's side
	SHADER_PARAMETER(uint32, HasNormals) // 0 none (shortest axes), 1 with their side, 2 directions only
	SHADER_PARAMETER(float, Inset) // component units the casters are moved inwards, along their normal
END_GLOBAL_SHADER_PARAMETER_STRUCT()

/**
 * Draws splats into shadow depth maps as small opaque octahedra
 * (Shaders/Private/SplatRenderer/VGSSplatShadowVertexFactory.ush), straight from the
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

	/** Render thread: what the next draws read. Call again when any of it changes. */
	void SetParameters(FRHICommandListBase& RHICmdList, const FVGSSplatShadowParameters& Parameters);

	/**
	 * Adds this proxy's splats, NumSplats of them, to a view's shadow casters: a batch of
	 * draws (one per SplatsPerDraw splats) drawing nothing in the view itself, as long as the
	 * proxy's view relevance keeps it out of the main and depth passes.
	 */
	void AddShadowMeshes(const FPrimitiveSceneProxy& Proxy, int32 ViewIndex, FMeshElementCollector& Collector, bool bReverseCulling) const;

	/** Splats per draw: as many as 16-bit indices reach, six vertices each. */
	static constexpr uint32 SplatsPerDraw = 10920;

	bool HasParameters() const { return UniformBuffer.IsValid() && NumSplats > 0; }

private:
	TUniformBufferRef<FVGSSplatShadowParameters> UniformBuffer;
	uint32 NumSplats = 0;

	friend class FVGSSplatShadowVertexFactoryShaderParameters;
};
