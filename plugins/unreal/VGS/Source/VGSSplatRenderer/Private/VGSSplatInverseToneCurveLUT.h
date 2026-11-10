// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

// Log category for the inverse-tone-curve feature.
DECLARE_LOG_CATEGORY_EXTERN(LogVGSSplat, Log, All);

namespace VGSSplat
{
	// Selects which baked inverse-tone-curve LUT to sample; cast from bInvertExpandGamut for now.
	enum class EInverseToneCurveLUTVariant : uint8
	{
		/** Inverse filmic tone curve only (Epic's asset UE_InverseFilmicTC; Resources/InverseToneCurve). */
		Filmic = 0,
		/** Inverse filmic tone curve plus gamut expansion (UE_InverseFilmicTC_Full). */
		FilmicExpandGamut = 1,
	};
}
