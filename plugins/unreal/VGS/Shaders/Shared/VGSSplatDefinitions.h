// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#define GAUSSIAN_SPLAT_RADIUS				2.0f
#define GAUSSIAN_SPLAT_RADIUS_SQUARED		(GAUSSIAN_SPLAT_RADIUS * GAUSSIAN_SPLAT_RADIUS)
#define GAUSSIAN_SPLAT_COVARIANCE_DILATION	0.3f
#define GAUSSIAN_SPLAT_MAX_SIZE_PIXELS		4096.0f
#define GAUSSIAN_SPLAT_MIN_TRANSMITTANCE	1.0e-4f
