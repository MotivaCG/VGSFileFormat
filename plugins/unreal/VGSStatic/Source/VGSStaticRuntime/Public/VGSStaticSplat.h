#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "VGSStaticSplat.generated.h"

class UAssetImportData;
class UStaticMesh;
class FVGSStaticSplatGpu;

/** Where an asset's normals come from. */
UENUM(BlueprintType)
enum class EVGSStaticNormals : uint8
{
	/** None: lit by each splat's shortest axis, turned to the camera, which is much noisier. */
	None,
	/** The PLY's own nx, ny, nz (SMNForge's and the VGS editor's are outward, from the capture's cameras). */
	File UMETA(DisplayName = "From the file"),
	/** Worked out at import with a studio rig's cameras, which decide each splat's outward side. */
	Rig UMETA(DisplayName = "Estimated with the rig"),
	/**
	 * Worked out at import as PGSR takes them, each splat's shortest axis, corrected by its
	 * neighbours where it is round or disagrees with them; the side is decided on screen,
	 * facing the camera.
	 */
	Axes UMETA(DisplayName = "Shortest axes (PGSR)"),
	/**
	 * Worked out at import as with a rig, but with cameras spread all around the splats: for a
	 * PLY no studio rig of ours captured. Outward, as a rig's.
	 */
	Surrounding UMETA(DisplayName = "Estimated with cameras all around"),
};

/** How normals are worked out for a PLY that has none. */
UENUM(BlueprintType)
enum class EVGSStaticNormalMethod : uint8
{
	/** With the rig the PLY names (SMNForge writes "comment capture_rig"), or else cameras all around. */
	Auto UMETA(DisplayName = "Rig from the file, else cameras all around"),
	/** The ScanMeNow rig's cameras: for its PLYs, in their own frame (Y up, metres). */
	SmnV1 UMETA(DisplayName = "SMNV1 rig"),
	/** The THE4DSCANNER rig's cameras, likewise. */
	The4DScanner UMETA(DisplayName = "THE4DSCANNER rig"),
	/** Each splat's shortest axis, as PGSR (for a PLY from anywhere): no rig needed. */
	ShortestAxes UMETA(DisplayName = "Shortest axes (PGSR)"),
	/**
	 * The rig estimate with cameras spread evenly around the splats instead of a rig's: for a PLY
	 * from anywhere, and the best of these without a rig (closer to the rig's own normals than
	 * the shortest axes, and outward).
	 */
	Surrounding UMETA(DisplayName = "Cameras all around"),
};

/** How the splats are kept on the GPU. */
UENUM(BlueprintType)
enum class EVGSStaticGpuLayout : uint8
{
	/** Every value as imported: 56 bytes a splat plus 12 per harmonic coefficient. */
	Float,
	/**
	 * Epic's compressed layout: about a third of the memory, a little more work per frame, and
	 * rotations a few degrees off on the flattest splats. Normals are kept apart, unchanged.
	 */
	Quantized,
};

/** What the asset keeps, immutable once made: shared by the asset and the GPU copies made from it. */
struct FVGSStaticSplatData
{
	uint32 NumSplats = 0;
	int32 SHCoefficients = 0; // 0, 3, 8 or 15
	uint32 Stride = 0; // of the float records
	TArray<uint8> Records; // the renderer's float layout, in cluster order
	TArray<uint32> Normals; // one word per splat (0 none), or empty
	TArray<uint8> Clusters; // 48 bytes each (VGSStaticSplatCluster.h)
	FBox3f Bounds = FBox3f(ForceInit); // splat centres, in the file's axes and units
};

/**
 * A static Gaussian splat character, imported from a PLY: its splats in the renderer's
 * layout, ordered into clusters the renderer culls by, and their normals when the file has
 * them or they were estimated at import. Placed in a level with a VGS Static Splat actor or
 * component, which says how it is drawn (exposure, grade, lighting).
 *
 * One GPU copy serves every component that shows it.
 *
 * It also keeps its shadow proxy, made at import: a hidden mesh of where the splats are
 * opaque, which components cast shadows from (and can collide with) instead of the splats.
 */
