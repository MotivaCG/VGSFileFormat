#pragma once

#include "Components/SceneComponent.h"
#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"

#include "VGSCaptureComponent.generated.h"

namespace VGS { class FPlayer; class FPackedChunk; }
class FVGSSplatSource;
struct FVGSPackedFrame;
struct vgsb_frame;
struct vgsb_packed_frame;

/** How the capture's time runs past its ends. */
UENUM(BlueprintType)
enum class EVGSLoopMode : uint8
{
	/** The playback mode the capture was authored with. */
	FromCapture UMETA(DisplayName = "From Capture"),
	/** Stop on the last frame. */
	Once,
	Loop,
	/** There and back: each end is shown once per turn. */
	PingPong UMETA(DisplayName = "Ping-Pong"),
};

/** Which axis of the capture points up. */
UENUM(BlueprintType)
enum class EVGSUpAxis : uint8
{
	/** How captures are written: right handed, Y up. Turned as Epic's importer turns SPZ files. */
	Y UMETA(DisplayName = "Y Up"),
	/** Right handed, Z up, as Blender. */
	Z UMETA(DisplayName = "Z Up"),
};

/** Whether the scene's lights reach the capture. */
UENUM(BlueprintType)
enum class EVGSLighting : uint8
{
	/** The capture shows its own colours, as captured; lights and shadows do not touch it. */
	SelfIllumination UMETA(DisplayName = "Self-illumination"),
	/**
	 * Lit by the scene as an opaque surface: its colours become the base colour of a Default
	 * Lit material, drawn into Unreal's GBuffer, so lights, shadows cast on it, the skylight,
	 * Lumen and fog all apply. Edges and hair are dithered, which temporal anti-aliasing
	 * smooths. The capture already has its studio's light in its colours, so it shows lit
	 * twice; Self-illumination Amount keeps part of it glowing. Needs the deferred renderer
	 * without Substrate; elsewhere the capture stays self-illuminated.
	 */
	Shaded UMETA(DisplayName = "Shaded"),
};

/** How splats are blended over each other: the way the capture was trained. */
UENUM(BlueprintType)
enum class EVGSBlendSpace : uint8
{
	/**
	 * In sRGB values, as most 3D Gaussian splatting training does and as the VGS viewer
	 * draws. The blended result is made linear when it is composited into the scene.
	 */
	SRGB UMETA(DisplayName = "sRGB (as trained)"),
	/**
	 * In linear light, as Epic's renderer does, for captures trained that way. Used on an
	 * sRGB-trained capture, faint splats show far more than they should: coloured haze
	 * around hair and edges.
	 */
	Linear,
};

/** Where each instant of the capture is worked out. */
UENUM(BlueprintType)
enum class EVGSEvaluateOn : uint8
{
	/**
	 * The GPU: the CPU only decompresses each chunk of the capture, about once a second,
	 * and a compute shader works out every frame from it. A tenth of the CPU time and a
	 * fraction of the upload; what lets many captures play at once.
	 */
	GPU,
	/** The CPU, which then uploads every splat of every frame: 20 ms or more a frame. */
	CPU,
};

/** The layout each decoded instant is uploaded to the GPU in, when the CPU evaluates it. */
UENUM(BlueprintType)
enum class EVGSGpuLayout : uint8
{
	/** Every value as decoded, 56 bytes a splat plus 12 per harmonic coefficient. */
	Float,
	/**
	 * Epic's compressed layout: about a third of the upload, a little more work per frame,
	 * and rotations a few degrees off on the flattest splats.
	 */
	Quantized,
};

