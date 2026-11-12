// VGS: not Epic's. See VGSSplatShadowVertexFactory.h.
#include "VGSSplatShadowVertexFactory.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialDomain.h"
#include "MeshDrawShaderBindings.h"
#include "MeshMaterialShader.h"
#include "PrimitiveSceneProxy.h"
#include "RenderUtils.h"
#include "RHIResourceUtils.h"
#include "Runtime/Launch/Resources/Version.h"
#include "SceneManagement.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "VGSSplatLightShadow.h"

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FVGSSplatShadowParameters, "VGSSplatShadow");

static TAutoConsoleVariable<int32> CVarVGSSplatShadowMethod(
	TEXT("r.VGSSplat.Shadow.Method"),
	0,
	TEXT("How splats cast shadows (the project's settings set it).\n")
	TEXT("0: the splat renderer's own shadow map of the main directional light, composited onto the lit scene (default).\n")
	TEXT("1: that for the main directional light, and Unreal's shadow maps (as 2) for every other light.\n")
	TEXT("2: in Unreal's shadow maps, for every light: each splat an opaque octahedron where its opacity reaches a cutoff of its own, drawn at random.\n")
	TEXT("3: none."),
	ECVF_RenderThreadSafe);

VGSSplat::EShadowMethod VGSSplat::GetShadowMethod()
{
	return EShadowMethod(FMath::Clamp(CVarVGSSplatShadowMethod.GetValueOnAnyThread(), 0, 3));
}

namespace
{
	/**
	 * The index buffer every draw shares: SplatsPerDraw octahedra of six vertices (vertex
	 * 2a + s is the end of axis a, s 0 the + end and 1 the - end) and eight faces, one per
	 * octant, wound outwards for a right-handed frame of axes.
	 */
	class FVGSSplatShadowIndexBuffer : public FIndexBuffer
	{
	public:
		virtual void InitRHI(FRHICommandListBase& RHICmdList) override
		{
			TArray<uint16> Indices;
			Indices.Reserve(FVGSSplatShadowVertexFactory::SplatsPerDraw * 24);
			for (uint32 Splat = 0; Splat < FVGSSplatShadowVertexFactory::SplatsPerDraw; ++Splat)
			{
				const uint32 Base = Splat * 6;
				for (uint32 Octant = 0; Octant < 8; ++Octant)
				{
					const uint32 SX = Octant & 1, SY = (Octant >> 1) & 1, SZ = (Octant >> 2) & 1;
					const uint16 A = uint16(Base + 0 + SX), B = uint16(Base + 2 + SY), C = uint16(Base + 4 + SZ);
					// (+x, +y, +z) wound A B C faces outwards; each axis flipped mirrors it.
					if (((SX + SY + SZ) & 1) == 0)
					{
						Indices.Append({ A, B, C });
					}
					else
					{
						Indices.Append({ A, C, B });
					}
				}
			}
			IndexBufferRHI = UE::RHIResourceUtils::CreateIndexBufferFromArray(RHICmdList, TEXT("VGSSplatShadow.Indices"), EBufferUsageFlags::Static, MakeConstArrayView(Indices));
		}
	};
	TGlobalResource<FVGSSplatShadowIndexBuffer> GVGSSplatShadowIndexBuffer;

	/** Bound as the normals when there are none: one zero word. */
	class FVGSSplatShadowNoNormals : public FRenderResource
	{
	public:
		FBufferRHIRef Buffer;
		FShaderResourceViewRHIRef SRV;

		virtual void InitRHI(FRHICommandListBase& RHICmdList) override
		{
			const uint32 Zero[4] = {};
			Buffer = UE::RHIResourceUtils::CreateBufferFromArray(RHICmdList, TEXT("VGSSplatShadow.NoNormals"),
				EBufferUsageFlags::Static | EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::ShaderResource, sizeof(uint32), Zero, sizeof(Zero));
			SRV = RHICmdList.CreateShaderResourceView(Buffer, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(Buffer));
		}
		virtual void ReleaseRHI() override
		{
			SRV.SafeRelease();
			Buffer.SafeRelease();
		}
	};
	TGlobalResource<FVGSSplatShadowNoNormals> GVGSSplatShadowNoNormals;
}

