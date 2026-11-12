#pragma once

#include "Components/PrimitiveComponent.h"
#include "CoreMinimal.h"

#include "VGSStaticSplatComponent.generated.h"

class UStaticMeshComponent;
class UVGSStaticSplat;
class FVGSStaticSplatSource;

/** Which axis of the PLY points up. */
UENUM(BlueprintType)
enum class EVGSStaticUpAxis : uint8
{
	/** Right handed, Y up, as ScanMeNow and THE4DSCANNER write them. Turned as Epic's importer turns SPZ files. */
	Y UMETA(DisplayName = "Y Up"),
	/** Right handed, Y down and Z forward, as COLMAP and many training runs leave them. */
	YDown UMETA(DisplayName = "Y Down (COLMAP)"),
	/** Right handed, Z up, as Blender. */
	Z UMETA(DisplayName = "Z Up"),
};

/** Whether the scene's lights reach the splats. */
UENUM(BlueprintType)
enum class EVGSStaticLighting : uint8
{
	/** The splats show their own colours, as captured; lights and shadows do not touch them. */
	SelfIllumination UMETA(DisplayName = "Self-illumination"),
	/**
	 * Lit by the scene as an opaque surface: the colours become the base colour of a Default
	 * Lit material, drawn into Unreal's GBuffer, so lights, shadows cast on it, the skylight,
	 * Lumen and fog all apply. Edges and hair are dithered, which temporal anti-aliasing
	 * smooths. The capture already has its studio's light in its colours, so it shows lit
	 * twice; Self-illumination Amount keeps part of it glowing. Needs the deferred renderer
	 * (with or without Substrate); elsewhere it stays self-illuminated.
	 */
	Shaded UMETA(DisplayName = "Shaded"),
};

/** What casts the actor's shadows. */
UENUM(BlueprintType)
enum class EVGSStaticShadowCaster : uint8
{
	/**
	 * The asset's shadow proxy, a hidden mesh of where the splats are opaque made at import:
	 * shadows from every light (sun, spot, point, rect), solid and clean, as any mesh casts
	 * them, at no cost per splat. Shaded, the character also shadows itself (an arm on the
	 * body), its edges a little grainy where the splats and the proxy part. When the asset has
	 * no proxy, the splats cast as below.
	 */
	ProxyMesh UMETA(DisplayName = "Proxy Mesh"),
	/**
	 * The splats themselves, as the project's Shadow Method says (Project Settings > Plugins >
	 * VGSStatic): by default softly, from the sun only, without self-shadowing.
	 */
	Splats UMETA(DisplayName = "Splats (project's Shadow Method)"),
};

/** How splats are blended over each other: the way they were trained. */
UENUM(BlueprintType)
enum class EVGSStaticBlendSpace : uint8
{
	/** In sRGB values, as most 3D Gaussian splatting training does and as the VGS viewer draws. */
	SRGB UMETA(DisplayName = "sRGB (as trained)"),
	/** In linear light, as Epic's renderer does, for splats trained that way. */
	Linear,
};