/**
 * Plays a VGS capture through Epic's Gaussian Splat renderer.
 *
 * The capture streams from its .vgs file: vgsbridge decodes it on threads of its own, ahead
 * of the time this component will ask for, and every new instant is uploaded to the GPU and
 * handed to the renderer as one more source of splats. The splats are drawn as the
 * renderer draws any splat: lit by nothing, casting no shadow, composited before depth of
 * field (see plugins/unreal/README.md).
 *
 * Time runs with the world's (so it pauses and dilates with it), snapped to the capture's
 * own frames, which is what lets the bridge have each frame decoded before it is due.
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent, DisplayName = "VGS Capture"),
	HideCategories = (Mobility, Physics, Collision, Lighting, LOD, Cooking, Navigation))
class VGSRUNTIME_API UVGSCaptureComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UVGSCaptureComponent();

	/** The capture. A relative path is looked for under the project's Content folder, then the project folder. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VGS", meta = (FilePathFilter = "vgs"))
	FFilePath File;

	/** Start playing when the game begins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Playback")
	bool bAutoPlay = true;

	/** Play in the editor's viewports too, outside Play In Editor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Playback")
	bool bPlayInEditor = false;

	/** 1 is the capture's own speed; negative plays it backwards. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Playback", meta = (UIMin = "-2.0", UIMax = "2.0"))
	float PlayRate = 1.0f;

	/** Where in the capture playback starts, as a fraction of its duration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Playback", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Phase = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Playback")
	EVGSLoopMode LoopMode = EVGSLoopMode::FromCapture;

	/**
	 * How long a game frame may wait for a capture frame that is not decoded yet, in
	 * milliseconds. Past it the previous frame stays on screen and the next tick asks again.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Playback", AdvancedDisplay, meta = (ClampMin = "0.0", UIMax = "50.0"))
	float MaxWaitMs = 10.0f;

	/** View-dependent colour. Most of what a frame costs to decode and upload. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Splats")
	bool bSphericalHarmonics = true;

	/**
	 * The fraction of splats drawn, for a lighter scene. Which ones stay holds still within a
	 * chunk of the capture, and those kept are made more opaque so it stays solid.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Splats", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float Density = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Splats")
	EVGSUpAxis UpAxis = EVGSUpAxis::Y;

	/** Unreal units per capture unit: captures are in metres, Unreal in centimetres. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Splats", AdvancedDisplay, meta = (ClampMin = "0.0001"))
	float UnitScale = 100.0f;

	/** Where each instant is worked out. Changing it reopens the capture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Splats", AdvancedDisplay)
	EVGSEvaluateOn EvaluateOn = EVGSEvaluateOn::GPU;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Splats", AdvancedDisplay,
		meta = (EditCondition = "EvaluateOn == EVGSEvaluateOn::CPU", EditConditionHides))
	EVGSGpuLayout GpuLayout = EVGSGpuLayout::Float;

	/** Scales every splat about its centre. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Rendering", meta = (ClampMin = "0.0", UIMax = "2.0"))
	float PointScaleFactor = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Rendering", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OpacityScale = 1.0f;

	/**
	 * Brightness in stops: each +1 doubles the light the splats give off. Splats are an
	 * emissive of fixed brightness and do not respond to lights, so a scene exposed for
	 * physical light units (a sun of 100,000 lux meters around EV100 14) shows them nearly
	 * black. Set this near the scene's EV100 to see the capture as it looks at EV100 0.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Rendering", meta = (DisplayName = "Intensity (EV)", UIMin = "-4.0", UIMax = "18.0"))
	float IntensityEV = 0.0f;

	/** How splats blend over each other: as the capture was trained. Most are trained in sRGB. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Rendering")
	EVGSBlendSpace BlendSpace = EVGSBlendSpace::SRGB;

	/** Self-illumination shows the capture as captured; Shaded lets the scene's lights reach it, by its normals when the capture has them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Lighting")
	EVGSLighting Lighting = EVGSLighting::SelfIllumination;

	/** How much of the colour still glows when Shaded: 0 is lit only by the scene, 1 as bright as self-illuminated plus the lights. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Lighting",
		meta = (DisplayName = "Self-illumination Amount", ClampMin = "0.0", ClampMax = "1.0", EditCondition = "Lighting == EVGSLighting::Shaded", EditConditionHides))
	float SelfIlluminationAmount = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Lighting",
		meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "Lighting == EVGSLighting::Shaded", EditConditionHides))
	float Roughness = 0.8f;

	/** Unreal's Specular: 0.5 is the usual dielectric, 0 no highlights at all. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Lighting",
		meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "Lighting == EVGSLighting::Shaded", EditConditionHides))
	float Specular = 0.3f;

	/**
	 * Averages the normal over this distance (cm) on screen, among neighbours at the same depth:
	 * a splat's own normal is noisy, and lit directly the capture looks crumpled and specked.
	 * 0 lights each pixel by its splats' normal alone.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Lighting",
		meta = (ClampMin = "0.0", UIMax = "10.0", Units = "cm", EditCondition = "Lighting == EVGSLighting::Shaded", EditConditionHides))
	float NormalSmoothing = 2.0f;

	// Grade of the capture's colours, as in an image editor: on its display (sRGB) values,
	// after the splats are blended and before Intensity and the tone curve. 1 leaves them as
	// captured. A few operations per pixel in the composite, which runs anyway.
	/** Midtones: above 1 brightens them, below 1 darkens them; black and white stay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Rendering|Grade", meta = (ClampMin = "0.05", UIMin = "0.25", UIMax = "4.0"))
	float Gamma = 1.0f;

	/** About mid grey (0.5 in sRGB values): above 1 spreads the tones, below 1 flattens them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Rendering|Grade", meta = (ClampMin = "0.0", UIMax = "3.0"))
	float Contrast = 1.0f;

	/** 0 is grey, 1 as captured, above 1 more saturated. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Rendering|Grade", meta = (ClampMin = "0.0", UIMax = "3.0"))
	float Saturation = 1.0f;

	/** Multiplies the colours, after the rest of the grade: white leaves them, Saturation 0 and a tint tones them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Rendering|Grade", meta = (HideAlphaChannel))
	FLinearColor Tint = FLinearColor::White;

	/**
	 * Show the capture's colours as captured, as the VGS viewer does, by undoing Unreal's
	 * filmic tone curve on the splats (Epic's inverse LUT, applied when they are composited).
	 * Off, the tone curve applies to them as to the rest of the scene: more contrast, crushed
	 * darks and warmer skin.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Rendering", meta = (DisplayName = "Invert UE Tone Curve"))
	bool bInvertToneCurve = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Rendering", AdvancedDisplay,
		meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bInvertToneCurve", EditConditionHides))
	float InverseToneCurveStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VGS|Rendering", AdvancedDisplay,
		meta = (DisplayName = "Invert Default Expand Gamut", EditCondition = "bInvertToneCurve", EditConditionHides))
	bool bInvertExpandGamut = true;

	// ---- what the capture says about itself --------------------------------------------

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "VGS|Capture")
	FString Status;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "VGS|Capture")
	FString Title;

	/** Seconds from the first frame to the last. */
	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "VGS|Capture")
	float Duration = 0.0f;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "VGS|Capture")
	float FrameRate = 0.0f;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "VGS|Capture")
	int64 FrameCount = 0;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "VGS|Capture")
	int64 MaxSplats = 0;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "VGS|Capture")
	int32 SHDegree = 0;

	/** Splats in the instant on screen. */
	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "VGS|Capture")
	int64 ShownSplats = 0;

	// ---- control -------------------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "VGS")
	void Play();

	UFUNCTION(BlueprintCallable, Category = "VGS")
	void Pause();

	UFUNCTION(BlueprintPure, Category = "VGS")
	bool IsPlaying() const { return bPlaying; }

	/** Seconds of playback since the start, before phase and looping. */
	UFUNCTION(BlueprintCallable, Category = "VGS")
	void SetPlaybackTime(float Seconds);

	UFUNCTION(BlueprintPure, Category = "VGS")
	float GetPlaybackTime() const { return float(Elapsed); }

	/** The capture's own time being shown, after phase, speed, looping and snapping to its frames. */
	UFUNCTION(BlueprintPure, Category = "VGS")
	float GetCaptureTime() const { return float(ShownSeconds); }

	/** Closes the capture and opens it again on the next tick. */
	UFUNCTION(BlueprintCallable, Category = "VGS")
	void Reload();

	//~ Begin UActorComponent / USceneComponent
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
	virtual void BeginDestroy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~ End UActorComponent / USceneComponent