/** Binds the parameters, and where each draw's batch of splats starts. */
class FVGSSplatShadowVertexFactoryShaderParameters : public FVertexFactoryShaderParameters
{
	DECLARE_TYPE_LAYOUT(FVGSSplatShadowVertexFactoryShaderParameters, NonVirtual);

public:
	void Bind(const FShaderParameterMap& ParameterMap)
	{
		FirstSplat.Bind(ParameterMap, TEXT("VGSShadowFirstSplat"));
	}

	void GetElementShaderBindings(
		const FSceneInterface* Scene,
		const FSceneView* View,
		const FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* InVertexFactory,
		const FMeshBatchElement& BatchElement,
		FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams) const
	{
		const FVGSSplatShadowVertexFactory* VertexFactory = static_cast<const FVGSSplatShadowVertexFactory*>(InVertexFactory);
		ShaderBindings.Add(Shader->GetUniformBufferParameter<FVGSSplatShadowParameters>(), VertexFactory->UniformBuffer);
		ShaderBindings.Add(FirstSplat, uint32(BatchElement.UserIndex));
	}

private:
	LAYOUT_FIELD(FShaderParameter, FirstSplat);
};

IMPLEMENT_TYPE_LAYOUT(FVGSSplatShadowVertexFactoryShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FVGSSplatShadowVertexFactory, SF_Vertex, FVGSSplatShadowVertexFactoryShaderParameters);

FVGSSplatShadowVertexFactory::FVGSSplatShadowVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
	: FVertexFactory(InFeatureLevel)
{
}

void FVGSSplatShadowVertexFactory::InitRHI(FRHICommandListBase& RHICmdList)
{
	// No vertex data: everything is read from the splat buffer by vertex index. Only the
	// stream of GPU Scene instance ids, which the engine supplies.
	FVertexStream NullStream;
	NullStream.VertexBuffer = nullptr;
	NullStream.Stride = 0;
	NullStream.Offset = 0;
	NullStream.VertexStreamUsage = EVertexStreamUsage::ManualFetch;
	check(Streams.Num() == 0);
	Streams.Add(NullStream);

	FVertexDeclarationElementList Elements;
#if ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 9
	AddPrimitiveIdStreamElement(EVertexInputStreamType::Default, Elements, 13);
#else
	AddPrimitiveIdStreamElement(EVertexInputStreamType::Default, Elements, 13, 13);
#endif
	InitDeclaration(Elements);
}

void FVGSSplatShadowVertexFactory::ReleaseRHI()
{
	UniformBuffer.SafeRelease();
	FVertexFactory::ReleaseRHI();
}

bool FVGSSplatShadowVertexFactory::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
	// Drawn with the default material only, on desktop shader models.
	return Parameters.MaterialParameters.bIsSpecialEngineMaterial
		&& Parameters.MaterialParameters.MaterialDomain == MD_Surface
		&& IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5)
		&& IsPCPlatform(Parameters.Platform);
}

void FVGSSplatShadowVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);
#if ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 9
	const bool bSceneData = Parameters.VertexFactoryType->SupportsGPUScene(GetMaxSupportedFeatureLevel(Parameters.Platform));
#else
	const bool bSceneData = Parameters.VertexFactoryType->SupportsPrimitiveIdStream() && UseGPUScene(Parameters.Platform);
#endif
	OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), bSceneData);
}

