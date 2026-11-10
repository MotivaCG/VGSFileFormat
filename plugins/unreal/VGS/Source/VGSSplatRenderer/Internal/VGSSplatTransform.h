// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Math/Matrix.h"

class UGaussianSplatComponent;

namespace VGSSplat
{
	/** Game-thread local-to-world matrix shared by proxy creation and per-frame updates. */
	VGSSPLATRENDERER_API FMatrix44f GetComponentLocalToWorld(const UGaussianSplatComponent& Component);
}
