// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#include "VGSSplatRenderStats.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "SystemTextures.h"
#include "ProfilingDebugging/CsvProfiler.h"

DECLARE_DWORD_COUNTER_STAT(TEXT("Proxies"), STAT_VGSSplat_Proxies, STATGROUP_VGSSplat);
DECLARE_DWORD_COUNTER_STAT(TEXT("Clusters Submitted"), STAT_VGSSplat_ClustersSubmitted, STATGROUP_VGSSplat);
DECLARE_DWORD_COUNTER_STAT(TEXT("Clusters Visible"), STAT_VGSSplat_ClustersVisible, STATGROUP_VGSSplat);
DECLARE_DWORD_COUNTER_STAT(TEXT("Splats Submitted"), STAT_VGSSplat_SplatsSubmitted, STATGROUP_VGSSplat);
DECLARE_DWORD_COUNTER_STAT(TEXT("Splats Shaded"), STAT_VGSSplat_SplatsShaded, STATGROUP_VGSSplat);

CSV_DEFINE_CATEGORY(VGSSplat, false);

namespace VGSSplat
{
	void FRenderStats::UpdateRegistration()
	{
		const bool bWantSocket = LastFrameWithProxies != 0 && GFrameCounterRenderThread <= LastFrameWithProxies + 1;
		if (bWantSocket == StatsFeedbackSocket.GetMessageId().IsValid())
		{
			return;
		}

		if (bWantSocket)
		{
			StatsFeedbackSocket = GPUMessage::RegisterHandler(TEXT("VGSSplat.StatsFeedback"),
				[this](GPUMessage::FReader Message)
			{
				HandleMessage(Message);
			});
		}
		else
		{
			StatsFeedbackSocket.Reset();
			Totals = FTotals();
			Publish(Totals);
		}
	}

	void FRenderStats::SetShaderParameters(
		FRDGBuilder& GraphBuilder,
		uint32 NumClustersSubmitted,
		uint32 NumSplatsSubmitted,
		FRDGBufferRef ClusterCountBuffer,
		FVGSSplatStatsParameters& OutParameters)
	{
		LastFrameWithProxies = GFrameCounterRenderThread;

		if (StatsFeedbackSocket.GetMessageId().IsValid())
		{
			OutParameters.GPUMessageParams = GPUMessage::GetShaderParameters(GraphBuilder);
			OutParameters.StatsMessageId = StatsFeedbackSocket.GetMessageId().GetIndex();
		}
		else
		{
			FRDGBufferRef DummyMessageBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("VGSSplat.DummyMessageBuffer"));
			OutParameters.GPUMessageParams.GPUMessageDataBuffer = GraphBuilder.CreateUAV(DummyMessageBuffer);
			AddClearUAVPass(GraphBuilder, OutParameters.GPUMessageParams.GPUMessageDataBuffer, 0u);
			OutParameters.GPUMessageParams.GPUMessageDataBufferSize = 0;
			OutParameters.StatsMessageId = 0;
		}
		OutParameters.StatsFrameNumber = uint32(GFrameCounterRenderThread);
		OutParameters.StatsNumClustersSubmitted = NumClustersSubmitted;
		OutParameters.StatsNumSplatsSubmitted = NumSplatsSubmitted;
		OutParameters.StatsHasClusterCount = ClusterCountBuffer != nullptr ? 1u : 0u;
		OutParameters.StatsClusterCountBuffer = GraphBuilder.CreateSRV(
			ClusterCountBuffer != nullptr ? ClusterCountBuffer : GSystemTextures.GetDefaultBuffer(GraphBuilder, sizeof(uint32)), PF_R32_UINT);
	}

	void FRenderStats::HandleMessage(GPUMessage::FReader Message)
	{
		const uint32 MessageFrameNumber = Message.Read<uint32>(0);
		bool bPublishCompletedFrame = MessageFrameNumber != FrameNumber;
		if (bPublishCompletedFrame)
		{
			Publish(Totals);
			FrameNumber = MessageFrameNumber;
			Totals = FTotals();
		}

		Totals.NumProxies += 1;
		Totals.NumClustersSubmitted += Message.Read<uint32>(0);
		Totals.NumClustersVisible += Message.Read<uint32>(0);
		Totals.NumSplatsSubmitted += Message.Read<uint32>(0);
		Totals.NumSplatsShaded += Message.Read<uint32>(0);
	}

	void FRenderStats::Publish(const FTotals& InTotals)
	{
		SET_DWORD_STAT(STAT_VGSSplat_Proxies, InTotals.NumProxies);
		SET_DWORD_STAT(STAT_VGSSplat_ClustersSubmitted, InTotals.NumClustersSubmitted);
		SET_DWORD_STAT(STAT_VGSSplat_ClustersVisible, InTotals.NumClustersVisible);
		SET_DWORD_STAT(STAT_VGSSplat_SplatsSubmitted, InTotals.NumSplatsSubmitted);
		SET_DWORD_STAT(STAT_VGSSplat_SplatsShaded, InTotals.NumSplatsShaded);

		CSV_CUSTOM_STAT(VGSSplat, Proxies, int32(InTotals.NumProxies), ECsvCustomStatOp::Set);
		CSV_CUSTOM_STAT(VGSSplat, ClustersSubmitted, int32(InTotals.NumClustersSubmitted), ECsvCustomStatOp::Set);
		CSV_CUSTOM_STAT(VGSSplat, ClustersVisible, int32(InTotals.NumClustersVisible), ECsvCustomStatOp::Set);
		CSV_CUSTOM_STAT(VGSSplat, SplatsSubmitted, int32(InTotals.NumSplatsSubmitted), ECsvCustomStatOp::Set);
		CSV_CUSTOM_STAT(VGSSplat, SplatsShaded, int32(InTotals.NumSplatsShaded), ECsvCustomStatOp::Set);
	}
}