/**
 * Shows a static Gaussian splat asset (a PLY imported as a VGS Static Splat) through a copy of
 * Epic's Gaussian Splat renderer, with the VGS capture's controls: exposure, a grade, and
 * lighting by the scene through the asset's normals.
 *
 * Nothing changes from frame to frame: the asset is on the GPU once for every component that
 * shows it, and the renderer culls its clusters against each view. A tick only notices
 * settings changed from Blueprint, and costs nothing when they have not.
 *
 * Its shadows (Lighting > Cast Shadow) come from the asset's shadow proxy, a hidden mesh
 * attached to it, by default (Shadow Caster); otherwise from the splats, as the project's
 * Shadow Method says (Project Settings > Plugins > VGSStatic): into the splat renderer's own
 * shadow map of the directional light, or into Unreal's shadow depth passes as small opaque
 * shapes, from the same GPU copy. It is a primitive for the latter.
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent, DisplayName = "VGS Static Splat"),
	HideCategories = (Mobility, Physics, Collision, LOD, Cooking, Navigation, HLOD, Mobile, RayTracing, VirtualTexture, TextureStreaming, MaterialParameters, PathTracing))
class VGSSTATICRUNTIME_API UVGSStaticSplatComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UVGSStaticSplatComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VGS Static")
	TObjectPtr<UVGSStaticSplat> Splat;

	/** View-dependent colour. Off, every splat shows its base colour from every side. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Splats")
	bool bSphericalHarmonics = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Splats")
	EVGSStaticUpAxis UpAxis = EVGSStaticUpAxis::Y;

	/** Unreal units per PLY unit: PLYs are usually in metres, Unreal in centimetres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Splats", AdvancedDisplay, meta = (ClampMin = "0.0001"))
	float UnitScale = 100.0f;

	/** Scales every splat about its centre. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Rendering", meta = (ClampMin = "0.0", UIMax = "2.0"))
	float PointScaleFactor = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Rendering", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OpacityScale = 1.0f;

	/**
	 * Brightness in stops: each +1 doubles the light the splats give off. Splats are an
	 * emissive of fixed brightness, so a scene exposed for physical light units (a sun of
	 * 100,000 lux meters around EV100 14) shows them nearly black. Set this near the scene's
	 * EV100 to see them as they look at EV100 0.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Rendering", meta = (DisplayName = "Intensity (EV)", UIMin = "-4.0", UIMax = "18.0"))
	float IntensityEV = 0.0f;

	/** How splats blend over each other: as they were trained. Most are trained in sRGB. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Rendering")
	EVGSStaticBlendSpace BlendSpace = EVGSStaticBlendSpace::SRGB;

	/** Self-illumination shows the splats as captured; Shaded lets the scene's lights reach them, by their normals when the asset has them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Lighting")
	EVGSStaticLighting Lighting = EVGSStaticLighting::SelfIllumination;

	/** How much of the colour still glows when Shaded: 0 is lit only by the scene, 1 as bright as self-illuminated plus the lights. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Lighting",
		meta = (DisplayName = "Self-illumination Amount", ClampMin = "0.0", ClampMax = "1.0", EditCondition = "Lighting == EVGSStaticLighting::Shaded", EditConditionHides))
	float SelfIlluminationAmount = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Lighting",
		meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "Lighting == EVGSStaticLighting::Shaded", EditConditionHides))
	float Roughness = 0.8f;

	/** Unreal's Specular: 0.5 is the usual dielectric, 0 no highlights at all. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Lighting",
		meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "Lighting == EVGSStaticLighting::Shaded", EditConditionHides))
	float Specular = 0.3f;

	/**
	 * Averages the normal over this distance (cm) on screen, among neighbours at the same depth:
	 * a splat's own normal is noisy, and lit directly the splats look crumpled and specked.
	 * 0 lights each pixel by its splats' normal alone.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Lighting",
		meta = (ClampMin = "0.0", UIMax = "10.0", Units = "cm", EditCondition = "Lighting == EVGSStaticLighting::Shaded", EditConditionHides))
	float NormalSmoothing = 2.0f;

	/**
	 * What casts the actor's shadows: the asset's shadow proxy mesh (every light), or the splats
	 * (as the project's Shadow Method says). Cast Shadow, under Lighting, turns them off.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Lighting")
	EVGSStaticShadowCaster ShadowCaster = EVGSStaticShadowCaster::ProxyMesh;

	/**
	 * Only when the splats cast with the project's Shadow Method "Unreal Shadow Maps" (Project
	 * Settings > Plugins > VGSStatic): how far (cm) each splat's shadow caster is moved inwards,
	 * along its normal. The lit
	 * surface lies among its own splats, and without this would shadow itself in blotches
	 * when Shaded; more than needed thins the shadows of thin parts (fingers, hair). Without
	 * normals with a side, "inwards" is away from the asset's centre.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Lighting", meta = (ClampMin = "0.0", UIMax = "10.0", Units = "cm"))
	float ShadowInset = 3.0f;

	/**
	 * Blocks everything (BlockAll) with the asset's shadow proxy mesh, as a mesh with its own
	 * triangles as collision: characters walk around it, traces hit it. Off, nothing collides.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Collision", meta = (DisplayName = "Proxy Collision"))
	bool bProxyCollision = false;

	// Grade of the colours, as in an image editor: on their display (sRGB) values, after the
	// splats are blended and before Intensity and the tone curve. 1 leaves them as captured.
	/** Midtones: above 1 brightens them, below 1 darkens them; black and white stay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Rendering|Grade", meta = (ClampMin = "0.05", UIMin = "0.25", UIMax = "4.0"))
	float Gamma = 1.0f;

	/** About mid grey (0.5 in sRGB values): above 1 spreads the tones, below 1 flattens them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Rendering|Grade", meta = (ClampMin = "0.0", UIMax = "3.0"))
	float Contrast = 1.0f;

	/** 0 is grey, 1 as captured, above 1 more saturated. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Rendering|Grade", meta = (ClampMin = "0.0", UIMax = "3.0"))
	float Saturation = 1.0f;

	/** Multiplies the colours, after the rest of the grade: white leaves them, Saturation 0 and a tint tones them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Rendering|Grade", meta = (HideAlphaChannel))
	FLinearColor Tint = FLinearColor::White;

	/**
	 * Show the colours as captured, as the VGS viewer does, by undoing Unreal's filmic tone
	 * curve on the splats (Epic's inverse LUT, applied when they are composited). Off, the
	 * tone curve applies to them as to the rest of the scene.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Rendering", meta = (DisplayName = "Invert UE Tone Curve"))
	bool bInvertToneCurve = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Rendering", AdvancedDisplay,
		meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bInvertToneCurve", EditConditionHides))
	float InverseToneCurveStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS Static|Rendering", AdvancedDisplay,
		meta = (DisplayName = "Invert Default Expand Gamut", EditCondition = "bInvertToneCurve", EditConditionHides))
	bool bInvertExpandGamut = true;

	// ---- what the asset says about itself ---------------------------------------------------

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "VGS Static|Asset")
	int64 Splats = 0;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "VGS Static|Asset", meta = (DisplayName = "SH Degree"))
	int32 SHDegree = 0;

	/** Whether the asset has normals for Shaded lighting, and where they come from. */
	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "VGS Static|Asset")
	FString Normals;

	UFUNCTION(BlueprintCallable, Category = "VGS Static")
	void SetSplat(UVGSStaticSplat* NewSplat);

	/** From a PLY's axes and units to a component's, for an Up Axis and Unit Scale. */
	static FMatrix FileToComponent(EVGSStaticUpAxis InUpAxis, float InUnitScale);

	/**
	 * Where the asset's shadow proxy goes in this component: the proxy is made for the default
	 * Up Axis and Unit Scale (Y up, 100), and turned and scaled for others.
	 */
	FTransform ProxyToComponent() const;

	//~ Begin UActorComponent / USceneComponent
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void BeginDestroy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~ End UActorComponent / USceneComponent / UPrimitiveComponent

