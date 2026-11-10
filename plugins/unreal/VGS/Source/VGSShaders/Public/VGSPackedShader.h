#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

/**
 * Evaluates a packed VGS chunk at one instant into the float records Epic's Gaussian Splat
 * renderer reads (Shaders/Private/VGSPacked.usf says how). One thread per record.
 */
class FVGSPackedEvaluateCS : public FGlobalShader
{
	DECLARE_EXPORTED_GLOBAL_SHADER(FVGSPackedEvaluateCS, VGSSHADERS_API);
	SHADER_USE_PARAMETER_STRUCT(FVGSPackedEvaluateCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, SplatA)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, SplatB)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, SplatC)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ShStatic)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ShTemporal)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, PositionTerms)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RotationTerms)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, PositionSamples)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RotationSamples)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Books)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float2>, Luts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Basis)
		SHADER_PARAMETER(uint32, Splats)
		SHADER_PARAMETER(uint32, Samples)
		SHADER_PARAMETER(uint32, Frame)
		SHADER_PARAMETER(uint32, GroupCount)
		SHADER_PARAMETER(uint32, ShPlanes)
		SHADER_PARAMETER(uint32, ShCoefficients)
		SHADER_PARAMETER(uint32, EntriesStatic)
		SHADER_PARAMETER(uint32, EntriesTemporal)
		SHADER_PARAMETER(uint32, Sh0Stride)
		SHADER_PARAMETER(uint32, OpacityStride)
		SHADER_PARAMETER(uint32, BooksTemporal)
		SHADER_PARAMETER(FIntVector4, BasisOffset)
		SHADER_PARAMETER(float, Alpha)
		SHADER_PARAMETER(float, Normalized)
		SHADER_PARAMETER_ARRAY(FVector4f, GroupA, [8])
		SHADER_PARAMETER_ARRAY(FVector4f, GroupB, [8])
		SHADER_PARAMETER(float, Gain)
		SHADER_PARAMETER(float, DcOffset)
		SHADER_PARAMETER(uint32, Thinned)
		SHADER_PARAMETER(uint32, DensityThreshold)
		SHADER_PARAMETER(float, DensityExponent)
		SHADER_PARAMETER(uint32, SplatStride)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, RWSplatBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, Normals)
		SHADER_PARAMETER(uint32, HasNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWNormalBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 64;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};
