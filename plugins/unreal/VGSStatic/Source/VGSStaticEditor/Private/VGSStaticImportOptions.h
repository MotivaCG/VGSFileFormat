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

	/**
	 * Make a shadow proxy: a hidden mesh of where the splats are opaque, kept in the asset.
	 * Components cast their shadows from it instead of from the splats (their Shadow Caster):
	 * from every light, as any mesh, at no cost per splat; they can also collide with it.
	 * Takes well under a second. The sizes below are for a PLY in metres (centimetres in
	 * Unreal at the component's Unit Scale of 100).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Shadow Proxy", meta = (DisplayName = "Make Shadow Proxy"))
	bool bMakeShadowProxy = true;

	/** The detail of the proxy: smaller follows fingers and hair better, and makes more triangles to reduce. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Shadow Proxy",
		meta = (DisplayName = "Voxel Size", ClampMin = "0.1", ClampMax = "20", Units = "cm", EditCondition = "bMakeShadowProxy"))
	float ProxyVoxelSize = 1.0f;

	/**
	 * How opaque the splats must make a voxel for it to be inside the proxy: lower takes in
	 * more of the hair's and the edges' haze (larger shadows), higher less. Lower also lets
	 * the proxy stand out in front of what is lit (haze before a face, a straw), which then
	 * shadows the character itself in blotches, seen up close under local lights.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Shadow Proxy",
		meta = (DisplayName = "Opacity", ClampMin = "0.01", ClampMax = "0.99", EditCondition = "bMakeShadowProxy"))
	float ProxyOpacity = 0.6f;

	/**
	 * How far the proxy lies inside the splats. The lit splats would shadow themselves in
	 * specks (virtual shadow maps compare almost per pixel) if they were close to their own
	 * shadow caster: 2 cm still left specks on a dress, 4 cm did not. Never more than 45 % of
	 * how thick the character is behind each point, so a finger or a shoe's edge keeps a
	 * thinner proxy instead of turning inside out.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Shadow Proxy",
		meta = (DisplayName = "Inset", ClampMin = "0", ClampMax = "20", Units = "cm", EditCondition = "bMakeShadowProxy"))
	float ProxyInset = 4.0f;

	/** The triangles the proxy is reduced to (it is first made with several times more). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, config, Category = "Shadow Proxy",
		meta = (DisplayName = "Triangles", ClampMin = "500", EditCondition = "bMakeShadowProxy"))
	int32 ProxyTriangles = 20000;
};
