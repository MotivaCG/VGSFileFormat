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
UENUM()
enum class EVGSSplatShadowMethod : uint8
{
	/**
	 * Recommended. The splats cast shadows from the main directional light only (the sun: the
	 * one marked Atmosphere Sun Light, otherwise the brightest that casts shadows). They are
	 * drawn from it into a shadow map of their own, as soft Gaussians with their real opacity,
	 * and that light is taken off what lies behind them: solid, clean shadows with a penumbra,
	 * and no blotches on the characters. Spot, point and rect lights get no shadow from the
	 * splats. About 0.6 to 1 ms for 300,000 splats, whatever the screen resolution.
	 */
	SplatShadowMap = 0 UMETA(DisplayName = "Splat Shadow Map (sun only)"),

	/**
	 * The sun as Splat Shadow Map (clean shadows), and every other light that casts shadows
	 * (spot, point, rect, other directional lights) as Unreal Shadow Maps (grainy shadows). The
	 * cost of both: about 0.6 to 1 ms for 300,000 splats, plus about 0.4 ms per other light.
	 */
	Mixed = 1 UMETA(DisplayName = "Splat Shadow Map (sun) + Unreal Shadow Maps (other lights)"),

	/**
	 * The splats go into Unreal's own shadow maps (virtual or cascaded) as small opaque shapes,
	 * each at a random opacity: shadows from every light that casts them, the sun included, but
	 * grainy and frayed, and Shaded characters shadow themselves in blotches (each actor's
	 * Shadow Inset reduces them). About 0.4 ms for 300,000 splats per light.
	 */
	UnrealShadowMaps = 2 UMETA(DisplayName = "Unreal Shadow Maps (all lights)"),

	/** The splats cast no shadows at all, whatever each actor's Cast Shadow says. Costs nothing. */
	None = 3 UMETA(DisplayName = "No Shadows"),
};

UCLASS(config = Engine, defaultconfig)
class UVGSSplatSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** How splats cast shadows, for every actor of the plugin in the project. */
	UPROPERTY(config, EditAnywhere, Category = "Shadows")
	EVGSSplatShadowMethod ShadowMethod = EVGSSplatShadowMethod::SplatShadowMap;

	/**
	 * In the sun's shadow from the splats' own shadow map, Shaded actors also shadow themselves:
	 * an arm on the body, one person on another. Off, only what lies behind them is shadowed,
	 * and each keeps the shading its normals give it. Self-illuminated actors are never darkened.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Shadows",
		meta = (DisplayName = "Self Shadowing", EditCondition = "ShadowMethod == EVGSSplatShadowMethod::SplatShadowMap || ShadowMethod == EVGSSplatShadowMethod::Mixed"))
	bool bSelfShadow = false;

	/**
	 * The size of a texel of the splats' shadow map, which covers every casting actor (up to
	 * 2048 texels a side, coarser beyond). Smaller is sharper and costs more: for 300,000 splats
	 * about 1 ms at 0.5 cm, 0.6 ms at 1 cm.
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
	UPROPERTY(config, EditAnywhere, Category = "Shadows", AdvancedDisplay,
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