protected:
	virtual void OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport) override;

private:
	bool EnsurePlayer();
	void ClosePlayer(const FString& Why);
	FString ResolvePath() const;
	bool ShouldAdvance() const;

	/** The capture's time at a playback time: phase, looping, then the nearest frame at or before it. */
	double CaptureSeconds(double PlaybackSeconds) const;
	EVGSLoopMode EffectiveLoopMode() const;

	void UpdateFrame(bool bAdvancing, double AdvancePerTick);
	void LogSummary(bool bAdvancing, float DeltaTime);
	void PackAndSend(const vgsb_frame& Frame);
	void SendPacked(const vgsb_packed_frame& Frame);
	bool WantsGpu() const;
	TSharedPtr<FVGSPackedFrame, ESPMode::ThreadSafe> TakeFrameBuffer();
	void PushRenderState();
	void RegisterSource();
	void ReleaseSource();

	FMatrix CaptureToComponent() const;

	TSharedPtr<VGS::FPlayer> Player;
	TSharedPtr<FVGSSplatSource, ESPMode::ThreadSafe> Source;

	/** Packed instants: the one on screen, and spares the next one is packed into once the render thread lets go. */
	TArray<TSharedPtr<FVGSPackedFrame, ESPMode::ThreadSafe>> FrameBuffers;
	TSharedPtr<FVGSPackedFrame, ESPMode::ThreadSafe> ShownFrame;

	/** Evaluating on the GPU: the chunk the frame on screen came from, kept for the render thread. */
	TSharedPtr<const VGS::FPackedChunk, ESPMode::ThreadSafe> ShownChunk;

	double Elapsed = 0.0;
	double ShownSeconds = -1.0;
	bool bPlaying = false;
	bool bFrameDirty = true;
	FString FailedPath; // a capture that failed to open is not retried every tick
	TArray<double> ScheduleScratch;
	TArray<double> LastSchedule;

	/** What playback cost since the last summary in the log. */
	struct FPlaybackSummary
	{
		double Start = 0.0;
		int32 Ticks = 0;
		double TickMs = 0.0;
		int32 Shown = 0;
		int32 Late = 0;
		double WaitMs = 0.0, WaitMaxMs = 0.0;
		double PackMs = 0.0, PackMaxMs = 0.0;
	};
	FPlaybackSummary Summary;

	/** What the instant on screen was packed with, so a change from Blueprint repacks it. */
	EVGSGpuLayout PackedLayout = EVGSGpuLayout::Float;
	float PackedDensity = 1.0f;
};
