#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "VGSStaticSplat.h"

#include "VGSStaticImportOptions.generated.h"

/**
 * What the import dialog asks, and what a reimport repeats. A script imports with its own by
 * setting an AssetImportTask's options to one of these.
 */
UCLASS(BlueprintType, config = EditorPerProjectUserSettings)
class UVGSStaticImportOptions : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Work out normals when the PLY has none (nx, ny, nz missing or all 0), so Shaded lighting
	 * follows the surface. A PLY from SMNForge or the VGS editor has its own, from the capture's
	 * cameras, and keeps them. Without any each splat is lit by its shortest axis on its own,
	 * which is much noisier. Takes a second or so for a few hundred thousand splats.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Normals")
	bool bEstimateNormals = true;

	/** Work them out even when the PLY has its own (to replace normals made some other way). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Normals", meta = (EditCondition = "bEstimateNormals"))
	bool bReplaceFileNormals = false;

	/**
	 * How. A studio rig's cameras decide each splat's outward side, but only for a PLY from that
	 * rig in its own frame (SMNForge names its rig in the file: Auto uses it); if they see less
	 * than half of the splats the import uses cameras all around instead, and says so. Cameras
	 * all around (the default without a rig) suit a PLY from anywhere; shortest axes (as PGSR)
	 * are rougher and leave the side to the renderer.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Normals", meta = (EditCondition = "bEstimateNormals"))
	EVGSStaticNormalMethod Method = EVGSStaticNormalMethod::Auto;

	/** Shortest axes: the nearest splats a round or stray splat takes its neighbours' direction from. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Normals", meta = (ClampMin = "4", ClampMax = "64", EditCondition = "bEstimateNormals"))
	int32 Neighbours = 16;

	/** Shortest axes: ... no farther than this share of the splats' size (their bounds' diagonal). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Normals", meta = (ClampMin = "0.05", ClampMax = "10", Units = "Percent", EditCondition = "bEstimateNormals"))
	float RadiusPercent = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Splats", meta = (DisplayName = "GPU Layout"))
	EVGSStaticGpuLayout GpuLayout = EVGSStaticGpuLayout::Float;
};
