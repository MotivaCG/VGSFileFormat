// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GPUMessaging.h"
#include "RenderGraphFwd.h"
#include "ShaderParameterMacros.h"
#include "Stats/Stats.h"

DECLARE_STATS_GROUP(TEXT("GaussianSplat"), STATGROUP_VGSSplat, STATCAT_Advanced);

BEGIN_SHADER_PARAMETER_STRUCT(FVGSSplatStatsParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(GPUMessage::FParameters, GPUMessageParams)
	SHADER_PARAMETER(uint32, StatsMessageId)
	SHADER_PARAMETER(uint32, StatsFrameNumber)
	SHADER_PARAMETER(uint32, StatsNumClustersSubmitted)
	SHADER_PARAMETER(uint32, StatsNumSplatsSubmitted)
	SHADER_PARAMETER(uint32, StatsHasClusterCount)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, StatsClusterCountBuffer)
END_SHADER_PARAMETER_STRUCT()

namespace VGSSplat
{
	class FRenderStats
	{
	public:
		void UpdateRegistration();

		void SetShaderParameters(
			FRDGBuilder& GraphBuilder,
			uint32 NumClustersSubmitted,
			uint32 NumSplatsSubmitted,
			FRDGBufferRef ClusterCountBuffer,
			FVGSSplatStatsParameters& OutParameters);

	private:
		struct FTotals
		{
			uint32 NumProxies = 0;
			uint32 NumClustersSubmitted = 0;
			uint32 NumClustersVisible = 0;
			uint32 NumSplatsSubmitted = 0;
			uint32 NumSplatsShaded = 0;
		};

		void HandleMessage(GPUMessage::FReader Message);
		void Publish(const FTotals& InTotals);

		uint32 FrameNumber = 0;
		FTotals Totals;
		uint64 LastFrameWithProxies = 0;

		GPUMessage::FSocket StatsFeedbackSocket;
	};
}