void FVGSSplatShadowVertexFactory::SetParameters(FRHICommandListBase& RHICmdList, const FVGSSplatShadowParameters& Parameters)
{
	NumSplats = Parameters.NumSplats;
	FVGSSplatShadowParameters Bound = Parameters;
	if (!Bound.Normals)
	{
		Bound.Normals = GVGSSplatShadowNoNormals.SRV;
		Bound.HasNormals = 0;
	}
	if (UniformBuffer.IsValid())
	{
		// In place, in order with the commands already recorded: draws of earlier frames keep
		// what they were given, and a capture that plays changes this every frame.
		UniformBuffer.UpdateUniformBufferImmediate(RHICmdList, Bound);
	}
	else
	{
		UniformBuffer = TUniformBufferRef<FVGSSplatShadowParameters>::CreateUniformBufferImmediate(Bound, UniformBuffer_MultiFrame);
	}
}

void FVGSSplatShadowVertexFactory::ClearSplats()
{
	NumSplats = 0;
}

void FVGSSplatShadowVertexFactory::AddShadowMeshes(const FPrimitiveSceneProxy& Proxy, const FSceneView& View, int32 ViewIndex, FMeshElementCollector& Collector, bool bReverseCulling) const
{
	const VGSSplat::EShadowMethod Method = VGSSplat::GetShadowMethod();
	if (!HasParameters() || (Method != VGSSplat::EShadowMethod::UnrealShadowMaps && Method != VGSSplat::EShadowMethod::Mixed))
	{
		return;
	}
	if (Method == VGSSplat::EShadowMethod::Mixed && View.Family && View.Family->Scene && View.Family->Scene->GetRenderScene())
	{
		// A shadow depth view looks along its light (FProjectedShadowInfo::ModifyViewForShadow
		// turns it so, for billboards): the main directional light's views are the ones looking
		// along its direction, which the splats' own shadow map covers.
		const FLightSceneInfo* MainLight = VGSSplat::FindMainLight(*View.Family->Scene->GetRenderScene());
		if (MainLight && FMath::Abs(FVector::DotProduct(View.GetViewDirection(), VGSSplat::GetMainLightDirection(*MainLight))) > 0.99999)
		{
			return;
		}
	}
	const FMaterialRenderProxy* Material = UMaterial::GetDefaultMaterial(MD_Surface)->GetRenderProxy();
	for (uint32 First = 0; First < NumSplats; First += SplatsPerDraw)
	{
		const uint32 Count = FMath::Min(SplatsPerDraw, NumSplats - First);
		FMeshBatch& Mesh = Collector.AllocateMesh();
		Mesh.VertexFactory = this;
		Mesh.MaterialRenderProxy = Material;
		Mesh.Type = PT_TriangleList;
		Mesh.CastShadow = true;
		Mesh.bUseAsOccluder = false;
		Mesh.bUseForDepthPass = false;
		Mesh.bUseForMaterial = false; // shadows only: the splat renderer draws them in the view
		Mesh.ReverseCulling = bReverseCulling;
		Mesh.bCanApplyViewModeOverrides = false;
		Mesh.DepthPriorityGroup = SDPG_World;

		FMeshBatchElement& Element = Mesh.Elements[0];
		Element.IndexBuffer = &GVGSSplatShadowIndexBuffer;
		Element.FirstIndex = 0;
		Element.NumPrimitives = Count * 8;
		Element.MinVertexIndex = 0;
		Element.MaxVertexIndex = Count * 6 - 1;
		Element.NumInstances = 1;
		Element.UserIndex = int32(First);
		Element.PrimitiveUniformBuffer = Proxy.GetUniformBuffer();
		Collector.AddMesh(ViewIndex, Mesh);
	}
}

// GPU Scene, so that virtual shadow maps draw the splats (they take only such vertex factories).
#if ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 9
static constexpr EVertexFactoryFlags GVGSSplatShadowGPUSceneFlag = EVertexFactoryFlags::SupportsGPUSceneNonMobile;
#else
static constexpr EVertexFactoryFlags GVGSSplatShadowGPUSceneFlag = EVertexFactoryFlags::SupportsPrimitiveIdStream;
#endif

IMPLEMENT_VERTEX_FACTORY_TYPE(FVGSSplatShadowVertexFactory, "/Plugin/VGS/Private/SplatRenderer/VGSSplatShadowVertexFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
	| GVGSSplatShadowGPUSceneFlag
);
