// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VGSSplatDefinitions.h"

#include "VGSSplatCommon.generated.h"

// Coordinate space used for SH evaluation.
UENUM()
enum class EVGSSplatCoordinateSpace : uint8
{
	// Unreal's native space (Left Handed, X=Forward, Y=Right, Z=Up)
	LH_FRU UMETA(DisplayName = "Unreal Native (LH X=Forward Y=Right Z=Up)"),

	// Right Handed, X=Right, Y=Up, Z=Back. Generally from SPZ splat files.
	RH_RUB UMETA(DisplayName = "Right Handed - X=Right Y=Up Z=Back"),

	// Right Handed, X=Right, Y=Down, Z=Forward. Generally from COLMAP PLY splat files.
	RH_RDF UMETA(DisplayName = "Right Handed - X=Right Y=Down Z=Forward"),
};

// Splat cluster grouping
UENUM()
enum class EVGSSplatClusterGrouping : uint8
{
	// Disabled
	None UMETA(DisplayName = "None"),

	// Sequential grouping
	Sequential UMETA(DisplayName = "Sequential"),
};

UENUM()
enum class EVGSSplatAggregation : uint8
{
	// Unweighted average
	Average UMETA(DisplayName = "Average"),

	// Moment preserving, weighted by opacity and footprint.
	MomentPreserving UMETA(DisplayName = "Moment Preserving"),
};

namespace VGSSplatCommon
{
	static constexpr int32 MaxSupportedSHDegree = 3;
	static constexpr float SplatBoundsScale = UE_SQRT_2 * GAUSSIAN_SPLAT_RADIUS;
	static constexpr int32 MinSplatsPerCluster = 16;

	inline int32 GetSHDimension(int32 SHDegree)
	{
		check(SHDegree >= 0 && SHDegree <= MaxSupportedSHDegree);
		return (SHDegree + 1) * (SHDegree + 1);
	}
}
