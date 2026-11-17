// VGS: not Epic's. The project's settings for the splat renderer (Project Settings > Plugins),
// written to the console variables the renderer reads.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "VGSSplatSettings.generated.h"

/**
 * How splats cast shadows (r.VGSSplat.Shadow.Method, the same numbers). Every light lights a
 * Shaded actor whichever this is, and every mesh's shadow falls on it; this is only about the
 * shadows the splats cast.
 */
UENUM(BlueprintType)
enum class EVGSSplatShadowMethod : uint8
{
	/**
	 * Recommended. The splats cast shadows from the main directional light only (the sun: the
	 * one marked Atmosphere Sun Light, otherwise the brightest that casts shadows). They are
	 * drawn from it into a shadow map of their own, as soft Gaussians with their real opacity,
	 * and cumulative transmittance is sampled at the receiver's depth by a Light Function
	 * before UE evaluates the sun. Select Mixed to also cast from spot, point and rect lights.
	 * Existing user light functions use native casters as fallback.
	 */
	SplatShadowMap = 0 UMETA(DisplayName = "Splat Shadow Map (sun only)"),

	/**
	 * The sun as Splat Shadow Map (clean shadows), and every other light that casts shadows
	 * (spot, point, rect, other directional lights) as Unreal Shadow Maps (grainy shadows). The
	 * cost of both representations depends on the grid, splat count and other lights.
	 */
	Mixed = 1 UMETA(DisplayName = "Splat Shadow Map (sun) + Unreal Shadow Maps (other lights)"),

	/**
	 * The splats go into Unreal's own shadow maps (virtual or cascaded) as small opaque shapes,
	 * each at a random opacity: shadows from every light that casts them, the sun included, but
	 * grainy and frayed, and Shaded characters shadow themselves in blotches (each actor's
	 * Shadow Inset reduces them). SGS uses splats too; approximate meshes are used only for collision.
	 */
	UnrealShadowMaps = 2 UMETA(DisplayName = "Unreal Shadow Maps (all lights)"),

	/** No splat casts shadows, even with Cast Shadow enabled. Receiving light is controlled by Lighting on the actor. */
	None = 3 UMETA(DisplayName = "No Shadows"),
};

UCLASS(config = Engine, defaultconfig, BlueprintType)
class UVGSSplatSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** How splats cast shadows, for every actor of the plugin in the project. */
	UPROPERTY(config, EditAnywhere, Category = "Shadows")
	EVGSSplatShadowMethod ShadowMethod = EVGSSplatShadowMethod::SplatShadowMap;

	/**
	 * In the sun's shadow from the splats' own shadow map, Shaded actors also shadow themselves:
	 * an arm on the body, one person on another. Off excludes visible Shaded pixels of this
	 * renderer; ordinary UE receivers still get depth-resolved shadows. Self-illuminated actors
	 * are composited later and do not receive this lighting.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Shadows",
		meta = (DisplayName = "Self Shadowing", EditCondition = "ShadowMethod == EVGSSplatShadowMethod::SplatShadowMap || ShadowMethod == EVGSSplatShadowMethod::Mixed"))
	bool bSelfShadow = false;

	/**
	 * The size of a texel of the splats' shadow map, which covers every casting actor (up to
	 * 1024 texels per spatial side, coarser beyond). The grid has 64 depth bins. Smaller spatial
	 * texels cost more memory and fill; at 512 spatial texels its main resources use about 101 MiB.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Shadows",
		meta = (DisplayName = "Shadow Texel Size", Units = "cm", ClampMin = "0.05", UIMin = "0.25", UIMax = "2.0",
			EditCondition = "ShadowMethod == EVGSSplatShadowMethod::SplatShadowMap || ShadowMethod == EVGSSplatShadowMethod::Mixed"))
	float ShadowTexelSize = 0.5f;

	/** The penumbra, as a multiple of what the light's Source Angle gives: 0 is a hard edge. */
	UPROPERTY(config, EditAnywhere, Category = "Shadows",
		meta = (DisplayName = "Shadow Softness", ClampMin = "0.0", UIMax = "4.0",
			EditCondition = "ShadowMethod == EVGSSplatShadowMethod::SplatShadowMap || ShadowMethod == EVGSSplatShadowMethod::Mixed"))
	float ShadowSoftness = 1.0f;

	/**
	 * How far behind the splats a surface must be to be shadowed. Raise it if the ground right
	 * under an actor shows dark specks; lower it if the shadow does not reach the feet.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Shadows",
		meta = (DisplayName = "Shadow Bias", Units = "cm", ClampMin = "0.0", UIMax = "20.0",
			EditCondition = "ShadowMethod == EVGSSplatShadowMethod::SplatShadowMap || ShadowMethod == EVGSSplatShadowMethod::Mixed"))
	float ShadowBias = 3.0f;

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual void PostInitProperties() override;
#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** Sets the console variables (as project settings: the console still overrides them). */
	void ApplyToConsoleVariables() const;
};