UCLASS(BlueprintType, hidecategories = Object)
class VGSSTATICRUNTIME_API UVGSStaticSplat : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Splats")
	int32 NumSplats = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Splats", meta = (DisplayName = "SH Degree"))
	int32 SHDegree = 0;

	/** Groups of neighbouring splats the renderer culls against the view as one. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Splats")
	int32 NumClusters = 0;

	/** Splat centres, in the file's axes and units (usually metres). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Splats")
	FBox3f Bounds = FBox3f(ForceInit);

	/** Splats left out of the file's: those with a position, scale or rotation that is not a number. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Splats")
	int32 DroppedSplats = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Splats", meta = (DisplayName = "GPU Layout"))
	EVGSStaticGpuLayout GpuLayout = EVGSStaticGpuLayout::Float;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Normals")
	EVGSStaticNormals Normals = EVGSStaticNormals::None;

	/** How the normals came to be, in words. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Normals", meta = (DisplayName = "How"))
	FString NormalsNote;

	/**
	 * A closed mesh of where the splats are opaque, made at import, a little inside them (in
	 * this asset's package). Components cast their shadows from it, from every light as any
	 * mesh does, and can collide with it (their Shadow Caster and Proxy Collision); it is never
	 * drawn. Its triangles are reduced when it is built: Reduction Settings in its own editor.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Shadow Proxy", meta = (DisplayName = "Mesh"))
	TObjectPtr<UStaticMesh> ShadowProxy;

	/** How the proxy was made, in words. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Shadow Proxy", meta = (DisplayName = "How"))
	FString ShadowProxyNote;

#if WITH_EDITORONLY_DATA
	UPROPERTY(VisibleAnywhere, Instanced, Category = "Import Settings")
	TObjectPtr<UAssetImportData> AssetImportData;

	/** Work out normals when the PLY has none (reimport does the same). */
	UPROPERTY(EditAnywhere, Category = "Import Settings")
	bool bEstimateNormals = true;

	/** Work them out even when the PLY has its own. */
	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (EditCondition = "bEstimateNormals"))
	bool bReplaceFileNormals = false;

	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (EditCondition = "bEstimateNormals"))
	EVGSStaticNormalMethod NormalMethod = EVGSStaticNormalMethod::Auto;

	/** Shortest axes: the nearest splats a round or stray splat takes its neighbours' direction from. */
	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (ClampMin = "4", ClampMax = "64", EditCondition = "bEstimateNormals"))
	int32 NormalNeighbours = 16;

	/** Shortest axes: ... no farther than this share of the splats' size (their bounds' diagonal). */
	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (ClampMin = "0.05", ClampMax = "10", Units = "Percent", EditCondition = "bEstimateNormals"))
	float NormalRadiusPercent = 1.0f;

	/** Make the shadow proxy mesh (reimport does the same). The sizes below are for a PLY in metres. */
	UPROPERTY(EditAnywhere, Category = "Import Settings")
	bool bMakeShadowProxy = true;

	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (DisplayName = "Proxy Voxel Size", ClampMin = "0.1", ClampMax = "20", Units = "cm", EditCondition = "bMakeShadowProxy"))
	float ProxyVoxelSize = 1.0f;

	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (DisplayName = "Proxy Opacity", ClampMin = "0.01", ClampMax = "0.99", EditCondition = "bMakeShadowProxy"))
	float ProxyOpacity = 0.6f;

	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (DisplayName = "Proxy Inset", ClampMin = "0", ClampMax = "20", Units = "cm", EditCondition = "bMakeShadowProxy"))
	float ProxyInset = 4.0f;

	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (DisplayName = "Proxy Triangles", ClampMin = "500", EditCondition = "bMakeShadowProxy"))
	int32 ProxyTriangles = 20000;
#endif

	/** The splats, or null for an asset with none. */
	TSharedPtr<const FVGSStaticSplatData, ESPMode::ThreadSafe> GetData() const { return Data; }

	/** The GPU copy every component showing this asset shares; made on first use, remade when the data or layout changes. */
	TSharedPtr<FVGSStaticSplatGpu, ESPMode::ThreadSafe> GetGpu();

	/** Changed data or layout: components ask for the GPU copy again. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnChanged, UVGSStaticSplat*);
	static FOnChanged OnChanged;

	void SetData(TSharedPtr<const FVGSStaticSplatData, ESPMode::ThreadSafe> InData);

	//~ UObject
	virtual void PostInitProperties() override;
	virtual void Serialize(FArchive& Ar) override;
	virtual void GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
#if WITH_EDITORONLY_DATA
	virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;
#endif

private:
	TSharedPtr<const FVGSStaticSplatData, ESPMode::ThreadSafe> Data;
	TSharedPtr<FVGSStaticSplatGpu, ESPMode::ThreadSafe> Gpu;
};