protected:
	virtual void OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport) override;
	virtual void OnVisibilityChanged() override;
	virtual void OnHiddenInGameChanged() override;

private:
	FMatrix FileToComponent() const;
	/** Whether the shadow proxy casts this actor's shadows (and the splats do not). */
	bool ProxyCastsShadows() const;
	/** Makes, sets up or removes the hidden mesh component that shows the asset's proxy to Unreal. */
	void UpdateProxyComponent();
	void RefreshInfo();
	void PushRenderState(bool bForce);
	void RegisterSource();
	void ReleaseSource();
	void OnSplatChanged(UVGSStaticSplat* Changed);

	TSharedPtr<FVGSStaticSplatSource, ESPMode::ThreadSafe> Source;
	FDelegateHandle ChangedHandle;
	/** What was last sent to the render thread, so a tick sends only changes. */
	TArray<uint8> SentState;
	/** What the scene proxy (the shadow casters) was made with: it is made again when this changes. */
	TArray<uint8> ProxyState;
	/** Whether the scene proxy was made with the shadow proxy casting instead of the splats. */
	bool bProxyStateUsesMesh = false;

	/** The asset's shadow proxy in the scene: casts shadows, collides when asked, never drawn. Not saved. */
	UPROPERTY(Transient, DuplicateTransient)
	TObjectPtr<UStaticMeshComponent> ProxyComponent;
};
