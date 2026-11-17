#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SGSSplat.h"

#include "SGSImportOptions.generated.h"

/**
 * What the import dialog asks, and what a reimport repeats. A script imports with its own by
 * setting an AssetImportTask's options to one of these.
 */
UCLASS(BlueprintType, config = EditorPerProjectUserSettings)
class USGSImportOptions : public UObject
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
	 * How, as the VGS editor's Normals: cameras all around the splats decide each one's outward
	 * side, for a PLY from any rig or none (Normal, or Fine: finer and about 2.3 times slower);
	 * shortest axes (as PGSR) are rougher and leave the side to the renderer.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Normals", meta = (EditCondition = "bEstimateNormals"))
	ESGSNormalMethod Method = ESGSNormalMethod::Fine;

	/** Shortest axes: the nearest splats a round or stray splat takes its neighbours' direction from. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Normals", meta = (ClampMin = "4", ClampMax = "64", EditCondition = "bEstimateNormals && Method == ESGSNormalMethod::ShortestAxes"))
	int32 Neighbours = 16;

	/** Shortest axes: ... no farther than this share of the splats' size (their bounds' diagonal). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Normals", meta = (ClampMin = "0.05", ClampMax = "10", Units = "Percent", EditCondition = "bEstimateNormals && Method == ESGSNormalMethod::ShortestAxes"))
	float RadiusPercent = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Splats", meta = (DisplayName = "GPU Layout"))
	ESGSGpuLayout GpuLayout = ESGSGpuLayout::Float;

	/** Import the SGS mesh or generate an approximate occupancy mesh for optional collision.
	 * It never casts shadows. Sizes assume source units in metres.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Collision Proxy", meta = (DisplayName = "Make Collision Proxy"))
	bool bMakeShadowProxy = true;

	/** The SGS contains its authored shadow/collision mesh; it is kept instead of rebuilding it. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Collision Proxy", meta = (DisplayName = "Embedded SGS Proxy"))
	bool bHasEmbeddedProxy = false;

	/** The detail of the proxy: smaller follows fingers and hair better, and makes more triangles to reduce. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Collision Proxy",
		meta = (DisplayName = "Voxel Size", ClampMin = "0.1", ClampMax = "20", Units = "cm", EditCondition = "bMakeShadowProxy && !bHasEmbeddedProxy"))
	float ProxyVoxelSize = 1.0f;

	/** Occupancy threshold: lower includes more faint splats in the collision mesh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Collision Proxy",
		meta = (DisplayName = "Opacity", ClampMin = "0.01", ClampMax = "0.99", EditCondition = "bMakeShadowProxy && !bHasEmbeddedProxy"))
	float ProxyOpacity = 0.6f;

	/** How far the collision mesh is inset inside the splats, bounded by local thickness. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Collision Proxy",
		meta = (DisplayName = "Inset", ClampMin = "0", ClampMax = "20", Units = "cm", EditCondition = "bMakeShadowProxy && !bHasEmbeddedProxy"))
	float ProxyInset = 4.0f;

	/** The triangles the proxy is reduced to (it is first made with several times more). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Collision Proxy",
		meta = (DisplayName = "Triangles", ClampMin = "500", EditCondition = "bMakeShadowProxy && !bHasEmbeddedProxy"))
	int32 ProxyTriangles = 20000;
};
