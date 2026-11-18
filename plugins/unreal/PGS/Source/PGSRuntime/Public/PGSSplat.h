#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "PGSSplat.generated.h"

class UAssetImportData;
class UStaticMesh;
class FPGSSplatGpu;

/** Where an asset's normals come from. */
UENUM(BlueprintType)
enum class EPGSNormals : uint8
{
	/** None: lit by each splat's shortest axis, turned to the camera, which is much noisier. */
	None,
	/** The PLY's own nx, ny, nz (SMNForge's and the VGS editor's are outward, from the capture's cameras). */
	File UMETA(DisplayName = "From the file"),
	/** Worked out with a studio rig's cameras, by imports before cameras all around replaced them. */
	Rig UMETA(DisplayName = "Estimated with the rig"),
	/**
	 * Worked out at import as PGSR takes them, each splat's shortest axis, corrected by its
	 * neighbours where it is round or disagrees with them; the side is decided on screen,
	 * facing the camera.
	 */
	Axes UMETA(DisplayName = "Shortest axes (PGSR)"),
	/**
	 * Worked out at import as the VGS editor does, with cameras spread all around the splats
	 * deciding each one's outward side (Normal or Fine: the asset's Normal Method).
	 */
	Surrounding UMETA(DisplayName = "Estimated with cameras all around"),
};

/** How normals are worked out for a PLY that has none. */
UENUM(BlueprintType)
enum class EPGSNormalMethod : uint8
{
	/**
	 * The VGS editor's Normal: 256 cameras all around the splats decide each one's outward side,
	 * whichever rig captured it and wherever it stands. About 1.3 s for 300 000 splats.
	 */
	Normal UMETA(DisplayName = "Normal (cameras all around)"),
	/** The VGS editor's Fine: the same, seeing finer; the side wrong on fewer splats. About 3 s. */
	Fine UMETA(DisplayName = "Fine (cameras all around, finer)"),
	/** Each splat's shortest axis, as PGSR: rougher, and the side is left to the renderer. */
	ShortestAxes UMETA(DisplayName = "Shortest axes (PGSR)"),
};

/** How the splats are kept on the GPU. */
UENUM(BlueprintType)
enum class EPGSGpuLayout : uint8
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
struct FPGSSplatData
{
	uint32 NumSplats = 0;
	int32 SHCoefficients = 0; // 0, 3, 8 or 15
	uint32 Stride = 0; // of the float records
	TArray<uint8> Records; // the renderer's float layout, in cluster order
	TArray<uint32> Normals; // one word per splat (0 none), or empty
	TArray<uint8> Clusters; // 48 bytes each (PGSSplatCluster.h)
	FBox3f Bounds = FBox3f(ForceInit); // splat centres, in the file's axes and units
};

/**
 * A static Gaussian splat character, imported from a PLY or PGS: its splats in the renderer's
 * layout, ordered into clusters the renderer culls by, and their normals when the file has
 * them or they were estimated at import. Placed in a level with a PGS (Plain Gaussian Splatting) actor or
 * component, which says how it is drawn (exposure, grade, lighting).
 *
 * One GPU copy serves every component that shows it.
 *
 * It also keeps its shadow proxy, made at import: a hidden mesh of where the splats are
 * opaque, which components cast shadows from (and can collide with) instead of the splats.
 */
UCLASS(BlueprintType, hidecategories = Object)
class PGSRUNTIME_API UPGSSplat : public UObject
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
	EPGSGpuLayout GpuLayout = EPGSGpuLayout::Float;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Normals")
	EPGSNormals Normals = EPGSNormals::None;

	/** How the normals came to be, in words. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Normals", meta = (DisplayName = "How"))
	FString NormalsNote;

	/** Optional imported or generated collision mesh, stored inside this asset's package.
	 * Components use it only when Proxy Collision is enabled; it never casts shadows.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Collision Proxy", meta = (DisplayName = "Mesh"))
	TObjectPtr<UStaticMesh> ShadowProxy;

	/** How the proxy was made, in words. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Collision Proxy", meta = (DisplayName = "How"))
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
	EPGSNormalMethod NormalMethod = EPGSNormalMethod::Fine;

	/** Shortest axes: the nearest splats a round or stray splat takes its neighbours' direction from. */
	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (ClampMin = "4", ClampMax = "64", EditCondition = "bEstimateNormals && NormalMethod == EPGSNormalMethod::ShortestAxes"))
	int32 NormalNeighbours = 16;

	/** Shortest axes: ... no farther than this share of the splats' size (their bounds' diagonal). */
	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (ClampMin = "0.05", ClampMax = "10", Units = "Percent", EditCondition = "bEstimateNormals && NormalMethod == EPGSNormalMethod::ShortestAxes"))
	float NormalRadiusPercent = 1.0f;

	/** Make the collision proxy mesh (reimport does the same). The sizes below are for a PLY in metres. */
	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (DisplayName = "Make Collision Proxy"))
	bool bMakeShadowProxy = true;

	/** Whether the source PGS supplies its own authored shadow/collision mesh. */
	UPROPERTY(VisibleAnywhere, Category = "Import Settings", meta = (DisplayName = "Embedded PGS Proxy"))
	bool bHasEmbeddedProxy = false;

	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (DisplayName = "Proxy Voxel Size", ClampMin = "0.1", ClampMax = "20", Units = "cm", EditCondition = "bMakeShadowProxy && !bHasEmbeddedProxy"))
	float ProxyVoxelSize = 1.0f;

	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (DisplayName = "Proxy Opacity", ClampMin = "0.01", ClampMax = "0.99", EditCondition = "bMakeShadowProxy && !bHasEmbeddedProxy"))
	float ProxyOpacity = 0.6f;

	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (DisplayName = "Proxy Inset", ClampMin = "0", ClampMax = "20", Units = "cm", EditCondition = "bMakeShadowProxy && !bHasEmbeddedProxy"))
	float ProxyInset = 4.0f;

	UPROPERTY(EditAnywhere, Category = "Import Settings", meta = (DisplayName = "Proxy Triangles", ClampMin = "500", EditCondition = "bMakeShadowProxy && !bHasEmbeddedProxy"))
	int32 ProxyTriangles = 20000;
#endif

	/** The splats, or null for an asset with none. */
	TSharedPtr<const FPGSSplatData, ESPMode::ThreadSafe> GetData() const { return Data; }

	/** The GPU copy every component showing this asset shares; made on first use, remade when the data or layout changes. */
	TSharedPtr<FPGSSplatGpu, ESPMode::ThreadSafe> GetGpu();

	/** Changed data or layout: components ask for the GPU copy again. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnChanged, UPGSSplat*);
	static FOnChanged OnChanged;

	void SetData(TSharedPtr<const FPGSSplatData, ESPMode::ThreadSafe> InData);

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
	TSharedPtr<const FPGSSplatData, ESPMode::ThreadSafe> Data;
	TSharedPtr<FPGSSplatGpu, ESPMode::ThreadSafe> Gpu;
};
