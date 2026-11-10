// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Mirrored by FVGSSplatCluster in VGSSplatCommon.ush, keep the two in sync.
struct FVGSSplatCluster
{
	FVector3f	BoundsMin;
	uint32		SplatOffset;
	FVector3f	BoundsMax;
	uint32		SplatCount;
	uint32		Level;

	float		Error;
	uint32		ParentIndex;
	uint32		Padding;
};

static_assert(sizeof(FVGSSplatCluster) == 48, "FVGSSplatCluster must stay three float4s");
